#pragma once
#include <stdint.h>

// ════════════════════════════════════════════════════════════════
// asset_names — 资产全名常量表（ABI 稳定层，init/kernel 双世界共用）
//
// 多arg name 语法："<arg0> <arg1>..."（arg0=本名/树键，arg1=路由类型）。
// 本表是资产全名的**单一来源**：
//   - 生产方（phase_3a / phase_3b / init_init）引用全名常量登记资产
//   - 消费方（mem_init 全局遍历 / exec_env_prepare 认领）引用同一常量检索
// 避免字符串字面量散落多处、改名漏同步。命名空间即本名，常量值即全名。
//
// inline constexpr：编译期常量，零运行时构造（不违反"禁止全局 C++ 构造"纪律），
// 跨 TU ODR 安全，落在 .rodata。
// ════════════════════════════════════════════════════════════════

namespace asset_names {

    // ── phase_3a：kernel.elf 四段 + kimg ──
    inline constexpr const char* kernel_code   = "kernel_code mem";
    inline constexpr const char* kernel_data   = "kernel_data mem";
    inline constexpr const char* kernel_rodata = "kernel_rodata mem";
    inline constexpr const char* kernel_bss    = "kernel_bss mem";
    inline constexpr const char* kimg          = "kimg movable";

    // ── phase_3b：十资产 ──
    inline constexpr const char* fpa_bitmaps     = "fpa_bitmaps movable";
    inline constexpr const char* pages_arr       = "pages_arr movable";
    inline constexpr const char* log_buffer      = "log_buffer movable";
    inline constexpr const char* ksymbols        = "ksymbols movable";
    inline constexpr const char* initramfs       = "initramfs movable";
    inline constexpr const char* gop_framebuffer = "gop_framebuffer mem";
    inline constexpr const char* hpet_mmio       = "hpet_mmio mem";
    inline constexpr const char* gs_complexes    = "gs_complexes mem";
    inline constexpr const char* hdstacks        = "hdstacks mem";
    inline constexpr const char* phyaddr_window  = "phyaddr_window mem";
    
    // ── init_init ──
    inline constexpr const char* gop_info        = "gop_info gop";
    inline constexpr const char* xsdt_pbase        = "xsdt_pbase scalar";

    // ── 全名总表（mem_init 全局遍历用；注册序 = 数组序）──
    inline constexpr const char* all[] = {
        kernel_code, kernel_data, kernel_rodata, kernel_bss, kimg,
        fpa_bitmaps, pages_arr, log_buffer, ksymbols, initramfs,
        gop_framebuffer, hpet_mmio, gs_complexes, hdstacks, phyaddr_window,
        gop_info,
        xsdt_pbase,
    };
    inline constexpr uint32_t all_count() {
        return sizeof(all) / sizeof(all[0]);
    }
}
