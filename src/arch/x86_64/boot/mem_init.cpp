#include "memory/memory_base.h"
#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/init_memory_info.h"
#include "memory/AddresSpace.h"
#include "memory/phyaddr_accessor.h"
#include "memory/page_frame_state_mgr.h"
#include "memory/main_phyaddr_access_window.h"
#include "boot/asset_table.h"
#include "abi/asset_names.h"
#include "arch/x86_64/mem_init.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "util/kout.h"
#include "util/init_printk.h"   // 启动期日志（bsp_kout 接替者）
#include "elf.h"
#include "panic.h"
uint64_t VM_intervals_count;
phymem_segment *phymem_segments;
uint64_t phymem_segments_count; 
uint32_t logical_processor_count;
vm_interval Kspace_phyaddr_access_window;
vm_interval conjucnt_GSs;
phyaddr_t g_xsdt_base;

// ================================================================
// bfs_delete_old_pagetable — CR3 切换后 BFS 递归删除老页表（init.elf kmmu 根表）
//
// 背景：kernel.elf 切入自建新 PML4（gKernelSpace）后，init.elf 侧 kmmu 建立的
//       老页表（identity 映射 + kernel 四段 + 各 mem 资产映射）不再被引用，
//       需要整棵回收。老页表根表物理地址 = 切换前的 CR3（调用方记录传入）。
//
// 回收对象 = 页表页（PML4/PDPT/PD/PT 各级表页本身），NOT 叶映射指向的数据页
//          （kernel 段 / gs / hdstacks / 窗口等是资产，数据页归 FPA 管）。
// 走读方式 = 老页表页物理地址一律经主窗口 PHYACC_VA 读（新页表已含窗口映射，
//           窗口 [0, dram_top) 覆盖所有物理页表页，无需 identity 残留）。
// 回收动作 = page_frame_state_mgr::state_set(base,1,free)：页表页原由 init 侧
//           page_allocator_v2 以 kernel_pinned 记账，这里翻回 free 归还账本。
//
// BFS 队列：显式循环 + 逐层出队，深度不递归（页表 4 层，但页数可达数万，
//           递归爆栈；BFS 用堆上队列）。队列容量动态翻倍。
//
// ⚠ 层级必须随队列携带：PT（level 3）的全部 Present 项都是 4KB 叶数据页
//   （其 bit7 是 PAT 位，不是 PS），绝不能当子页表页入队。只有 PML4/PDPT/PD
//   的非大页项才指向下一级页表页。
//
// 约束：必须在切换 CR3 之后、但页面仍经窗口可读时调用（即 gKernelSpace 已映射
//       主窗口）；本函数不碰 TLB（新页表已 load，老表条目随 CR3 reload 失效）。
// ================================================================
static loc_code_t bfs_delete_old_pagetable(phyaddr_t old_root)
{
    if (old_root == 0) return SRC_LOC();
    constexpr uint64_t ENTRIES = 512;
    constexpr uint64_t P_BIT  = 1ULL << 0;   // Present
    constexpr uint64_t PS_BIT = 1ULL << 7;   // Page Size（PDPT 1GB / PD 2MB 大页）
    constexpr uint64_t ADDR_MASK = PHYS_ADDR_MASK & ~0xFFFull;  // 页表项物理地址字段

    // 队列条目：待处理页表页物理地址 + 层级（0=PML4,1=PDPT,2=PD,3=PT）
    struct qent_t { phyaddr_t pa; uint8_t level; };
    uint64_t q_cap  = 1024;
    uint64_t q_head = 0, q_tail = 0;
    qent_t* queue = new qent_t[q_cap];
    if (!queue) return SRC_LOC();

    auto queue_push = [&](phyaddr_t pa, uint8_t level) -> bool {
        if (q_tail == q_cap) {
            uint64_t ncap = q_cap * 2;
            qent_t* nq = new qent_t[ncap];
            if (!nq) return false;
            for (uint64_t i = 0; i < q_tail; i++) nq[i] = queue[i];
            delete[] queue;
            queue = nq;
            q_cap = ncap;
        }
        queue[q_tail++] = { pa, level };
        return true;
    };

    uint64_t freed_count = 0;
    bool     failed = false;

    if (!queue_push(old_root, 0)) failed = true;   // 根表（PML4）本身也是页表页

    while (!failed && q_head < q_tail) {
        const qent_t cur = queue[q_head++];
        const phyaddr_t  pa    = cur.pa;
        const uint8_t    level = cur.level;

        // ---- 走读本页表页全部条目，把指向"下一级页表页"的项入队 ----
        //   level 0（PML4）：Present → 指向 PDPT（无大页语义）
        //   level 1（PDPT）：Present && !PS → 指向 PD；PS=1 → 1GB 叶，跳过
        //   level 2（PD）  ：Present && !PS → 指向 PT；PS=1 → 2MB 叶，跳过
        //   level 3（PT）  ：全部是 4KB 叶数据页，无子页表页，绝不入队
        for (uint64_t i = 0; i < ENTRIES; i++) {
            const uint64_t raw = PHYACC_READU64(pa + i * sizeof(uint64_t));
            if (!(raw & P_BIT)) continue;
            if (level >= 3) break;                 // PT 层无子页表页
            if ((level == 1 || level == 2) && (raw & PS_BIT)) continue;  // 大页叶
            const uint64_t child = raw & ADDR_MASK;
            if (child == 0) continue;
            if (!queue_push(child, (uint8_t)(level + 1))) { failed = true; break; }
        }
        if (failed) break;

        // ---- 回收本级页表页本身：翻回 free 归还 page_frame_state_mgr ----
        if (page_frame_state_mgr::state_set(pa, 1, page_state_t::free) != 0) {
            init_printk("[bfs_delete_old_pagetable] state_set free fail pa=0x%lx",
                        (unsigned long)pa);
            failed = true;
            break;
        }
        freed_count++;
    }

    delete[] queue;
    if (failed) return SRC_LOC();
    init_printk("[bfs_delete_old_pagetable] freed %lu page-table pages (old root 0x%lx)",
                (unsigned long)freed_count, (unsigned long)old_root);
    return 0;
}
// ================================================================
// remap_hdstacks — hdstacks 资产精细重映射（镜像 init.elf phase_3b build_hdstacks 的精化版）
//
// 背景：phase_3b 只对 hdstacks 做了 KMMU 粗映射（含 guard 页全量 RW），那只是
//       过渡期映射。guard 页的"不映射"铁律由 kernel 侧精细映射阶段落实。
//
// 两步走：
//   1. kspace_vm_table 跑马圈地：把整段 VA 区间（nproc × stride + 尾 guard）登记为
//      一块领地（仅台账，不建页表），杜绝后续 VA 分配器（如 multi_heap_enable 的
//      alloc_available_space）在 guard 页位置再放任何映射——guard 必须永久缺页。
//   2. 内部精细映射：逐处理器逐栈（rsp0/ist1/ist2/ist3/idle_task）只对真实占用页
//      enable_VMentry，各栈间的 guard 页保持 not-present——栈溢出立即 #PF。
//      栈区间只有 32KB 且 4KB 对齐，enable_VMentry 内部按 4KB 粒度分块映射，
//      不会用 2MB/1GB 大页把 guard 页一起盖住。
// ================================================================
static KURD_t remap_hdstacks(const vm_interval& hd)
{
    // 业务代码 KURD 空占位（不碰模块错误树）
    auto placeholder_fail = []() -> KURD_t {
        KURD_t f;
        f.result      = result_code::FAIL;
        f.level       = level_code::ERROR;
        f.module_code = module_code::MEMORY;
        return f;
    };

    // 圈地 + 建页表必须整体持锁，保证"红黑树登记 + 页表修改"原子（同 direct_map 约定）
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    // ---- 1. 跑马圈地：整段 VA（含全部 guard）登记为一块领地 ----
    VM_DESC territory = {
        .start           = hd.vbase(),
        .end             = hd.vbase() + hd.byte_cnt(),
        .map_type        = VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start      = hd.pbase(),
        .access          = hd.access,
        .committed_full  = true,
        .is_vaddr_alloced = false,
    };
    if (kspace_vm_table->insert(territory) != OS_SUCCESS) return placeholder_fail();

    // ---- 2. 内部精细映射：逐处理器逐栈跳过 guard ----
    //      VA/PA 偏移相同（VA 与 PA 对 stride 基址线性同构）。
    const uint64_t stride = sizeof(per_processor_hardware_stack_t);
    auto map_stack = [&](uint64_t off, uint64_t sz) -> KURD_t {
        if (sz == 0) return KURD_t();
        vm_interval sub = {
            .vpn    = (hd.vbase() + off) >> 12,
            .ppn    = (hd.pbase() + off) >> 12,
            .npages = sz >> 12,
            .access = hd.access,
        };
        return KspacePageTable::enable_VMentry(sub);
    };

    for (uint32_t p = 0; p < logical_processor_count; p++) {
        const uint64_t off = p * stride;
        KURD_t k;
        k = map_stack(off + RSP0_BASE_OFF,            RSP0_STACKSIZE);      if (error_kurd(k)) return k;
        k = map_stack(off + IST1_BASE_OFF,            DF_STACKSIZE);        if (error_kurd(k)) return k;
        k = map_stack(off + IST2_BASE_OFF,            MC_STACKSIZE);        if (error_kurd(k)) return k;
        k = map_stack(off + IST3_BASE_OFF,            NMI_STACKSIZE);       if (error_kurd(k)) return k;
        k = map_stack(off + IDLE_TASK_STACK_BASE_OFF, IDLE_TASK_STACKSIZE); if (error_kurd(k)) return k;
    }
    return KURD_t();
}

// ================================================================
// load_low_half_segments — 低半区（非内核地址）段实际加载 + 映射
//
// 背景：phase_3a 只加载了 kernel.elf 的高半区四段（.text/.data/.rodata/.bss），
//       ap_bootstrap_*（kld.ld init 区，0x4000 起）低地址段只存在于 kimg 文件
//       缓冲里，从未落到其链接物理地址。AP 经 INIT-SIPI-SIPI 在物理 0x4000 处
//       实模式启动（SIPI vector = AP_realmode_start/4096），若内容不在位，
//       0x4000 处执行的是残留垃圾。故必须先把文件内容复制到对应物理内存。
//
// 步骤（逐 PT_LOAD）：
//   1. 读 kimg 资产（movable_file_entry_t = kernel.elf 文件镜像），经主窗口重链
//   2. 遍历全部 PT_LOAD，跳过已由 phase_3a 加载的内核地址段
//   3. 低半段：把 [p_offset, p_offset+p_filesz) 从文件复制到物理 p_paddr；
//      p_memsz 超出 p_filesz 的部分清零（.bss 语义）
//   4. enable_low_half_vm_interval 映射进新 PML4（va==pa，低半区恒等）
//
// 写物理内存一律经主窗口 PHYACC_VA（此时新页表已含 phyaddr_window 映射）。
// 约束：必须在 CR3 切到新 PML4、主窗口可用之后调用（本函数在 mem_init 末段调用）。
// ================================================================
static KURD_t load_low_half_segments()
{
    auto placeholder_fail = []() -> KURD_t {
        KURD_t f;
        f.result      = result_code::FAIL;
        f.level       = level_code::ERROR;
        f.module_code = module_code::MEMORY;
        return f;
    };

    const asset_table_entry* kimg = g_asset_table->read(asset_names::kimg);
    if (!kimg) return placeholder_fail();
    const movable_file_entry_t* kimg_file = (const movable_file_entry_t*)kimg->data;

    uint8_t* elf_b = (uint8_t*)PHYACC_VA(kimg_file->base_ppn << 12);
    Elf64_Ehdr* eh = (Elf64_Ehdr*)elf_b;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
        init_printk("[mem_init] kimg bad ELF magic");
        return placeholder_fail();
    }
    uint8_t* ptbl = elf_b + eh->e_phoff;
    init_printk("[mem_init] loading non-kernel (low-half) segments...");

    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        vaddr_t   va = ph->p_vaddr;
        phyaddr_t pa = ph->p_paddr;
        uint64_t  sz = align_up(ph->p_memsz, 4096);

        // 跳过已由 phase_3a 加载的内核地址段
        vm_interval seg = {.vpn = va >> 12, .ppn = pa >> 12,
                           .npages = sz >> 12, .access = {}};
        if (seg.is_kernel_address()) continue;

        // 越界防护：文件内容必须落在 kimg 文件缓冲内
        if (ph->p_offset + ph->p_filesz > kimg_file->size) {
            init_printk("[mem_init] non-kernel seg[%u] beyond kimg file", (unsigned)i);
            return placeholder_fail();
        }

        // ---- 实际加载：文件内容 → 链接物理地址（经主窗口） ----
        if (ph->p_filesz > 0) {
            ksystemramcpy(elf_b + ph->p_offset, (void*)PHYACC_VA(pa), ph->p_filesz);
        }
        if (ph->p_memsz > ph->p_filesz) {
            ksetmem_8((void*)PHYACC_VA(pa + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
        }

        // ---- 映射进新 PML4（低半区恒等） ----
        pgaccess acc = {1, (uint8_t)((ph->p_flags & PF_W) ? 1 : 0), 1,
                        (uint8_t)((ph->p_flags & PF_X) ? 1 : 0), 0, WB};
        vm_interval seg_map = {.vpn = va >> 12, .ppn = pa >> 12,
                               .npages = sz >> 12, .access = acc};
        KURD_t mk = gKernelSpace->enable_low_half_vm_interval(seg_map);
        if (error_kurd(mk)) {
            init_printk("[mem_init] non-kernel seg[%u] map fail", (unsigned)i);
            return mk;
        }
        init_printk("[mem_init] low-half seg[%u] v=0x%lx p=0x%lx filesz=0x%lx sz=0x%lx",
                    (unsigned)i, (unsigned long)va, (unsigned long)pa,
                    (unsigned long)ph->p_filesz, (unsigned long)sz);
    }
    return KURD_t();
}

KURD_t assets_remap(){
    // arg1=mem 型的统一重映射：全部 entry 只 read 不 build 不 deal（后续阶段自领）。
    // 已 deal 条目位图已清，read 自然跳过；非 mem 型（movable/scalar/gop...）归各自消费方。
    for (uint32_t i = 0; i < asset_names::all_count(); i++) {
        const char* name = asset_names::all[i];
        const asset_table_entry* e = g_asset_table->read(name);
        if (!e) continue;

        // 标量资产：非区间，匹配到即解析标量落账（目前仅 xsdt_pbase）
        if (e->kind == ASSET_KIND_SCALAR) {
            if (strcmp_in_kernel(e->name, asset_names::xsdt_pbase) == 0) {
                g_xsdt_base = *(const phyaddr_t*)e->data;
                init_printk("[assets_remap] xsdt_pbase: phys 0x%lx", (unsigned long)g_xsdt_base);
            }
            continue;
        }

        if (e->kind != ASSET_KIND_MEM_INTERVAL) continue;

        const vm_interval iv = *(const vm_interval*)e->data;

        // hdstacks 特殊：先圈地再逐栈精细映射（guard 页不映射）
        if (strcmp_in_kernel(e->name, asset_names::hdstacks) == 0) {
            KURD_t k = remap_hdstacks(iv);
            if (error_kurd(k)) {
                init_printk("[assets_remap] hdstacks fine remap fail");
                return k;
            }
            init_printk("[assets_remap] hdstacks: territory v=0x%lx p=0x%lx npg=%lu, "
                        "%lu procs fine-mapped (guards skipped)",
                        (unsigned long)iv.vbase(), (unsigned long)iv.pbase(),
                        (unsigned long)iv.npages, (unsigned long)logical_processor_count);
            continue;
        }
        if(strcmp_in_kernel(e->name, asset_names::gs_complexes)==0){
            conjucnt_GSs=*(vm_interval*)e->data;
        }

        // 普通 mem 型：Kspace_phyaddr_direct_map 一站式（登记 VM_DESC + 建页表）
        KURD_t k = Kspace_phyaddr_direct_map(iv);
        if (error_kurd(k)) {
            init_printk("[assets_remap] direct map fail: %s", name);
            return k;
        }
        init_printk("[assets_remap] %s: v=0x%lx p=0x%lx npg=%lu",
                    name, (unsigned long)iv.vbase(), (unsigned long)iv.pbase(),
                    (unsigned long)iv.npages);
    }
    init_printk("all mem_properties mapped");
    return KURD_t();
}
extern "C" uint32_t assigned_cr3;
KURD_t mem_init(){ 
    KURD_t bsp_init_kurd;
    bsp_init_kurd=KspacePageTable::Init();
    if(error_kurd(bsp_init_kurd)){
        init_printk("KspaceMapMgr Init Failed");
        return bsp_init_kurd;
    }
    bsp_init_kurd=assets_remap();
    gKernelSpace=new AddressSpace();
    bsp_init_kurd=gKernelSpace->second_stage_init();
    if(error_kurd(bsp_init_kurd)){
        init_printk("identity map fail");
        return bsp_init_kurd;
    }
    // 把老的 cr3 读出来在先（init.elf kmmu 根表，含 identity + 资产粗映射；
    // 切页表后凭它 BFS 整棵回收，见 bfs_delete_old_pagetable 注释）
    phyaddr_t old_cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3) :: "memory");
    old_cr3 &= ~0xFFFull;   // 去 PCID/低 12 位杂项，只留根表物理页基址

    gKernelSpace->unsafe_load_pml4_to_cr3(KERNEL_SPACE_PCID);
    // BFS 释放老的页表（新页表已含主窗口映射，老表页可经窗口走读；叶数据页归 FPA 管）
    if (bfs_delete_old_pagetable(old_cr3) != 0) {
        init_printk("[mem_init] bfs_delete_old_pagetable fail, old_cr3=0x%lx",
                    (unsigned long)old_cr3);
        KURD_t fail;
        fail.result      = result_code::FAIL;
        fail.level       = level_code::ERROR;
        fail.module_code = module_code::MEMORY;
        return fail;
    }
    {
    const asset_table_entry*fpa_bitmap=g_asset_table->read(asset_names::fpa_bitmaps);
    movable_file_entry_t*fpa_pinterval=(movable_file_entry_t*)fpa_bitmap->data;
    vm_interval fpa_vinterval={
        .vpn=PHYACC_VA(fpa_pinterval->base_ppn<<12)>>12,
        .ppn=fpa_pinterval->base_ppn,
        .npages=align_up(fpa_pinterval->size,0x1000)>>12
    };
    bsp_init_kurd=FreePagesAllocator::Init(FreePagesAllocator::BEST_FIT,&fpa_vinterval);
    if(error_kurd(bsp_init_kurd)){
        init_printk("FreePagesAllocator Init Failed");
        return bsp_init_kurd;
    }
    }
    bsp_init_kurd=kpoolmemmgr_t::multi_heap_enable();
    if(error_kurd(bsp_init_kurd)){
        init_printk("Kpoolmemmgr_t::multi_heap_enable Failed");
    }
    // 加载非内核地址空间的段（ap_bootstrap 等）到物理内存并映射进新 PML4
    bsp_init_kurd = load_low_half_segments();
    if (error_kurd(bsp_init_kurd)) {
        init_printk("load_low_half_segments Failed");
        return bsp_init_kurd;
    }
    assigned_cr3=gKernelSpace->get_root_table_phybase();
    __sync_synchronize();
    GlobalKernelStatus=kernel_state::MM_READY;
    return KURD_t();
}
