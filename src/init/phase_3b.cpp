#include "init/phase_3.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "firmware/gSTResloveAPIs.h"
#include "init/init_bcb_juvenile.h"
#include "init/init_fatal.h"
#include "init/initramfs_lookup.h"
#include "init/util/kout.h"

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

// ============================================================================
// Phase 3b 资产生产：集中登记表（.rodata 全局 const）
// ============================================================================
// 每个资产一行：多arg 名 + 生产函数（分配/映射/写 iv/登记一次完成）。
//   - 顺序 = 数组顺序（与 kmmu 台账解耦，仅决定登记次序）
//   - 可选资产（ksymbols/initramfs/GOP/HPET）找不到时生产函数打警告返回 0 跳过
//   - 硬错误（OOM / 预置缺失）返回 SRC_LOC()，由 init_main halt
// 生产函数共享一个 p3b_ctx（kmmu / header / em / iv），避免散落参数。
struct p3b_ctx {
    kernel_mmu*          kmmu;
    BootInfoHeader*      header;
    const ctx_early_mem* em;
    ctx_intervals*       iv;
};

// ---- FPA_bitmaps（mem）：init_bcb_juvenile 位图元数据池的交接，隐式状态穿越 ----
//      FPA_bitmaps 即 BCB 位图池本身（get_region_pbase/get_region_size）。池由
//      plan_and_setup 自保护（step 8c 池页在所属 BCB 叶子置占用），不额外分配/记账；
//      此处只映射 + 塞入资产，kernel 收养路径后续据此重建 BCBS。
static loc_code_t build_fpa_bitmaps(p3b_ctx& ctx, const char* name) {
    phyaddr_t p  = init_bcb_juvenile::get_region_pbase();
    uint64_t sz  = init_bcb_juvenile::get_region_size();
    if (!p || !sz) { bsp_kout << "[Phase3b] FPA pool not ready" << kendl; return SRC_LOC(); }
    uint64_t npg = sz >> 12;
    vaddr_t v = va_alloc_up(sz, 12);
    ctx.kmmu->map(kernel_mmu::make_entry(p, v, sz, KSPACE_RW_ACCESS,
                                         "fpa_bitmaps", KMMU_ENTRY_FLAG_PERSISTENT));
    ctx.iv->FPA_bitmaps = {.vpn = v >> 12, .ppn = p >> 12,
                           .npages = npg, .access = KSPACE_RW_ACCESS};
    asset_reg_add(name, new vm_interval{ .vpn = v >> 12, .ppn = p >> 12,
                                         .npages = npg, .access = KSPACE_RW_ACCESS });
    bsp_kout << "[Phase3b] FPA_bitmaps(pool): p=0x" << p << " v=" << (void*)v
             << " sz=" << (void*)sz << kendl;
    return 0;
}

// ---- log_buffer（mem）：日志输出连续性，保留提前映射 ----
static loc_code_t build_log_buffer(p3b_ctx& ctx, const char* name) {
    uint64_t sz  = LOGBUFFER_SIZE;
    uint64_t npg = sz >> 12;
    loc_code_t alloc_err = 0;
    phyaddr_t p  = init_bcb_juvenile::alloc(npg, 21, &alloc_err);
    if (!p) { bsp_kout << "log OOM" << kendl; return SRC_LOC(); }
    ksetmem_8((void*)(uint64_t)p, 0, sz);
    vaddr_t v = va_alloc_up(sz, 21);
    ctx.kmmu->map(kernel_mmu::make_entry(p, v, sz, KSPACE_RW_ACCESS,
                                         "log_buffer", KMMU_ENTRY_FLAG_PERSISTENT));
    ctx.iv->log_buffer = {.vpn = v >> 12, .ppn = p >> 12,
                          .npages = npg, .access = KSPACE_RW_ACCESS};
    asset_reg_add(name, new vm_interval{ .vpn = v >> 12, .ppn = p >> 12,
                                         .npages = npg, .access = KSPACE_RW_ACCESS });
    bsp_kout << "[Phase3b] log_buffer: p=0x" << p << " v=" << (void*)v << kendl;
    return 0;
}

// ---- symtable_file（movable）：probe + initramfs_lookup，纯物理描述符不做 KMMU 映射 ----
static loc_code_t build_ksymbols(p3b_ctx& ctx, const char* name) {
    phyaddr_t sym_in_ramfs = 0; uint64_t sym_sz = 0;
    if (ctx.em->ramfs_base) {
        auto* rh = (const initramfs_header*)(uint64_t)ctx.em->ramfs_base;
        sym_in_ramfs = initramfs_lookup(rh, "/ksymbols.bin", &sym_sz);
    }
    if (sym_in_ramfs == 0 || sym_sz == 0) {
        bsp_kout << "[Phase3b] ksymbols.bin not found" << kendl;
        return 0;   // 可选资产，跳过
    }
    uint64_t sz  = align_up(sym_sz, 4096);
    uint64_t npg = sz >> 12;
    loc_code_t alloc_err = 0;
    phyaddr_t p  = init_bcb_juvenile::alloc(npg, 21, &alloc_err);
    if (!p) { bsp_kout << "sym OOM" << kendl; return SRC_LOC(); }
    ksystemramcpy((void*)(uint64_t)sym_in_ramfs, (void*)(uint64_t)p, sym_sz);
    ctx.iv->symtable_file = { .base_ppn = p >> 12, .size = sym_sz };
    asset_reg_add(name, new movable_file_entry_t{ .base_ppn = p >> 12, .size = sym_sz });
    bsp_kout << "[Phase3b] symtable: p=0x" << p << " size=" << sym_sz << kendl;
    return 0;
}

// ---- initramfs_file（movable）：原位引用 UEFI 加载位置，不做 KMMU 映射 ----
static loc_code_t build_initramfs(p3b_ctx& ctx, const char* name) {
    if (ctx.em->ramfs_base && ctx.em->ramfs_size) {
        ctx.iv->initramfs_file = { .base_ppn = ctx.em->ramfs_base >> 12,
                                   .size     = ctx.em->ramfs_size };
        asset_reg_add(name, new movable_file_entry_t{ .base_ppn = ctx.em->ramfs_base >> 12,
                                                      .size     = ctx.em->ramfs_size });
        bsp_kout << "[Phase3b] initramfs: p=" << (void*)ctx.em->ramfs_base
                 << " size=" << ctx.em->ramfs_size << kendl;
    }
    return 0;
}

// ---- GOP 帧缓冲（mem 资产）：pass_through 扫描 + WC 映射 ----
// gop_info 结构体资产（GlobalBasicGraphicInfoType）在 init_main 登记（见 init_init.cpp）。
static loc_code_t build_gop(p3b_ctx& ctx, const char* name) {
    for (uint64_t i = 0; i < ctx.header->pass_through_device_info_count; i++) {
        if (ctx.header->pass_through_devices[i].device_info != PASS_THROUGH_DEVICE_GRAPHICS_INFO) continue;
        auto* gfx = (GlobalBasicGraphicInfoType*)ctx.header->pass_through_devices[i].specify_data;
        if (!gfx) break;
        // 帧缓冲区间资产（mem）：WC 映射，kernel 重映射用
        uint64_t fb_sz  = align_up(gfx->FrameBufferSize, 4096);
        uint64_t fb_npg = fb_sz >> 12;
        phyaddr_t fb_p  = (phyaddr_t)gfx->FrameBufferBase;
        vaddr_t fb_v    = va_alloc_up(fb_sz, 21);
        ctx.kmmu->map(kernel_mmu::make_entry(fb_p, fb_v, fb_sz, KSPACE_RW_WC_ACCESS,
                                             "gop_framebuffer", KMMU_ENTRY_FLAG_PERSISTENT));
        ctx.iv->arch_info.Gop_vbase = fb_v;
        asset_reg_add(name, new vm_interval{ .vpn = fb_v >> 12, .ppn = fb_p >> 12,
                                             .npages = fb_npg, .access = KSPACE_RW_WC_ACCESS });
        bsp_kout << HEX << "[Phase3b] GOP fb: p=0x" << fb_p << " v=0x" << fb_v << kendl;

        // 过渡：iv.arch_info.gop_info 仍保留（v1 kernel 消费路径未迁移）
        ksystemramcpy(gfx, &ctx.iv->arch_info.gop_info, sizeof(*gfx));
        break;
    }
    return 0;
}

// ---- HPET MMIO（mem）：XSDT 扫描 + UC 映射 ----
static loc_code_t build_hpet(p3b_ctx& ctx, const char* name) {
    if (ctx.em->xsdt_base) {
        auto* xsdt = (const XSDT_Table*)(uint64_t)ctx.em->xsdt_base;
        uint64_t ec = (xsdt->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
        for (uint64_t i = 0; i < ec; i++) {
            auto* hdr = (const ACPI_Table_Header*)(uint64_t)xsdt->Entry[i];
            if (!hdr) continue;
            if (*(const uint32_t*)hdr->Signature == HPET_SIGNATURE_UINT32) {
                auto* ht = (const HPET_Table*)(uint64_t)xsdt->Entry[i];
                phyaddr_t hp = ht->Base_Address;
                if (hp) {
                    vaddr_t hv = va_alloc_up(0x1000, 12);
                    ctx.kmmu->map(kernel_mmu::make_entry(hp, hv, 0x1000, KSPACE_RW_UC_ACCESS,
                                                         "hpet_mmio", KMMU_ENTRY_FLAG_PERSISTENT));
                    ctx.iv->arch_info.hpet_mmio = {.vpn = hv >> 12, .ppn = hp >> 12,
                                                   .npages = 1, .access = KSPACE_RW_UC_ACCESS};
                    asset_reg_add(name, new vm_interval{ .vpn = hv >> 12, .ppn = hp >> 12,
                                                         .npages = 1, .access = KSPACE_RW_UC_ACCESS });
                    bsp_kout  << "[Phase3b] HPET MMIO: p=" << (void*)hp << " v=" << (void*)hv << kendl;
                }
                break;
            }
        }
    }
    return 0;
}

// ---- conjunc_GSs（mem）：每个处理器一块 gs_complex_t ----
static loc_code_t build_gs_complexes(p3b_ctx& ctx, const char* name) {
    uint64_t total_bytes    = ctx.header->logical_processor_count * GS_COMPLEX_STRIDE;
    uint64_t npg            = total_bytes >> 12;
    loc_code_t alloc_err = 0;
    phyaddr_t pbase         = init_bcb_juvenile::alloc(npg, 12, &alloc_err);
    vaddr_t  vbase          = va_alloc_up(total_bytes, 12);
    ksetmem_8((void*)(uint64_t)pbase, 0, total_bytes);
    ctx.iv->arch_info.conjunc_GSs = {
        .vpn    = vbase >> 12,
        .ppn    = pbase >> 12,
        .npages = npg,
        .access = KSPACE_RW_ACCESS
    };
    ctx.kmmu->map(kernel_mmu::make_entry(pbase, vbase, total_bytes, KSPACE_RW_ACCESS,
                                         "gs_complexes", KMMU_ENTRY_FLAG_PERSISTENT));
    asset_reg_add(name, new vm_interval{ .vpn = vbase >> 12, .ppn = pbase >> 12,
                                         .npages = npg, .access = KSPACE_RW_ACCESS });
    bsp_kout << "[Phase3b] conjunc_GSs: vaddr=" << (void*)(uint64_t)vbase
             << " paddr=" << (void*)(uint64_t)pbase
             << " size=0x" << (uint64_t)(npg << 12) << kendl;
    return 0;
}

// ---- hardware stacks（mem）：预留 VA + 物理区间穿越 + KMMU 粗映射 ----
//     整体按 RW 粗映射入 kmmu（与 gs_complexes 等 mem 资产一致）；guard 页的
//     不映射铁律由 kernel 侧精细映射阶段落实（先 kspace_vm_table 圈地再逐栈跳过 guard）。
static loc_code_t build_hdstacks(p3b_ctx& ctx, const char* name) {
    uint64_t stack_stride   = sizeof(per_processor_hardware_stack_t);  // 含 5 guard 页
    uint64_t total_phys     = ctx.header->logical_processor_count * stack_stride + 4096;  // + 尾 guard
    uint64_t hd_pages       = total_phys >> 12;
    loc_code_t alloc_err = 0;
    phyaddr_t hd_pbase      = init_bcb_juvenile::alloc(hd_pages, 12, &alloc_err);
    if (!hd_pbase) { bsp_kout << "hdstacks OOM" << kendl; return SRC_LOC(); }
    vaddr_t  hd_vbase       = va_alloc_up(total_phys, 12);
    ctx.kmmu->map(kernel_mmu::make_entry(hd_pbase, hd_vbase, total_phys, KSPACE_RW_ACCESS,
                                         "hdstacks", KMMU_ENTRY_FLAG_PERSISTENT));
    // 过渡：arch_info 字段同步（vbase 不再为 0，v1 kernel 的 hw_stacks 获得真实 VA）
    ctx.iv->arch_info.hdstacks_interval_pbase  = hd_pbase;
    ctx.iv->arch_info.hdstacks_4kbpgs_count    = hd_pages;
    ctx.iv->arch_info.hdstacks_interval_vbase  = hd_vbase;
    asset_reg_add(name, new vm_interval{ .vpn = hd_vbase >> 12, .ppn = hd_pbase >> 12,
                                         .npages = hd_pages, .access = KSPACE_RW_ACCESS });
    bsp_kout << "[Phase3b] hdstacks: paddr=0x" << HEX << hd_pbase
             << " vaddr=" << (void*)(uint64_t)hd_vbase
             << " pages=" << hd_pages << DEC << kendl;
    return 0;
}

// ---- high_window / Kspace_phyaddr_access_window（mem）：[0, dram_top) → 1GB 对齐高 VA ----
//     不是恒等映射！将整个物理地址区间映射到内核高位空间（1GB 对齐），
//     供 kernel.elf 的 PhyAddrAccessor / 页表重建时访问任意物理地址。
static loc_code_t build_phyaddr_window(p3b_ctx& ctx, const char* name) {
    phyaddr_t top = ctx.em->dram_top;
    uint64_t  sz  = align_up(top, 0x40000000ULL);
    vaddr_t   v   = va_alloc_up(sz, 30);
    ctx.kmmu->map(kernel_mmu::make_entry(0, v, sz, KSPACE_RW_ACCESS,
                                         "phyaddr_window", KMMU_ENTRY_FLAG_PERSISTENT));
    bsp_kout << "[Phase3b] high_window: [0," << (void*)top << ") at" << (void*)v << kendl;
    ctx.iv->Kspace_phyaddr_access_window = {
        .vpn    = v >> 12,
        .ppn    = 0,
        .npages = sz >> 12,
        .access = KSPACE_RW_ACCESS
    };
    asset_reg_add(name, new vm_interval{ .vpn = v >> 12, .ppn = 0,
                                         .npages = sz >> 12, .access = KSPACE_RW_ACCESS });
    return 0;
}

// ---- 集中登记表（.rodata 全局 const）：注册顺序 = 数组顺序 ----
static const struct {
    const char* name;
    loc_code_t (*build)(p3b_ctx&, const char*);
} k_p3b_assets[] = {
    { "fpa_bitmaps mem",     build_fpa_bitmaps },
    { "log_buffer mem",      build_log_buffer },
    { "ksymbols movable",    build_ksymbols },
    { "initramfs movable",   build_initramfs },
    { "gop_framebuffer mem", build_gop },
    { "hpet_mmio mem",       build_hpet },
    { "gs_complexes mem",    build_gs_complexes },
    { "hdstacks mem",        build_hdstacks },
    { "phyaddr_window mem",  build_phyaddr_window },
};
static constexpr uint64_t k_p3b_asset_count =
    sizeof(k_p3b_assets) / sizeof(k_p3b_assets[0]);

// ============================================================================
// Phase 3b (串行): 恒等映射 + 区间分配 + 架构信息收集 → 产出进资产容器 + ctx_intervals
// ============================================================================
// 返回 loc_code_t（0 成功）；内部失败 return SRC_LOC()，由 init_main init_fatal。
// ctx_intervals 经 out-param 输出（过渡；资产本体已在容器内，iv 供 info_fill/4.5 过渡用）。
loc_code_t phase_3b(kernel_mmu* kmmu, BootInfoHeader* header,
                    const ctx_early_mem* em, ctx_intervals* iv_out) {
    ctx_intervals iv = {};

    // --- 清空 extra VM 数组 ---
    iv.extra_vm_arr   = new loaded_VM_interval[8];
    iv.extra_vm_count = 0;

    bsp_kout << "[Phase3b] start..." << kendl;

    // ---- 恒等映射: [4KB, dram_top) WB+RWX（短暂存在，不进 info header / 资产容器） ----
    //     仅用于 init.elf 自身 CR3 切换的极小窗口 + 跳转 kernel.elf 后访问信息包。
    //     kernel.elf 接手后通过 Kspace_phyaddr_access_window 访问物理地址。
    {
        phyaddr_t top = em->dram_top;
        uint64_t  sz  = top - 0x1000;
        pgaccess id_a = KSPACE_RWX_NG_ACCESS;
        kmmu->map(kernel_mmu::make_entry(0x1000, 0x1000, sz, id_a,
                                         "identity_map", KMMU_ENTRY_FLAG_TRANSIENT));
        bsp_kout << "[Phase3b] identity: [0x1000, 0x" << top << ") WB+RWX (transient)" << kendl;
    }

    // XSDT 基址（架构标量，非资产；HPET 生产函数消费 em->xsdt_base）
    iv.arch_info.XSDT_base = em->xsdt_base;

    // ---- 集中注册：.rodata 表驱动，逐资产生产 + 登记 ----
    p3b_ctx ctx = { kmmu, header, em, &iv };
    for (uint64_t i = 0; i < k_p3b_asset_count; i++) {
        loc_code_t rc = k_p3b_assets[i].build(ctx, k_p3b_assets[i].name);
        if (rc != 0) return rc;
    }

    bsp_kout << "[Phase3b] done: " << iv.extra_vm_count << " extra VM entries" << kendl;
    *iv_out = iv;
    return 0;
}
