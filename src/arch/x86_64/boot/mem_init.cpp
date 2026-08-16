#include "memory/memory_base.h"
#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/init_memory_info.h"
#include "memory/AddresSpace.h"
#include "memory/phyaddr_accessor.h"
#include "memory/page_frame_state_mgr.h"
#include "memory/main_phyaddr_access_window.h"
#include "arch/x86_64/mem_init.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "util/kout.h"
#include "elf.h"
#include "panic.h"
uint64_t VM_intervals_count;
phymem_segment *phymem_segments;
uint64_t phymem_segments_count; 
uint32_t logical_processor_count;
vm_interval Kspace_phyaddr_access_window;
phyaddr_t g_xsdt_base;
// 从 kIMG_self_window 解析 ELF 程序头表，标记所有 PT_LOAD 段为持久页
// 替换旧版对 kBSS_interval 的单独标记——BSS 已是普通 PT_LOAD
static KURD_t persist_elf_segments() {
    uint8_t* elf_base = (uint8_t*)kIMG_self_window.vbase();
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        bsp_kout << "[persist_elf] bad magic" << kendl;
        return {result_code::FATAL, 0, module_code::MEMORY,
                MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
                0, level_code::FATAL, err_domain::CORE_MODULE};
    }
    uint8_t* ptbl = elf_base + ehdr->e_phoff;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint64_t npg = align_up(ph->p_memsz, 4096) >> 12;
        if (npg == 0) continue;
        KURD_t kurd = all_pages_arr::simp_pages_set(ph->p_paddr, npg, page_state_t::kernel_persisit);
        if (error_kurd(kurd)) return kurd;
    }
    return {result_code::SUCCESS, 0, module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
            0, level_code::INFO, err_domain::CORE_MODULE};
}

KURD_t pesisitent_properties_set(){
    KURD_t bsp_init_kurd;
    bsp_init_kurd=persist_elf_segments();  // 从 ELF 程序头表标记所有 PT_LOAD 段（含 BSS）
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    bsp_init_kurd=all_pages_arr::simp_pages_set(pages_arr.pbase(),pages_arr.npages,page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    bsp_init_kurd=all_pages_arr::simp_pages_set(FPA_bitmaps.pbase(),FPA_bitmaps.npages,page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    bsp_init_kurd=all_pages_arr::simp_pages_set(conjucnt_GSs.pbase(),conjucnt_GSs.npages,page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    return bsp_init_kurd;
}
void fpa_properties_deal(){
    phymem_segment initramfs;
    initramfs.start=initramfs_file.interval.pbase();
    initramfs.size=initramfs_file.interval.byte_cnt();
    phymem_segment symboltable;
    symboltable.start=symtable_file.interval.pbase();
    symboltable.size=symtable_file.interval.byte_cnt();
    phymem_segment log_interval;
    log_interval.start=log_buffer.pbase();
    log_interval.size=log_buffer.byte_cnt();
    phymem_segment KImg_pinterval;
    KImg_pinterval.start=kIMG_self_window.pbase();
    KImg_pinterval.size=kIMG_self_window.byte_cnt();
}
KURD_t KImage_map_rebuild(){
    KURD_t success(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        0, level_code::INFO, err_domain::CORE_MODULE
    );

    // 通过 kIMG_self_window 读取 ELF（CR3 尚未切换，旧 identity 页表仍可见）
    uint8_t* elf_base = (uint8_t*)kIMG_self_window.vbase();
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        bsp_kout << "[KImage_map_rebuild] bad ELF magic" << kendl;
        KURD_t fail(
            result_code::FATAL, 0, module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
            0, level_code::FATAL, err_domain::CORE_MODULE
        );
        return fail;
    }

    uint8_t* ptbl = elf_base + ehdr->e_phoff;

    // 遍历所有 PT_LOAD，在 KspacePageTable（新 PML4）中重建映射
    // 物理地址统一从 p_paddr 读取——init.elf 已将 0x100 段的真实 PA 写回此处
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        phyaddr_t pa = ph->p_paddr;   // init.elf 已填好的真实 PA
        vaddr_t   va = ph->p_vaddr;   // 固定虚拟地址
        if(!is_addr_kernel_address((void*)va))continue;
        uint64_t  sz = align_up(ph->p_memsz, 4096);
        uint64_t npg = sz >> 12;

        // 构造页表访问属性
        pgaccess acc;
        acc.is_kernel      = 1;
        acc.is_writeable   = (ph->p_flags & PF_W) ? 1 : 0;
        acc.is_readable    = 1;
        acc.is_executable  = (ph->p_flags & PF_X) ? 1 : 0;
        acc.is_global      = 1;
        acc.cache_strategy = WB;

        vm_interval seg_iv = {.vpn = va >> 12, .ppn = pa >> 12,
                              .npages = npg, .access = acc};
        // 跳过不在内核地址空间的段（如 TLS 段在低地址）
        if (!seg_iv.is_kernel_address()) {
            bsp_kout << "[KImage_map_rebuild]  skip seg[" << i << "] v=0x" << HEX << va
                     << " (non-kernel address)" << DEC << kendl;
            continue;
        }
        // 恒定映射：Kspace_phyaddr_direct_map 既建页表又维护红黑树
        KURD_t kurd = Kspace_phyaddr_direct_map(seg_iv);
        if (error_kurd(kurd)) {
            bsp_kout << "[KImage_map_rebuild] seg " << i << " map fail" << kendl;
            return kurd;
        }
        bsp_kout << "[KImage_map_rebuild]  seg[" << i << "] v=0x" << HEX << va
                 << " p=0x" << pa << " sz=0x" << sz << kendl;
    }
    bsp_kout.shift_hex();
    bsp_kout << "[KImage_map_rebuild] done: kIMG 0x" << kIMG_self_window.pbase()
             << " ->0x" << kIMG_self_window.vbase() << kendl;
    bsp_kout.shift_dec();

    return success;
}
// 全量 TLB 刷出：reload CR3 使所有非全局缓存项失效
// 在 Kspace_phyaddr_direct_map 后必须立即调用，防止 old identity-map TLB 残留
static void tlb_full_flush() {
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    asm volatile("mov %0, %%cr3" :: "r"(cr3_val) : "memory");
}

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
            bsp_kout << "[bfs_delete_old_pagetable] state_set free fail pa=0x"
                     << HEX << pa << DEC << kendl;
            failed = true;
            break;
        }
        freed_count++;
    }

    delete[] queue;
    if (failed) return SRC_LOC();
    bsp_kout << "[bfs_delete_old_pagetable] freed " << freed_count
             << " page-table pages (old root 0x" << HEX << old_root << ")" << DEC << kendl;
    return 0;
}


KURD_t kimg_affiliate_property_map1(){
    struct aff_entry {
        vm_interval* iv;
        const char* name;
    };
    // ── hw_stacks 特殊处理：逐处理器逐栈映射，跳过 guard 页（镜像 init.elf Phase 3b 逻辑）──
    {
        uint64_t stack_stride = sizeof(per_processor_hardware_stack_t);
        vaddr_t  hw_v = hw_stacks.vbase();
        phyaddr_t hw_p = hw_stacks.pbase();
        for (uint32_t p = 0; p < logical_processor_count; p++) {
            uint64_t  off    = p * stack_stride;
            vaddr_t   proc_v = hw_v + off;
            phyaddr_t proc_p = hw_p + off;

            auto map_stack = [&](uint64_t field_off, uint64_t sz, const char* name) -> KURD_t {
                if (sz == 0) return {result_code::SUCCESS, 0, module_code::MEMORY,
                                     MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
                                     0, level_code::INFO, err_domain::CORE_MODULE};
                vm_interval iv = {.vpn = (proc_v + field_off) >> 12,
                                  .ppn = (proc_p + field_off) >> 12,
                                  .npages = sz >> 12, .access = KSPACE_RW_ACCESS};
                KURD_t kurd = Kspace_phyaddr_direct_map(iv);  // 恒定映射：建红黑树 + 页表
                if (error_kurd(kurd))
                    bsp_kout << "[kimg_affiliate] hw_stacks[" << p << "]." << name << " fail" << kendl;
                return kurd;
            };
            KURD_t k;
            k = map_stack(RSP0_BASE_OFF, RSP0_STACKSIZE, "rsp0"); if (error_kurd(k)) return k;
            k = map_stack(IST1_BASE_OFF, DF_STACKSIZE,   "ist1"); if (error_kurd(k)) return k;
            k = map_stack(IST2_BASE_OFF, MC_STACKSIZE,   "ist2"); if (error_kurd(k)) return k;
            k = map_stack(IST3_BASE_OFF, NMI_STACKSIZE,  "ist3"); if (error_kurd(k)) return k;
            k = map_stack(IDLE_TASK_STACK_BASE_OFF, IDLE_TASK_STACKSIZE, "idle_task"); if (error_kurd(k)) return k;
        }
        bsp_kout << "[kimg_affiliate_property_map1] hw_stacks: " << logical_processor_count
                 << " processors, stride=" << stack_stride << kendl;
    }

    aff_entry list[] = {
        {&FPA_bitmaps,         "FPA_bitmaps"},
        {&pages_arr,           "pages_arr"},
        {&hpet_mmio,           "hpet_mmio"},
        {&gop_buffer,          "gop_buffer"},
        {&conjucnt_GSs,        "conjucnt_GSs"},
        {&Kspace_phyaddr_access_window, "Kspace_phyaddr_access_window"},
    };

    for (auto& e : list) {
        if (e.iv->npages == 0) {
            bsp_kout << "[kimg_affiliate_property_map1] skip " << e.name
                     << " (npages=0 or ppn=0)" << kendl;
            continue;
        }
        KURD_t map_kurd = Kspace_phyaddr_direct_map(*e.iv);
        if (error_kurd(map_kurd)) {
            bsp_kout << "[kimg_affiliate_property_map1] " << e.name
                     << " map fail" << kendl;
            return map_kurd;
        }
        bsp_kout << "[kimg_affiliate_property_map1] " << e.name
                 << " 0x" << HEX << e.iv->pbase() << " ->0x" << e.iv->vbase() << kendl;
        bsp_kout.shift_dec();
    }
    KURD_t ok(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        0, level_code::INFO, err_domain::CORE_MODULE
    );
    return ok;
}
// 将物理资源从 init.elf 分配转生到 kernel 的 FreePagesAllocator
// 策略：新分配 → 拷贝内容 → 更新 ppn → 重映射 → 释放旧区间
// 三个资产（initramfs / symtable / log_buffer）共享此模式
KURD_t properties_modify_stage1(){
    auto relocate_one = [](vm_interval* iv, phyaddr_t old_pbase, uint64_t old_size,
                           const char* name) -> KURD_t {
        KURD_t kurd;
        uint64_t new_pa = FreePagesAllocator::alloc(
            iv->byte_cnt(), BUDDY_ALLOC_DEFAULT_FLAG, page_state_t::kernel_anonymous, kurd);
        if (error_kurd(kurd)) {
            bsp_kout << name << " alloc Failed" << kendl;
            return kurd;
        }
        // 拷贝内容到新位置
        PhyAddrAccessor::paddr_memcpy(new_pa, old_pbase, old_size);
        iv->ppn = new_pa >> 12;
        // 重映射 + TLB 刷出
        kurd = Kspace_phyaddr_direct_map(*iv);
        tlb_full_flush();
        if (error_kurd(kurd)) {
            bsp_kout << name << " phyaddr_direct_map Failed" << kendl;
            return kurd;
        }
        // 释放旧物理页（收养后位图直接归还）
        FreePagesAllocator::free(old_pbase, old_size);
        return kurd;
    };

    KURD_t kurd;

    kurd = relocate_one(&initramfs_file.interval,
                         initramfs_file.interval.pbase(),
                         initramfs_file.interval.byte_cnt(),
                         "initramfs_file");
    if (error_kurd(kurd)) return kurd;

    kurd = relocate_one(&symtable_file.interval,
                         symtable_file.interval.pbase(),
                         symtable_file.interval.byte_cnt(),
                         "symtable_file");
    if (error_kurd(kurd)) return kurd;

    kurd = relocate_one(&log_buffer,
                         log_buffer.pbase(),
                         log_buffer.byte_cnt(),
                         "log_buffer");
    if (error_kurd(kurd)) return kurd;

    kurd = relocate_one(&kIMG_self_window,
                         kIMG_self_window.pbase(),
                         kIMG_self_window.byte_cnt(),
                         "kIMG_self_window");
    if (error_kurd(kurd)) return kurd;

    return {result_code::SUCCESS, 0, module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
            0, level_code::INFO, err_domain::CORE_MODULE};
}
extern "C" uint32_t assigned_cr3;
KURD_t mem_init(){ 
    KURD_t bsp_init_kurd;
    PhyAddrAccessor::Init(Kspace_phyaddr_access_window);
    bsp_init_kurd=all_pages_arr::Init(&pages_arr);
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"phymemspace_mgr Init Failed"<<kendl;
        return bsp_init_kurd;
    }
     bsp_init_kurd=pesisitent_properties_set();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"pesisitent_properties_set Failed"<<kendl;
        return bsp_init_kurd;
    }
    bsp_init_kurd=FreePagesAllocator::Init(FreePagesAllocator::BEST_FIT,&FPA_bitmaps);
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"FreePagesAllocator Init Failed"<<kendl;
        return bsp_init_kurd;
    }
    fpa_properties_deal();
    if(error_kurd(bsp_init_kurd)){
        return bsp_init_kurd;
    }
    bsp_init_kurd=KspacePageTable::Init();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"KspaceMapMgr Init Failed"<<kendl;
        return bsp_init_kurd;
    }
    bsp_init_kurd=KImage_map_rebuild();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"KImage_map_rebuild Failed"<<kendl;
        return bsp_init_kurd;
    }
    bsp_init_kurd=kimg_affiliate_property_map1();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"kimg_affiliate_property_map1 Failed"<<kendl;
        return bsp_init_kurd;
    }
    gKernelSpace=new AddressSpace();
    bsp_init_kurd=gKernelSpace->second_stage_init();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"identity map fail"<<kendl;
        return bsp_init_kurd;
    }
    gKernelSpace->unsafe_load_pml4_to_cr3(KERNEL_SPACE_PCID);
    properties_modify_stage1();
    bsp_init_kurd=kpoolmemmgr_t::multi_heap_enable();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"Kpoolmemmgr_t::multi_heap_enable Failed"<<kendl;
    }

    // 加载非内核地址空间的段（ap_bootstrap/TLS 等）到新 PML4
    // 这些段在 KImage_map_rebuild 中被 is_kernel_address() 跳过，
    // 但 AP 启动代码需要它们在低地址（0x4000+）可访问
    {
        uint8_t* elf_b = (uint8_t*)kIMG_self_window.vbase();
        Elf64_Ehdr* eh = (Elf64_Ehdr*)elf_b;
        uint8_t* ptbl   = elf_b + eh->e_phoff;
        bsp_kout << "[mem_init] mapping non-kernel segments..." << kendl;
        for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
            Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * eh->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;
            if (ph->p_memsz == 0) continue;

            vaddr_t   va = ph->p_vaddr;
            phyaddr_t pa = ph->p_paddr;
            uint64_t  sz = align_up(ph->p_memsz, 4096);

            pgaccess acc = {1, (uint8_t)((ph->p_flags & PF_W) ? 1 : 0), 1,
                            (uint8_t)((ph->p_flags & PF_X) ? 1 : 0), 0, WB};
            vm_interval seg = {.vpn = va >> 12, .ppn = pa >> 12,
                               .npages = sz >> 12, .access = acc};
            // 跳过已在 KImage_map_rebuild 处理的内核地址段
            if (seg.is_kernel_address()) continue;

            KURD_t mk = gKernelSpace->enable_low_half_vm_interval(seg);
            if (error_kurd(mk)) {
                bsp_kout << "[mem_init] non-kernel seg[" << i << "] map fail" << kendl;
            } else {
                bsp_kout << "  seg[" << i << "] v=0x" << HEX << va
                         << " p=0x" << pa << " sz=0x" << sz << DEC << kendl;
            }
        }
    }
    assigned_cr3=gKernelSpace->get_root_table_phybase();
    __sync_synchronize();
    GlobalKernelStatus=kernel_state::MM_READY;
    return KURD_t();
}
