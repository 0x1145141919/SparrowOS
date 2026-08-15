#include "abi/boot.h"
#include "init/load_kernel.h"
#include "init/init_asset_registry.h"
#include "init/page_allocator_v2.h"
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
#include "firmware/gSTResloveAPIs.h"   // RSDP_struct, ACPI_20_TABLE_GUID
#include "arch/x86_64/init/page_table.h"// modify_access
#include "memory/memory_base.h"
#include "init/init_phase_ctx.h"
#include "init/phase_3.h"
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
// g_va_alloc_base 定义见下方（Phase 3b 注释块前），声明在 init/phase_3.h

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

    // 2d. page_allocator_v2 顶替 init_bcb_juvenile —— 可穿越页级分配器（分配职责唯一来源）
    //     纯洁视图 → mem_map 状态数组（1B/页）；mem_map 终态整体作为 pages_arr 资产穿越，
    //     kernel 收养后按语义态（kernel_persisit/kernel_file_property/free...）自建分配器。
    //     与 BCB 路线的根本区别：穿越的是"每页物理事实"，不是分配器拓扑。
    {
        uint64_t segcnt = 0;
        phymem_segment* view = basic_allocator::get_pure_memory_view(&segcnt);
        if (!view || segcnt == 0) {
            bsp_kout << "[INIT] get_pure_memory_view failed" << kendl; return em;
        }

        // DRAM 物理上界：freeSystemRam 最高末尾（纯视图快照，替代 page_allocator::dram_top）
        em.dram_top = 0;
        for (uint64_t i = 0; i < segcnt; i++) {
            if (view[i].type != PHY_MEM_TYPE::freeSystemRam) continue;
            uint64_t end = view[i].start + view[i].size;
            if (end > em.dram_top) em.dram_top = end;
        }

        if (page_allocator_v2::init() != 0) {
            bsp_kout << "[INIT] page_allocator_v2::init failed" << kendl; return em;
        }
        bsp_kout << "[INIT] page_allocator_v2 up: managed=" << page_allocator_v2::total_page_count()
                 << " free=" << page_allocator_v2::free_page_count()
                 << " mem_map@0x" << HEX << page_allocator_v2::get_mem_map_pbase() << DEC << kendl;

        // init() 已自标记 init 镜像 / 区间数组 / low-1MB。此处补 header + loaded files：
        // 纯视图快照里它们仍是 freeSystemRam，账本不钉住分配器就会把它们交出去。
        // 语义态：header/loaded files 是 init 临时财产（不穿越，4.5 归还 free）→ init_tmp_property。
        if (page_allocator_v2::pages_set(
                {(uint64_t)header, (uint64_t)header->total_pages_count * 4096},
                page_state_t::init_tmp_property) != 0) {
            bsp_kout << "[INIT] header pages_set failed" << kendl; return em;
        }
        for (uint64_t i = 0; i < header->loaded_file_count; i++) {
            if (header->loaded_files[i].file_type == LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD) continue;
            if (page_allocator_v2::pages_set(
                    {(uint64_t)header->loaded_files[i].raw_data,
                     align_up(header->loaded_files[i].file_size, 4096)},
                    page_state_t::init_tmp_property) != 0) {
                bsp_kout << "[INIT] loaded_file[" << i << "] pages_set failed" << kendl;
                return em;
            }
        }
    }

    bsp_kout << "[INIT] Phase 2: memory ready, free pages="
             << page_allocator_v2::free_page_count() << kendl;
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

    // 语义态提交：initramfs = kernel_file_property（VFS 消化后可转 user_file）。
    // 防御性页对齐（raw_data 起点/大小不保证 4K 对齐），对齐后覆盖区间 ⊇ 资产区间。
    const phyaddr_t ramfs_lo = em->ramfs_base & ~0xFFFull;
    const phyaddr_t ramfs_hi = (em->ramfs_base + em->ramfs_size + 0xFFFull) & ~0xFFFull;
    if (page_allocator_v2::pages_set({ramfs_lo, ramfs_hi - ramfs_lo},
                                     page_state_t::kernel_file_property) != 0) {
        bsp_kout << "[INIT] initramfs pages_set failed" << kendl;
        em->ramfs_base = 0;
        em->ramfs_size = 0;
        return;
    }
    bsp_kout << "[INIT] initramfs in-place: base=0x" << HEX << em->ramfs_base
             << " size=0x" << em->ramfs_size << DEC << kendl;
}
uint64_t g_va_alloc_base=0;

// ============================================================================
// Phase 4 (串行): 构建 init_to_kernel_header_v2（extern，定义在 info_fill.cpp）
// ============================================================================
// 签名：kmmu / iv 已移除（v2 不需要——一等字段只剩 phymem_segments /
// properties_table / free_segs_descriptors_table，其余全部走资产注册表）。
extern phyaddr_t build_init_to_kernel_header(
    phyaddr_t                pkt_pbase,
    uint64_t                 pkt_pages,
    BootInfoHeader*          header,
    phymem_segment*          seg_view,
    uint64_t                 seg_count);

// ============================================================================
// Phase 4.5 (自裁 → CR3 切换 → gs_complex_load_gdt_tss → iretq)
// ============================================================================
static void phase_45_finalize(kernel_mmu* kmmu, phyaddr_t info_pbase,
                              const ctx_intervals* iv, uint64_t entry_vaddr,
                              BootInfoHeader* header) {
    // 4.5-0: 自裁——init.elf 的财产不穿越。
    //   移交资产（注册表 + pages_arr 账本）不含 init.elf 自身信息，kernel 无从
    //   回收 init 镜像与 BootInfoHeader；故 init 在移交账本里抹除这两个区域，
    //   归还为可用页。归还 = pages_set(_, free) 翻状态，不改映射——init 仍在其上
    //   执行直至跳转完成（替代旧 init_bcb_juvenile::free）。
    {
        auto erase_pages = [](phyaddr_t base, uint64_t byte_size) {
            if (base == 0 || byte_size == 0) return;
            const phyaddr_t lo = base & ~0xFFFull;
            const phyaddr_t hi = (base + byte_size + 0xFFFull) & ~0xFFFull;
            for (phyaddr_t p = lo; p < hi; p += 0x1000)
                page_allocator_v2::pages_set({p, 0x1000}, page_state_t::free);
        };
        const uint64_t init_img_sz = (uint64_t)&__init_heap_end - (uint64_t)&__init_text_start;
        erase_pages((uint64_t)&__init_text_start, align_up(init_img_sz, 4096));
        erase_pages((uint64_t)header, (uint64_t)header->total_pages_count * 4096);
        bsp_kout << "[Phase4.5] self-eliminated: init image + BootInfoHeader erased from BCB" << kendl;
    }

    // 4.5-1: CR3
    // 回收职能由 mem_map 账本（page_allocator_v2 状态数组，pages_arr mem 资产）接替，
    // kernel 收养账本后按每页状态正常回收。
    phyaddr_t root = kmmu->get_root_table_base();
    bsp_kout << "[Phase4.5] CR3 <- 0x" << root << kendl;
    asm volatile("sfence");
    asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");
    
    // 4.5-2: 构建所有处理器的 GDT/TSS 到 GS 复合体（恒等映射，pbase == vbase）
    {
        vaddr_t gs_base  = iv->arch_info.conjunc_GSs.vbase();
        wrmsr_func(msr::syscall::IA32_GS_BASE,gs_base);//提前给bsp加载好gs
        wrmsr_func(msr::syscall::IA32_KERNEL_GS_BASE,gs_base);//提前给bsp加载好gs
        uint32_t  pcount   = ((init_to_kernel_header_v2*)(uint64_t)info_pbase)->logical_processor_count;
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
    // 4.5-3: 加载 BSP 的 GDT + TSS（上一步已完全构建，此步仅 LGDT+LTR）
    {
        gs_complex_t* bsp_cx = (gs_complex_t*)(uint64_t)(iv->arch_info.conjunc_GSs.vbase());
        bsp_kout << "[Phase4.5] LGDT+LTR: complex @ 0x" << HEX << (uint64_t)bsp_cx << kendl;
        gs_complex_load_gdt_tss(bsp_cx);
    }
    
    // 4.5-4: init_jump_to_kernel — 用 BSP 的 rsp0 栈构建 x64_standard_context 后跳入 kernel.elf
    {
        // entry_vaddr：phase_3a out-param 直出（init 内部消费，不进资产注册表/handoff 包）

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

    // BootInfoHeader 的 GOP 元信息 → 资产表（gop_info 结构体，arg1="gop" 走 arch 路由表
    // 固定 desc_size）。纯 boot 信息提取，不依赖 kmmu/iv，注册表就绪即可登记。
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].device_info != PASS_THROUGH_DEVICE_GRAPHICS_INFO) continue;
        auto* gfx = (GlobalBasicGraphicInfoType*)header->pass_through_devices[i].specify_data;
        if (!gfx) break;
        GlobalBasicGraphicInfoType* gop_copy = new GlobalBasicGraphicInfoType(*gfx);
        if (!asset_reg_add("gop_info gop", gop_copy)) {
            bsp_kout << "[INIT] asset dup: gop_info" << kendl;
            init_fatal::halt(SRC_LOC());
        }
        break;
    }

    uint64_t entry_vaddr = 0;
    if (phase_3a_load_kernel(kmmu, &em, header, &entry_vaddr) != 0) init_fatal::halt(SRC_LOC());
    ctx_intervals iv;
    if (phase_3b(kmmu, header, &em, &iv) != 0) init_fatal::halt(SRC_LOC());
    
    // Phase 4: 构造 v2 信息包
    // 包物理页由 page_allocator_v2 分配（free_ram_explore + pages_set 提交），
    // 语义态 transfer_package 入账穿越——kernel 收养 mem_map 后即知这些页被占用，
    // 且状态语义明确指向"交接信息包"（与普通持久元数据区分）。
    uint64_t segcnt = 0;
    phymem_segment* pure_view = basic_allocator::get_pure_memory_view(&segcnt);
    constexpr uint64_t PKT_PAGES = 8;   // v2 含 free_segs_descriptors_table，8 页预算
    phyaddr_t pkt = page_allocator_v2::free_ram_explore(PKT_PAGES, 12);
    if (!pkt) { bsp_kout << "pkt OOM" << kendl; init_fatal::halt(SRC_LOC()); }
    if (page_allocator_v2::pages_set({pkt, PKT_PAGES * 4096},
                                     page_state_t::transfer_package) != 0) {
        bsp_kout << "pkt pages_set failed" << kendl; init_fatal::halt(SRC_LOC());
    }
    ksetmem_8((void*)(uint64_t)pkt, 0, PKT_PAGES * 4096);

    if (!build_init_to_kernel_header(pkt, PKT_PAGES, header, pure_view, segcnt)) {
        bsp_kout << "build_init_to_kernel_header failed" << kendl; init_fatal::halt(SRC_LOC());
    }

    bsp_kout << "[Phase4] info_pkt: paddr=" << (void*)(uint64_t)pkt
             << " pages=" << (uint32_t)PKT_PAGES
             << " phymem_segments=" << (uint64_t)segcnt
             << " processors=" << (uint32_t)header->logical_processor_count << kendl;

    // Phase 4.5
    phase_45_finalize(kmmu, pkt, &iv, entry_vaddr, header);
    init_fatal::halt(SRC_LOC());
}
