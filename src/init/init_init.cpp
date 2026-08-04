#include "abi/boot.h"
#include "init/load_kernel.h"
#include "init/init_asset_registry.h"
#include "init/page_allocator.h"
#include "init/init_bcb_juvenile.h"
#include "init/pages_alloc.h"
#include "init/util/textConsole.h"
#include "init/util/kout.h"
#include "init/core_hardwares/PortDriver.h"
#include "init/init_fatal.h"
#include "init/init_linker_symbols.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "init/core_hardwares/init_gop.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "arch/x86_64/boot.h"          // x86_specify_init_to_kernel_info
#include "firmware/gSTResloveAPIs.h"
#include "init/initramfs_lookup.h"      // initramfs_lookup, RSDP_struct, ACPI_20_TABLE_GUID
#include "initramfs/fs_format.h"        // initramfs_header
#include "arch/x86_64/init/page_table.h"// modify_access
#include "arch/x86_64/core_hardwares/HPET.h"
#include "memory/memory_base.h"
#include "init/init_phase_ctx.h"
#include "init/init_heap_v3.h"
#include "sys/io.h"
void wrmsr_func(uint32_t offset, uint64_t value)
{
    uint32_t value_high=(value>>32)&0xffffffff, value_low=value&0xffffffff; 
    asm volatile("wrmsr"
                 :
                 : "c" (offset),
                   "a" (value_low),
                   "d" (value_high));
}
#include <elf.h>
extern "C" void init_jump_to_kernel(x64_standard_context_v2* ctx);
// ============================================================================
// 前向声明 & 常量
// ============================================================================
#define OS_INVALID_PARAMETER (-5)
#define OS_ACPI_NOT_FOUNED   (-6)
#define OS_OUT_OF_MEMORY     (-7)
#define OS_SUCCESS           0

// 入口在 gs_complex_load_gdt_tss + iretq 中实现（替代 shift_kernel）
// 初始 RFLAGS（与 create_kthread 一致）：IF=1, IOPL=0, 保留位 1
static constexpr uint64_t KERNEL_INIT_RFLAGS = 0x002;

// 全局 magic
static constexpr uint64_t INIT_TO_KERNEL_MAGIC = 0x494E494B524E4C48ULL; // "INIKRNLH"

// ============================================================================
// VA 分配器状态（唯一保留的文件级全局）
// ============================================================================
// 定义在 kernel_load.cpp，这里使用相同的全局变量
extern uint64_t g_va_alloc_base;

// ── BSS: 堆位图 ──
// flat bitmap: 1 bit = 8B，2MB 堆 → 2MB/8 = 262144 bits = 32KB
static constexpr uint64_t HEAP_BITMAP_BITS  = 1ull << 18;   // 262144
static constexpr uint64_t HEAP_BITMAP_BYTES = HEAP_BITMAP_BITS / 8;
alignas(64) static uint8_t s_heap_bitmap[HEAP_BITMAP_BYTES];
uint64_t va_alloc(uint64_t size,uint8_t align_log2){
    if (align_log2 < 12) {
        align_log2 = 12;
    }
    if(g_va_alloc_base==0)return 0;
    uint64_t alignment = 1ULL << align_log2;
    uint64_t res=align_down(g_va_alloc_base, alignment);
    g_va_alloc_base += size;
    return res;
}
// ============================================================================
// Phase 1: 输出器 + 堆
// ============================================================================
static loc_code_t init_io_and_heap(BootInfoHeader* header) {
    bsp_kout.Init();
    // 初始化 V3 伴侣堆 (BCB-based, 单线程, 无锁)
    uint64_t heap_sz = (uint64_t)&__init_heap_end - (uint64_t)&__init_heap_start;
    g_init_heap.linktime_init((vaddr_t)&__init_heap_start,
                              (uint32_t)heap_sz,
                              (vaddr_t)s_heap_bitmap);
    bsp_kout.shift_hex();
    // pass-through: 初始化 GOP
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].device_info == PASS_THROUGH_DEVICE_GRAPHICS_INFO) {
            auto* gfx = (GlobalBasicGraphicInfoType*)header->pass_through_devices[i].specify_data;
            InitGop::Init(gfx);
            break;
        }
    }
    init_textconsole::Init(
        (const unsigned char*)ter16x32_data, {16, 32}, 0xFFFFFFFF, 0xFF000000);
    serial_init_stage1();
    bsp_kout << "[INIT] Phase 1: I/O + heap ready" << kendl;
    return 0;
}

// ============================================================================
// Phase 2: 内存准备 → 返回 ctx_early_mem
// ============================================================================
static ctx_early_mem init_memory_early(BootInfoHeader* header) {
    ctx_early_mem em = {};

    // 2a. 解析 XSDT (UEFI memory 尚可访问)
    if (header->gST_ptr) {
        auto* st = (EFI_SYSTEM_TABLE*)header->gST_ptr;
        auto* ct  = st->ConfigurationTable;
        UINTN n   = st->NumberOfTableEntries;
        const EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
        const uint8_t* g1 = (const uint8_t*)&acpi20;
        for (uint64_t i = 0; i < n; i++) {
            const uint8_t* g2 = (const uint8_t*)&ct[i].VendorGuid;
            bool match = true;
            for (size_t b = 0; b < sizeof(EFI_GUID); b++)
                if (g1[b] != g2[b]) { match = false; break; }
            if (match) {
                auto* rsdp = (RSDP_struct*)ct[i].VendorTable;
                if (rsdp && rsdp->Revision >= 2) {
                    em.xsdt_base = rsdp->XsdtAddress;
                    break;
                }
            }
        }
    }
    if (em.xsdt_base)
        bsp_kout << "[INIT] XSDT at phys 0x" << em.xsdt_base << kendl;
    else
        bsp_kout << "[WARN] ACPI 2.0 XSDT not found" << kendl;

    // 2b. basic_allocator 自举
    int r = basic_allocator::Init(header->memory_map_ptr, header->memory_map_entry_count);
    if (r != 0) { bsp_kout << "[INIT] basic_allocator::Init failed: " << r << kendl; return em; }

    // 2c. 标记 init 自身映像
    uint64_t init_img_sz = (uint64_t)&__init_heap_end - (uint64_t)&__init_text_start;
    basic_allocator::pages_set({(uint64_t)&__init_text_start, align_up(init_img_sz, 4096)},
                                PHY_MEM_TYPE::OS_KERNEL_DATA);
    basic_allocator::pages_set({(uint64_t)header, (uint64_t)header->total_pages_count * 4096},
                                PHY_MEM_TYPE::OS_KERNEL_DATA);
    for (uint64_t i = 0; i < header->loaded_file_count; i++) {
        if (header->loaded_files[i].file_type == LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD) continue;
        basic_allocator::pages_set(
            {(uint64_t)header->loaded_files[i].raw_data,
             align_up(header->loaded_files[i].file_size, 4096)},
            PHY_MEM_TYPE::OS_KERNEL_DATA);
    }

    // 2d. init_bcb_juvenile 顶替 page_allocator —— 幼年 BCB 位图分配器（分配职责唯一来源）
    //     位图池从 basic_allocator 预挖（POOL_SOURCE_BASIC_ALLOCATOR，生产路径）
    {
        uint64_t segcnt = 0;
        phymem_segment* view = basic_allocator::get_pure_memory_view(&segcnt);
        if (!view || segcnt == 0) {
            bsp_kout << "[INIT] get_pure_memory_view failed" << kendl; return em;
        }

        bcb_juvenile_init_config cfg = bcb_juvenile_init_config::BEST_FIT();
        cfg.segs                    = view;
        cfg.segs_count              = segcnt;
        cfg.logical_processor_count = header->logical_processor_count;
        cfg.pool                    = bcb_juvenile_init_config::POOL_SOURCE_BASIC_ALLOCATOR;
        if (init_bcb_juvenile::plan_and_setup(&cfg) != 0) {
            bsp_kout << "[INIT] init_bcb_juvenile::plan_and_setup failed" << kendl; return em;
        }
        bsp_kout << "[INIT] BCB up: managed=" << init_bcb_juvenile::total_page_count()
                 << " free=" << init_bcb_juvenile::free_page_count()
                 << " pool=0x" << HEX << init_bcb_juvenile::region_pbase_ << DEC << kendl;

        // 2c 再做一遍（BCB 侧钉占用）：纯视图快照里 init 镜像/header/loaded files 仍是
        // freeSystemRam，plan 会覆盖它们；位图里不钉住，BCB 就会把它们分配出去。
        init_bcb_juvenile::mark_used((uint64_t)&__init_text_start, align_up(init_img_sz, 4096));
        init_bcb_juvenile::mark_used((uint64_t)header, (uint64_t)header->total_pages_count * 4096);
        for (uint64_t i = 0; i < header->loaded_file_count; i++) {
            if (header->loaded_files[i].file_type == LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD) continue;
            init_bcb_juvenile::mark_used(
                (uint64_t)header->loaded_files[i].raw_data,
                align_up(header->loaded_files[i].file_size, 4096));
        }
        // 低 1MB（x86 实模式 IVT/BDA/EBDA/BIOS，保留语义）
        init_bcb_juvenile::mark_used(0, 0x100000);
    }

    // 2e. page_allocator 降级为 pages_arr 账本（不再承担分配）
    //     kernel 的 all_pages_arr::Init 会从 handoff 的 phymem_segments 全量重建状态，
    //     它真正需要的只是这块 per-page 缓冲（决定 mem_map_entry_count 上界）。
    //     handoff 升级前保留；其缓冲页必须钉进 BCB，防被分配出去。
    r = page_allocator::init();
    if (r != 0) { bsp_kout << "[INIT] page_allocator ledger init failed: " << r << kendl; return em; }
    init_bcb_juvenile::mark_used(page_allocator::get_mem_map_pbase(),
                                 page_allocator::total_page_count() * sizeof(page));

    bsp_kout << "[INIT] Phase 2: memory ready, BCB free pages="
             << init_bcb_juvenile::free_page_count() << kendl;
    return em;
}

// ============================================================================
// Phase 2.5: initramfs 原位锚定（不再高位搬迁）
// ============================================================================
// 旧版 relocate_initramfs 把 initramfs 从 UEFI 加载处搬到 BCB 新分配区。
// 现改为直接引用 UEFI 加载位置 + mark_used 钉住：BCB 纯视图里 loaded 文件已在
// 2c mark_used，此处显式重申 + 更新 ctx_early_mem，避免搬迁造成的二次记账。
static void initramfs_mark_used(BootInfoHeader* header, ctx_early_mem* em) {
    loaded_file_entry* ramfs = nullptr;
    for (uint64_t i = 0; i < header->loaded_file_count; i++) {
        if (strcmp_in_kernel(header->loaded_files[i].file_name, "\\initramfs.img") == 0) {
            ramfs = &header->loaded_files[i];
            break;
        }
    }
    if (!ramfs || ramfs->raw_data == 0) {
        bsp_kout << "[INIT] initramfs not loaded" << kendl;
        em->ramfs_base = 0;
        em->ramfs_size = 0;
        return;
    }

    em->ramfs_base = (uint64_t)ramfs->raw_data;
    em->ramfs_size = align_up(ramfs->file_size, 4096);

    init_bcb_juvenile::mark_used(em->ramfs_base, em->ramfs_size);
    bsp_kout << "[INIT] initramfs in-place: base=0x" << HEX << em->ramfs_base
             << " size=0x" << em->ramfs_size << DEC << kendl;
}
uint64_t g_va_alloc_base=0;
// ============================================================================
// va_alloc_up — 从 g_va_alloc_base 向上分配 VA（Phase 3b 专用）
// ============================================================================
static uint64_t va_alloc_up(uint64_t size, uint8_t align_log2) {
    uint64_t align = 1ULL << align_log2;
    if (align < 4096) align = 4096;
    size = align_up(size, align);
    uint64_t ret = align_up(g_va_alloc_base, align);
    g_va_alloc_base = ret + size;
    return ret;
}

// 登记资产进 init 侧资产树（handoff 清单）。多arg name 的 arg1 指示路由类型，
// 树键 = arg0（本名）。返回 false 表示同名冲突（arg0 重复）。
static bool asset_reg_add(const char* name, void* data) {
    if (!g_asset_registry) return false;
    return g_asset_registry->add({ const_cast<char*>(name), data });
}

// ============================================================================
// Phase 3a (串行): kernel.elf 解包 → 精确狙击 4 段进 kmmu → 产出进资产容器
// ============================================================================
//
// 本函数是纯"产出方"：产物全部进资产容器（隐式状态），函数只返回 loc_code_t。
//   - 四段 kernel_code/data/rodata/bss → "kernel_* mem"（vm_interval desc）
//   - kIMG（kernel.elf 瞬态文件映像）→ "kimg movable"（movable_file_entry_t desc）
//   - 入口点 → "entry_vaddr scalar"
// ctx/header 管线退居幕后（偶然复杂度），调用点从容器按 arg0 取资产。
//
// 设计：
//   1. kernel.elf 完整文件 → 瞬态端分配 kimg_pbase（kIMG 文件的家，经恒等窗口访问）
//   2. 不再遍历所有 PT_LOAD 加载；按段名精确命中 4 个段：
//        .text(.text_main) / .data / .rodata / .bss
//      每段三关：名字匹配 → 确认在 PT_LOAD 内 → is_kernel_address 确认内核区间
//   3. 独立分配物理页 + 拷贝/清零 + 进入 kmmu（kernel_code/data/rodata/bss 名字）
//   4. 所属 LOAD 的 p_paddr 写回真实 PA（kernel.elf 经 kIMG 自省契约不变）
//   5. kIMG 不做专用映射（恒等窗口已覆盖），登记 movable 资产即可
//   ap_bootstrap 低地址段不做加载（本函数只管内核区间资产）
//
// 依赖：kld.ld 下每个主段独立成 LOAD（section ⊇ LOAD 1:1），p_paddr 写回即整段真实 PA。
//
static loc_code_t phase_3a_load_kernel(kernel_mmu* kmmu, const ctx_early_mem* em,
                                       BootInfoHeader* /*header*/) {
    phyaddr_t kimg_pbase = 0;

    // ---- 1. 从 initramfs 定位 kernel.elf，拷贝到瞬态端 ----
    if (em->ramfs_base == 0) {
        bsp_kout << "[Phase3a] initramfs not present" << kendl; init_fatal::halt(SRC_LOC());
    }
    const initramfs_header* rh = (const initramfs_header*)(uint64_t)em->ramfs_base;
    uint64_t kelf_sz = 0;
    phyaddr_t kelf_in_ramfs = initramfs_lookup(rh, "/kernel.elf", &kelf_sz);
    if (kelf_in_ramfs == 0 || kelf_sz == 0) {
        bsp_kout << "[Phase3a] initramfs_lookup failed" << kendl; init_fatal::halt(SRC_LOC());
    }
    uint64_t kelf_pages = align_up(kelf_sz, 4096) >> 12;
    loc_code_t alloc_err = 0;
    kimg_pbase = init_bcb_juvenile::alloc(kelf_pages, 12, &alloc_err);
    if (kimg_pbase == 0) {
        bsp_kout << "[Phase3a] transient OOM: " << kelf_pages << " pages" << kendl; init_fatal::halt(SRC_LOC());
    }
    page_allocator::pages_set({kimg_pbase, kelf_pages << 12}, page_state_t::kernel_persisit);
    ksystemramcpy((void*)(uint64_t)kelf_in_ramfs, (void*)(uint64_t)kimg_pbase, kelf_sz);
    ksetmem_8((void*)(uint64_t)(kimg_pbase + kelf_sz), 0, (kelf_pages << 12) - kelf_sz);
    bsp_kout << "[Phase3a] kernel.elf (transient) at 0x" << kimg_pbase
             << " size=" << kelf_sz << kendl;

    // ---- 2. ELF header 校验 ----
    uint8_t* elf_base = (uint8_t*)(uint64_t)kimg_pbase;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[EI_MAG0]!=ELFMAG0||ehdr->e_ident[EI_MAG1]!=ELFMAG1||
        ehdr->e_ident[EI_MAG2]!=ELFMAG2||ehdr->e_ident[EI_MAG3]!=ELFMAG3) {
        bsp_kout << "[Phase3a] bad magic" << kendl; init_fatal::halt(SRC_LOC());
    }
    bsp_kout << "[Phase3a] phnum=" << ehdr->e_phnum
             << " shnum=" << ehdr->e_shnum
             << " entry=0x" << HEX << ehdr->e_entry << DEC << kendl;

    // 登记入口点资产（scalar，phase_4.5 跳转用；隐式状态走容器）
    {
        uint64_t* entry_desc = new uint64_t(ehdr->e_entry);
        if (!asset_reg_add("entry_vaddr scalar", entry_desc)) {
            bsp_kout << "[Phase3a] asset dup: entry_vaddr" << kendl;
            init_fatal::halt(SRC_LOC());
        }
    }

    // ---- 3. 段表解析 + 精确狙击 4 段 ----
    if (ehdr->e_shnum == 0 || ehdr->e_shstrndx == SHN_UNDEF) {
        bsp_kout << "[Phase3a] no section headers" << kendl; init_fatal::halt(SRC_LOC());
    }
    Elf64_Shdr* shdr = (Elf64_Shdr*)(elf_base + ehdr->e_shoff);
    Elf64_Shdr& shstr_hdr = shdr[ehdr->e_shstrndx];
    const char* shstrtab = (const char*)(elf_base + shstr_hdr.sh_offset);

    struct sec_target_t {
        const char* name_key;    // 段名匹配键
        bool        prefix;      // true=前缀匹配（.text 兼容 .text_main）
        const char* kmmu_name;   // 进 kmmu 的名字（arg0 语义）
        const char* asset_name;  // 进资产树的多arg 名字（arg0 + arg1 路由）
    };
    const sec_target_t k_sec_targets[] = {
        { ".text",   true,  "kernel_code",   "kernel_code mem"   },
        { ".data",   false, "kernel_data",   "kernel_data mem"   },
        { ".rodata", false, "kernel_rodata", "kernel_rodata mem" },
        { ".bss",    false, "kernel_bss",    "kernel_bss mem"    },
    };
    constexpr int k_sec_count = 4;

    // 名字匹配：prefix ? 前缀 : 精确
    auto sec_name_hit = [](const char* sname, const sec_target_t& tg) -> bool {
        if (tg.prefix) {
            return strncmp_in_kernel(sname, tg.name_key, strlen_in_kernel(tg.name_key)) == 0;
        }
        return strcmp_in_kernel(sname, tg.name_key) == 0;
    };

    uint64_t kernel_vaddr_top = 0;

    // 加载单个命中段：PT_LOAD 确认 → 内核区间确认 → 分配/拷贝/清零 → kmmu → 资产树
    auto load_section = [&](Elf64_Shdr& sh, const char* kmmu_name, const char* asset_name) -> int {
        if (sh.sh_size == 0) return -12;

        // ---- a. 确认属于某个 PT_LOAD ----
        Elf64_Phdr* owner = nullptr;
        for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
            Elf64_Phdr* ph = (Elf64_Phdr*)(elf_base + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;
            if (sh.sh_addr >= ph->p_vaddr &&
                sh.sh_addr + sh.sh_size <= ph->p_vaddr + ph->p_memsz) {
                owner = ph;
                break;
            }
        }
        if (!owner) {
            bsp_kout << "[Phase3a] " << kmmu_name << " not in any PT_LOAD" << kendl;
            return -13;
        }

        // ---- b. 内核区间确认（is_kernel_address） ----
        uint64_t sz  = align_up(sh.sh_size, 4096);
        uint64_t npg = sz >> 12;
        pgaccess acc;
        acc.is_kernel     = 1;
        acc.is_writeable  = (sh.sh_flags & SHF_WRITE) ? 1 : 0;
        acc.is_readable   = 1;
        acc.is_executable = (sh.sh_flags & SHF_EXECINSTR) ? 1 : 0;
        acc.is_global     = 1;
        acc.cache_strategy = WB;
        vm_interval iv = {.vpn = sh.sh_addr >> 12, .ppn = 0, .npages = npg, .access = acc};
        if (!iv.is_kernel_address()) {
            bsp_kout << "[Phase3a] " << kmmu_name << " not kernel range 0x"
                     << HEX << sh.sh_addr << DEC << kendl;
            return -15;
        }

        // ---- c. 独立分配物理页 + 内容安置 ----
        if (sh.sh_type != SHT_NOBITS) {
            if (sh.sh_offset + sh.sh_size > kelf_pages * 4096ULL) return -17;  // 越界
        }
        // 物理分配对齐上调：取 min(段虚拟基址自然对齐, 段大小上界 2 的幂)，封顶 1GB。
        // 例：.text_main sh_addr=0xFFFF800000000000 基址 2MB 对齐 → 分配也 2MB 对齐，
        //     使 kmmu 映射能上 2MB/1GB 大页（kld.ld 中主段起始均按 ALIGN(2M) 布局）。
        int align_log2 = 12;
        while (align_log2 < 30 && (sh.sh_addr & (1ULL << align_log2)) == 0) {
            align_log2++;   // sh_addr 最低置位位 → 自然基址对齐
        }
        int sz_log = 12;
        while (sz_log < 30 && (sz >> sz_log) > 1) {
            sz_log++;       // floor(log2(sz))：页面粒度上界
        }
        if (sz_log < align_log2) align_log2 = sz_log;
        loc_code_t err = 0;
        phyaddr_t pa = init_bcb_juvenile::alloc(npg, align_log2, &err);
        if (pa == 0) {
            bsp_kout << "[Phase3a] " << kmmu_name << " alloc OOM" << kendl;
            return -16;
        }
        page_allocator::pages_set({pa, sz}, page_state_t::kernel_persisit);
        if (sh.sh_type == SHT_NOBITS) {
            ksetmem_8((void*)(uint64_t)pa, 0, sz);   // .bss：清零
        } else {
            ksystemramcpy((void*)(uint64_t)(kimg_pbase + sh.sh_offset),
                          (void*)(uint64_t)pa, sh.sh_size);
            if (sz > sh.sh_size) {
                ksetmem_8((void*)(uint64_t)(pa + sh.sh_size), 0, sz - sh.sh_size);
            }
        }

        // 写回真实 PA 到所属 LOAD 的 p_paddr（kld.ld 下段⊇LOAD 1:1，整段即真实 PA）
        owner->p_paddr = pa;

        // ---- d. 进入 kmmu（名字台账，PERSISTENT 交 kernel 认领） ----
        int map_rc = kmmu->map(kernel_mmu::make_entry(pa, sh.sh_addr, sz, acc,
                                                      kmmu_name, KMMU_ENTRY_FLAG_PERSISTENT));
        if (map_rc != 0) {
            bsp_kout << "[Phase3a] " << kmmu_name << " kmmu map fail " << map_rc << kendl;
            return map_rc;
        }

        // ---- e. 登记资产树（handoff 清单）：与 kmmu 台账同 arg0 的持久 desc ----
        //      desc = vm_interval（init 堆分配，随条目生命周期持久），路由 arg1 = "mem"。
        vm_interval* adesc = new vm_interval{ .vpn   = sh.sh_addr >> 12,
                                              .ppn   = pa >> 12,
                                              .npages = npg,
                                              .access = acc };
        if (!asset_reg_add(asset_name, adesc)) {
            bsp_kout << "[Phase3a] asset dup: " << asset_name << kendl;
            return -18;
        }

        uint64_t vend = sh.sh_addr + sh.sh_size;
        if (vend > kernel_vaddr_top) kernel_vaddr_top = vend;
        bsp_kout << "[Phase3a] " << kmmu_name << ": v=0x" << HEX << sh.sh_addr
                 << " p=0x" << pa << " sz=0x" << sz << DEC << kendl;
        return 0;
    };

    // 对四个目标段分别命中（每个命中首个匹配段）并加载
    for (int t = 0; t < k_sec_count; t++) {
        const sec_target_t& tg = k_sec_targets[t];
        bool matched = false;
        for (Elf64_Half si = 0; si < ehdr->e_shnum; si++) {
            if (shdr[si].sh_type == SHT_NULL) continue;
            const char* sname = shstrtab + shdr[si].sh_name;
            if (!sec_name_hit(sname, tg)) continue;
            matched = true;
            int rc = load_section(shdr[si], tg.kmmu_name, tg.asset_name);
            if (rc != 0) {
                bsp_kout << "[Phase3a] section load fail: " << tg.name_key << " rc=" << rc << kendl;
                init_fatal::halt(SRC_LOC());
            }
            break;
        }
        if (!matched) {
            bsp_kout << "[Phase3a] section not found: " << tg.name_key << kendl;
            init_fatal::halt(SRC_LOC());
        }
    }

    // ---- 4. 解禁 va_alloc ----
    g_va_alloc_base = align_up(kernel_vaddr_top, 0x200000);

    // ---- 5. kIMG 作为 movable 资产（不再专用映射——恒等窗口已覆盖） ----
    //      movable_file_entry_t{ base_ppn, size }：offset 0 起即文件内容，
    //      访问经恒等映射（[0x1000, dram_top)）/ Kspace_phyaddr_access_window。
    {
        movable_file_entry_t* kimg_desc = new movable_file_entry_t{
            .base_ppn = kimg_pbase >> 12,
            .size     = kelf_sz,
        };
        if (!asset_reg_add("kimg movable", kimg_desc)) {
            bsp_kout << "[Phase3a] asset dup: kimg" << kendl;
            init_fatal::halt(SRC_LOC());
        }
    }

    bsp_kout << "[Phase3a] done: kIMG(transient) 0x" << (void*)kimg_pbase
             << " size=" << kelf_sz << " vaddr_top=" << (void*)kernel_vaddr_top << kendl;

    // 资产树 dump（handoff 清单，字典序）
    if (g_asset_registry) {
        bsp_kout << "[Phase3a] asset tree (" << g_asset_registry->size() << "):";
        for (auto it = g_asset_registry->begin(); it != g_asset_registry->end(); ++it) {
            bsp_kout << " [" << it->name << "]";
        }
        bsp_kout << kendl;
    }
    return 0;
}

// ============================================================================
// Phase 3b (串行): 恒等映射 + 区间分配 + 架构信息收集 → 产出进资产容器 + ctx_intervals
// ============================================================================
// 返回 loc_code_t（0 成功）；内部失败 return SRC_LOC()，由 init_main init_fatal。
// ctx_intervals 经 out-param 输出（过渡；资产本体已在容器内，iv 供 info_fill/4.5 过渡用）。
static loc_code_t phase_3b(kernel_mmu* kmmu, BootInfoHeader* header,
                           const ctx_early_mem* em, ctx_intervals* iv_out) {
    ctx_intervals iv = {};

    // --- 清空 extra VM 数组 ---
    iv.extra_vm_arr   = new loaded_VM_interval[8];
    iv.extra_vm_count = 0;

    auto add_extra = [&](phyaddr_t p, vaddr_t v, uint64_t sz, uint32_t id, pgaccess a) {
        iv.extra_vm_arr[iv.extra_vm_count++] = {p, v, sz, id, a};
    };

    bsp_kout << "[Phase3b] start..." << kendl;

    // ---- 恒等映射: [4KB, dram_top) WB+RWX（短暂存在，不进 info header / 资产容器） ----
    //     仅用于 init.elf 自身 CR3 切换的极小窗口 + 跳转 kernel.elf 后访问信息包。
    //     kernel.elf 接手后通过 Kspace_phyaddr_access_window 访问物理地址。
    {
        phyaddr_t top = page_allocator::dram_top();
        uint64_t  sz  = top - 0x1000;
        pgaccess id_a = KSPACE_RWX_NG_ACCESS;
        kmmu->map(kernel_mmu::make_entry(0x1000, 0x1000, sz, id_a,
                                         "identity_map", KMMU_ENTRY_FLAG_TRANSIENT));
        bsp_kout << "[Phase3b] identity: [0x1000, 0x" << top << ") WB+RWX (transient)" << kendl;
    }

    // ---- FPA_bitmaps（mem 资产：保留提前映射，让 kernel 免重映射改 BCB 状态） ----
    //      例外：FPA_bitmaps 描述 BCB 自身的位图区，从 basic_allocator 分配，
    //      不经过 init_bcb_juvenile（避免自描述自消费）。
    {
        uint64_t tp  = page_allocator::total_page_count();
        uint64_t sz  = align_up((tp*3)>>3, 4096);//一个页框3bit的预算
        uint64_t npg = sz >> 12;
        phyaddr_t p  = basic_allocator::pages_alloc(sz, 12);
        if (!p) { bsp_kout << "FPA OOM" << kendl; return SRC_LOC(); }
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, sz);
        vaddr_t v = va_alloc_up(sz, 12);
        kmmu->map(kernel_mmu::make_entry(p, v, sz, KSPACE_RW_ACCESS,
                                         "fpa_bitmaps", KMMU_ENTRY_FLAG_PERSISTENT));
        iv.FPA_bitmaps = {.vpn = v >> 12, .ppn = p >> 12,
                          .npages = npg, .access = KSPACE_RW_ACCESS};
        asset_reg_add("fpa_bitmaps mem",
                      new vm_interval{ .vpn = v >> 12, .ppn = p >> 12,
                                       .npages = npg, .access = KSPACE_RW_ACCESS });
        bsp_kout << "[Phase3b] FPA_bitmaps: p=0x" << p << " v=" << (void*)v << " sz=" << (void*)sz << kendl;
    }

    // ---- log_buffer（mem 资产：日志输出连续性，保留提前映射） ----
    {
        uint64_t sz  = LOGBUFFER_SIZE;
        uint64_t npg = sz >> 12;
        loc_code_t alloc_err = 0;
        phyaddr_t p  = init_bcb_juvenile::alloc(npg, 21, &alloc_err);
        if (!p) { bsp_kout << "log OOM" << kendl; return SRC_LOC(); }
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, sz);
        vaddr_t v = va_alloc_up(sz, 21);
        kmmu->map(kernel_mmu::make_entry(p, v, sz, KSPACE_RW_ACCESS,
                                         "log_buffer", KMMU_ENTRY_FLAG_PERSISTENT));
        iv.log_buffer = {.vpn = v >> 12, .ppn = p >> 12,
                         .npages = npg, .access = KSPACE_RW_ACCESS};
        asset_reg_add("log_buffer mem",
                      new vm_interval{ .vpn = v >> 12, .ppn = p >> 12,
                                       .npages = npg, .access = KSPACE_RW_ACCESS });
        bsp_kout << "[Phase3b] log_buffer: p=0x" << p << " v=" << (void*)v << kendl;
    }

    // ---- symtable_file (probe + initramfs_lookup) → movable_file_entry_t ----
    //      movable = 纯物理描述符 {base_ppn, size}，不做 KMMU 映射（经恒等/high 窗口访问）
    {
        phyaddr_t sym_in_ramfs = 0; uint64_t sym_sz = 0;
        if (em->ramfs_base) {
            auto* rh = (const initramfs_header*)(uint64_t)em->ramfs_base;
            sym_in_ramfs = initramfs_lookup(rh, "/ksymbols.bin", &sym_sz);
        }
        if (sym_in_ramfs == 0 || sym_sz == 0) {
            bsp_kout << "[Phase3b] ksymbols.bin not found" << kendl;
        } else {
            uint64_t sz  = align_up(sym_sz, 4096);
            uint64_t npg = sz >> 12;
            loc_code_t alloc_err = 0;
            phyaddr_t p  = init_bcb_juvenile::alloc(npg, 21, &alloc_err);
            if (!p) { bsp_kout << "sym OOM" << kendl; return SRC_LOC(); }
            page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
            ksystemramcpy((void*)(uint64_t)sym_in_ramfs, (void*)(uint64_t)p, sym_sz);
            iv.symtable_file = { .base_ppn = p >> 12, .size = sym_sz };
            asset_reg_add("ksymbols movable",
                          new movable_file_entry_t{ .base_ppn = p >> 12, .size = sym_sz });
            bsp_kout << "[Phase3b] symtable: p=0x" << p << " size=" << sym_sz << kendl;
        }
    }

    // ---- initramfs_file → movable_file_entry_t（纯物理描述符，不做 KMMU 映射） ----
    {
        if (em->ramfs_base && em->ramfs_size) {
            iv.initramfs_file = { .base_ppn = em->ramfs_base >> 12,
                                  .size     = em->ramfs_size };
            asset_reg_add("initramfs movable",
                          new movable_file_entry_t{ .base_ppn = em->ramfs_base >> 12,
                                                    .size     = em->ramfs_size });
            bsp_kout << "[Phase3b] initramfs: p=" << (void*)em->ramfs_base
                     << " size=" << em->ramfs_size << kendl;
        }
    }

    // ---- x86 arch_specify ----
    // GOP
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].device_info != PASS_THROUGH_DEVICE_GRAPHICS_INFO) continue;
        auto* gfx = (GlobalBasicGraphicInfoType*)header->pass_through_devices[i].specify_data;
        if (!gfx) break;
        ksystemramcpy(gfx, &iv.arch_info.gop_info, sizeof(*gfx));
        uint64_t fb_sz  = align_up(gfx->FrameBufferSize, 4096);
        uint64_t fb_npg = fb_sz >> 12;
        phyaddr_t fb_p  = (phyaddr_t)gfx->FrameBufferBase;
        vaddr_t fb_v    = va_alloc_up(fb_sz, 21);
        kmmu->map(kernel_mmu::make_entry(fb_p, fb_v, fb_sz, KSPACE_RW_WC_ACCESS,
                                         "gop_framebuffer", KMMU_ENTRY_FLAG_PERSISTENT));
        iv.arch_info.Gop_vbase = fb_v;
        asset_reg_add("gop_framebuffer mem",
                      new vm_interval{ .vpn = fb_v >> 12, .ppn = fb_p >> 12,
                                       .npages = fb_npg, .access = KSPACE_RW_WC_ACCESS });
        bsp_kout << HEX << "[Phase3b] GOP fb: p=0x" << fb_p << " v=0x" << fb_v << kendl;
        break;
    }

    // HPET
    if (em->xsdt_base) {
        auto* xsdt = (const XSDT_Table*)(uint64_t)em->xsdt_base;
        uint64_t ec = (xsdt->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
        for (uint64_t i = 0; i < ec; i++) {
            auto* hdr = (const ACPI_Table_Header*)(uint64_t)xsdt->Entry[i];
            if (!hdr) continue;
            if (*(const uint32_t*)hdr->Signature == HPET_SIGNATURE_UINT32) {
                auto* ht = (const HPET_Table*)(uint64_t)xsdt->Entry[i];
                phyaddr_t hp = ht->Base_Address;
                if (hp) {
                    vaddr_t hv = va_alloc_up(0x1000, 12);
                    kmmu->map(kernel_mmu::make_entry(hp, hv, 0x1000, KSPACE_RW_UC_ACCESS,
                                                     "hpet_mmio", KMMU_ENTRY_FLAG_PERSISTENT));
                    iv.arch_info.hpet_mmio = {.vpn = hv >> 12, .ppn = hp >> 12,
                                              .npages = 1, .access = KSPACE_RW_UC_ACCESS};
                    asset_reg_add("hpet_mmio mem",
                                  new vm_interval{ .vpn = hv >> 12, .ppn = hp >> 12,
                                                   .npages = 1, .access = KSPACE_RW_UC_ACCESS });
                    bsp_kout  << "[Phase3b] HPET MMIO: p=" << (void*)hp << " v=" << (void*)hv << kendl;
                }
                break;
            }
        }
    }
    iv.arch_info.XSDT_base = em->xsdt_base;

    // conjunc_GSs: 每个处理器分配一个 gs_complex_t
    {
        uint64_t total_bytes    = header->logical_processor_count * GS_COMPLEX_STRIDE;
        uint64_t npg            = total_bytes >> 12;
        loc_code_t alloc_err = 0;
        phyaddr_t pbase         = init_bcb_juvenile::alloc(npg, 12, &alloc_err);
        page_allocator::pages_set({pbase, npg << 12}, page_state_t::kernel_persisit);
        vaddr_t  vbase          = va_alloc_up(total_bytes, 12);
        ksetmem_8((void*)(uint64_t)pbase, 0, total_bytes);
        iv.arch_info.conjunc_GSs = {
            .vpn    = vbase >> 12,
            .ppn    = pbase >> 12,
            .npages = npg,
            .access = KSPACE_RW_ACCESS
        };
        kmmu->map(kernel_mmu::make_entry(pbase, vbase, total_bytes, KSPACE_RW_ACCESS,
                                         "gs_complexes", KMMU_ENTRY_FLAG_PERSISTENT));
        asset_reg_add("gs_complexes mem",
                      new vm_interval{ .vpn = vbase >> 12, .ppn = pbase >> 12,
                                       .npages = npg, .access = KSPACE_RW_ACCESS });
        bsp_kout << "[Phase3b] conjunc_GSs: vaddr=" << (void*)(uint64_t)vbase
                 << " paddr=" << (void*)(uint64_t)pbase
                 << " size=0x" << (uint64_t)(npg << 12) << kendl;
    }

    // hardware stacks: 降级为纯物理区间（p_interval 穿越，无 KMMU 映射、不链入 GS 复合体）
    //     kernel.elf 自行决断如何映射 + 链接到 conjunc_GSs 的 TSS。
    {
        uint64_t stack_stride   = sizeof(per_processor_hardware_stack_t);  // 含 5 guard 页
        uint64_t total_phys     = header->logical_processor_count * stack_stride + 4096;  // + 尾 guard
        uint64_t hd_pages       = total_phys >> 12;
        loc_code_t alloc_err = 0;
        phyaddr_t hd_pbase      = init_bcb_juvenile::alloc(hd_pages, 12, &alloc_err);
        if (!hd_pbase) { bsp_kout << "hdstacks OOM" << kendl; return SRC_LOC(); }
        page_allocator::pages_set({hd_pbase, hd_pages << 12}, page_state_t::kernel_persisit);
        // arch_info 字段暂留（vbase=0：降级为物理区间无 VA）；容器以 "hdstacks pint" 承载 p_interval
        iv.arch_info.hdstacks_interval_pbase  = hd_pbase;
        iv.arch_info.hdstacks_4kbpgs_count    = hd_pages;
        iv.arch_info.hdstacks_interval_vbase  = 0;
        asset_reg_add("hdstacks pint",
                      new p_interval{ .ppn = hd_pbase >> 12, .pages_count = hd_pages });
        bsp_kout << "[Phase3b] hdstacks: paddr=0x" << HEX << hd_pbase
                 << " pages=" << hd_pages << DEC << kendl;
    }

    // ---- bsp_entry_stack（mem 资产）：BSP 跳入内核的入口栈，复用 VM_ID_BSP_INIT_STACK 32KB 语义 ----
    {
        constexpr uint64_t stack_sz = BSP_INIT_STACK_SIZE;      // 32KB
        uint64_t npg = stack_sz >> 12;
        loc_code_t alloc_err = 0;
        phyaddr_t p  = init_bcb_juvenile::alloc(npg, BSP_INIT_STACK_ALIGN_LOG2, &alloc_err);
        if (!p) { bsp_kout << "bsp_entry_stack OOM" << kendl; return SRC_LOC(); }
        page_allocator::pages_set({p, stack_sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, stack_sz);
        vaddr_t v = va_alloc_up(stack_sz, 21);
        kmmu->map(kernel_mmu::make_entry(p, v, stack_sz, KSPACE_RW_ACCESS,
                                         "bsp_entry_stack", KMMU_ENTRY_FLAG_PERSISTENT));
        asset_reg_add("bsp_entry_stack mem",
                      new vm_interval{ .vpn = v >> 12, .ppn = p >> 12,
                                       .npages = npg, .access = KSPACE_RW_ACCESS });
        bsp_kout << "[Phase3b] bsp_entry_stack: p=0x" << p << " v=" << (void*)v << kendl;
    }
    // ---- high_window: [0, dram_top) → 1GB 对齐高 VA（kernel 物理访问主窗口） ----
    {
        phyaddr_t top = page_allocator::dram_top();
        uint64_t  sz  = align_up(top, 0x40000000ULL);
        vaddr_t   v   = va_alloc_up(sz, 30);
        kmmu->map(kernel_mmu::make_entry(0, v, sz, KSPACE_RW_ACCESS,
                                         "phyaddr_window", KMMU_ENTRY_FLAG_PERSISTENT));
        bsp_kout << "[Phase3b] high_window: [0," << (void*)top << ") at" << (void*)v << kendl;
        iv.Kspace_phyaddr_access_window={
            .vpn    = v >> 12,
            .ppn    = 0,
            .npages = sz >> 12,
            .access = KSPACE_RW_ACCESS
        };
        asset_reg_add("phyaddr_window mem",
                      new vm_interval{ .vpn = v >> 12, .ppn = 0,
                                       .npages = sz >> 12, .access = KSPACE_RW_ACCESS });
        (void)v;
    }
    // ---- Kspace_phyaddr_access_window: [0, dram_top) → Kspace VA ----
    //     不是恒等映射！将整个物理地址区间映射到内核高位空间（1GB 对齐），
    //     供 kernel.elf 的 PhyAddrAccessor / 页表重建时访问任意物理地址。
    bsp_kout << "[Phase3b] done: " << iv.extra_vm_count << " extra VM entries" << kendl;
    *iv_out = iv;
    return 0;
}

// ============================================================================
// Phase 4 (串行): 构建 init_to_kernel_header（extern，定义在 info_fill.cpp）
// ============================================================================
// 签名：kl（ctx_kernel_loaded 已废弃）→ kmmu 直接传参；kIMG 字段从资产容器取
extern phyaddr_t build_init_to_kernel_header(
    phyaddr_t                pkt_pbase,
    uint64_t                 pkt_pages,
    BootInfoHeader*          header,
    kernel_mmu*              kmmu,
    const ctx_intervals*     iv,
    phymem_segment*          seg_view,
    uint64_t                 seg_count);

// ============================================================================
// Phase 4.5 (自裁 → CR3 切换 → gs_complex_load_gdt_tss → iretq)
// ============================================================================
static void phase_45_finalize(kernel_mmu* kmmu, phyaddr_t info_pbase,
                              const ctx_intervals* iv) {
    // 4.5-1: relinquish
    phyaddr_t mm_pb; uint64_t mm_pc;
    page_allocator::relinquish_mem_map(&mm_pb, &mm_pc);

    // 4.5-2: pages_arr
    uint64_t mm_sz   = mm_pc * sizeof(page);
    uint64_t mm_npg  = align_up(mm_sz, 4096) >> 12;
    init_to_kernel_header* h = (init_to_kernel_header*)(uint64_t)info_pbase;
    h->pages_arr = {.vpn = iv->pages_arr_vbase >> 12, .ppn = mm_pb >> 12,
                    .npages = mm_npg, .access = KSPACE_RW_ACCESS};
    kmmu->map({mm_pb, iv->pages_arr_vbase, align_up(mm_sz, 4096)}, KSPACE_RW_ACCESS);
    bsp_kout << "[Phase4.5] pages_arr: paddr=" << (void*)(uint64_t)mm_pb
             << " vaddr=" << (void*)(uint64_t)iv->pages_arr_vbase
             << " size=0x" << (uint64_t)mm_sz << kendl;
    
    // 4.5-3: CR3
    phyaddr_t root = kmmu->get_root_table_base();
    bsp_kout << "[Phase4.5] CR3 <- 0x" << root << kendl;
    asm volatile("sfence");
    asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");
    
    // 4.5-4: 构建所有处理器的 GDT/TSS 到 GS 复合体（恒等映射，pbase == vbase）
    {
        vaddr_t gs_base  = iv->arch_info.conjunc_GSs.vbase();
        wrmsr_func(msr::syscall::IA32_GS_BASE,gs_base);//提前给bsp加载好gs
        wrmsr_func(msr::syscall::IA32_KERNEL_GS_BASE,gs_base);//提前给bsp加载好gs
        uint32_t  pcount   = h->logical_processor_count;
        constexpr TSSDescriptorEntry ktss = {
            .limit = sizeof(TSSentry), .base0 = 0, .base1 = 0,
            .type = 0x9, .zero = 0, .dpl = 0, .p = 1,
            .limit1 = (sizeof(TSSentry) - 1) >> 16,
            .avl = 0, .reserved = 0, .g = 0, .base2 = 0, .base3 = 0, .reserved2 = 0
        };
        for (uint32_t p = 0; p < pcount; p++) {
            gs_complex_t* cx = (gs_complex_t*)(uint64_t)(gs_base + p * GS_COMPLEX_STRIDE);
            per_processor_hardware_stack_t* st = cx->stacks_ptr;
            vaddr_t st_va=reinterpret_cast<vaddr_t>(st);
            cx->slots[PROCESSOR_ID_GS_INDEX]=p;
            // GDT 条目
            cx->gdt[K_cs_idx]    = kspace_CS_entry;
            cx->gdt[K_ds_ss_idx] = kspace_DS_SS_entry;
            cx->gdt[U_cs_idx]    = userspace_CS_entry;
            cx->gdt[U_ds_ss_idx] = userspace_DS_SS_entry;
            // TSS 描述符 + 基址 → 恒等映射下 &cx->tss
            vaddr_t tss_va = reinterpret_cast<vaddr_t>(&cx->tss);
            cx->tss_descriptor = ktss;
            cx->tss_descriptor.base0 = (uint32_t)tss_va & 0xFFFF;
            cx->tss_descriptor.base1 = (tss_va >> 16) & 0xFF;
            cx->tss_descriptor.base2 = (tss_va >> 24) & 0xFF;
            cx->tss_descriptor.base3 = tss_va >> 32;
            // TSS 栈指针 → hdstacks 内嵌栈顶部
            cx->tss.rsp0   = st_va+RSP0_BOTTOM_OFF;
            cx->tss.ist[0] = 0;
            cx->tss.ist[1] = st_va+IST1_BOTTOM_OFF;
            cx->tss.ist[2] = st_va+IST2_BOTTOM_OFF;
            cx->tss.ist[3] = st_va+IST3_BOTTOM_OFF;
            // stacks_ptr + slot[0]
            cx->stacks_ptr = st;
            cx->slots[PROCESSOR_RSP0_STACK_BTM_IDX] = cx->tss.rsp0 ;
        }
        bsp_kout << "[Phase4.5] prepare " << pcount << " GS complexes" << kendl;
    }
    //outb(0xDB, 0x80);
    // 4.5-5: 加载 BSP 的 GDT + TSS（上一步已完全构建，此步仅 LGDT+LTR）
    {
        gs_complex_t* bsp_cx = (gs_complex_t*)(uint64_t)(iv->arch_info.conjunc_GSs.vbase());
        bsp_kout << "[Phase4.5] LGDT+LTR: complex @ 0x" << HEX << (uint64_t)bsp_cx << kendl;
        gs_complex_load_gdt_tss(bsp_cx);
    }
    
    // 4.5-6: init_jump_to_kernel — 用 BSP 的 rsp0 栈构建 x64_standard_context 后跳入 kernel.elf
    {
        // entry_vaddr 隐式状态：从资产容器读 scalar
        const asset_entry_t* ev = g_asset_registry->read("entry_vaddr");
        if (!ev || !ev->data) {
            bsp_kout << "[Phase4.5] entry_vaddr asset missing" << kendl;
            init_fatal::halt(SRC_LOC());
        }
        uint64_t entry_vaddr = *(uint64_t*)ev->data;

        // kernel_entry_stack 已废弃，改用 BSP GS 复合体内嵌的 rsp0 栈
        gs_complex_t* bsp = (gs_complex_t*)(uint64_t)iv->arch_info.conjunc_GSs.vbase();
        vaddr_t bsp_rsp0  = bsp->tss.rsp0;
        // 与 create_kthread 同款帧，区别：RDI=info_pbase, RFLAGS=KERNEL_INIT_RFLAGS
        x64_standard_context_v2 ctx = {};
        ctx.rdi                     = info_pbase;
        ctx.core_ctx.idtctx.iret.rip        = entry_vaddr;
        ctx.core_ctx.idtctx.iret.cs         = K_cs_idx << 3;
        ctx.core_ctx.idtctx.iret.rflags     = KERNEL_INIT_RFLAGS;
        ctx.core_ctx.idtctx.iret.rsp        = bsp_rsp0;
        ctx.core_ctx.idtctx.iret.ss         = K_ds_ss_idx << 3;
        bsp_kout << "[Phase4.5] init_jump_to_kernel: entry=" << (void*)(uint64_t)entry_vaddr
                 << " rsp=" << (void*)(uint64_t)bsp_rsp0
                 << " rdi=" << (void*)(uint64_t)info_pbase << kendl;
        init_jump_to_kernel(&ctx);
    }
}

// ============================================================================
// init — 主入口
// ============================================================================
extern "C" void init_main(BootInfoHeader* header) {
    if (init_io_and_heap(header) != 0) init_fatal::halt(SRC_LOC());    
    auto em = init_memory_early(header);
    if (!em.xsdt_base && /* memory early 出错检测 */ 0) init_fatal::halt(SRC_LOC());
    // 注意: init_memory_early 返回空 struct 时 xsdt_base=0 属于正常（ACPI 找不到），
    // 不 halt。只有 basic_allocator/page_allocator 失败才会内部 halt。

    // Phase 2.5：initramfs 不再高位搬迁——原位 mark_used 钉住，直接引用 UEFI 加载位置
    initramfs_mark_used(header, &em);

    // Phase 3a + 3b 共享同一 KMMU，由 Phase 3a 初始化，Phase 3b 使用
    kernel_mmu* kmmu = new kernel_mmu(arch_enums::x86_64_PGLV4);

    // 资产树（handoff 清单）显式构造——全局裸指针零动态初始化
    g_asset_registry = new init_asset_registry_t();

    if (phase_3a_load_kernel(kmmu, &em, header) != 0) init_fatal::halt(SRC_LOC());
    ctx_intervals iv;
    if (phase_3b(kmmu, header, &em, &iv) != 0) init_fatal::halt(SRC_LOC());
    
    // Phase 4: 构造信息包
    uint64_t segcnt = 0;
    phymem_segment* pure_view = basic_allocator::get_pure_memory_view(&segcnt);
    constexpr uint64_t PKT_PAGES = 4;
    phyaddr_t pkt = page_allocator::available_meminterval_probe(PKT_PAGES, 12);
    if (!pkt) { bsp_kout << "pkt OOM" << kendl; init_fatal::halt(SRC_LOC()); }
    pkt -= (PKT_PAGES << 12);
    ksetmem_8((void*)(uint64_t)pkt, 0, PKT_PAGES * 4096);
    page_allocator::pages_set({pkt, PKT_PAGES * 4096}, page_state_t::kernel_persisit);

    if (!build_init_to_kernel_header(pkt, PKT_PAGES, header,
                                     kmmu, &iv, pure_view, segcnt)) {
        bsp_kout << "build_init_to_kernel_header failed" << kendl; init_fatal::halt(SRC_LOC());
    }

    bsp_kout << "[Phase4] info_pkt: paddr=" << (void*)(uint64_t)pkt
             << " pages=" << (uint32_t)PKT_PAGES
             << " phymem_segments=" << (uint64_t)segcnt
             << " processors=" << (uint32_t)header->logical_processor_count << kendl;

    // Phase 4.5
    phase_45_finalize(kmmu, pkt, &iv);
    init_fatal::halt(SRC_LOC());
}
