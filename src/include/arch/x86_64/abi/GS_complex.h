#pragma once
#include <stdint.h>
#include "arch/x86_64/Interrupt_system/loacl_processor.h"

// ============================================================================
// GS 复合体 — per-processor 元数据 + 硬件栈 统一区域
// ============================================================================
//
// 每个逻辑处理器的 GS 段指向一个 gs_complex_t 实例。
// 所有 per-CPU 元数据、GDT/TSS、FPU 暂存区、硬件栈**一次分配**，物理连续。
//
// 布局概述：
//   [0x0000, 0x0800)  slots[256]         — SLOT_IS_SLOT 区，uint64_t 槽
//   [0x0800, 0x1000)  dispatch[256]      — 中断函数指针表，零间接分发
//   [0x1000, 0x10B0)  gdt + tss_desc + tss — 架构描述符
//   [0x10C0, 0x30C0)  fpu_area[8KB]     — XSAVE/XRSTOR 暂存区
//   [0x4000, 0x17000) 硬件栈区（5 个固定栈）
//
//  总大小 ≈ 92KB → stride = 96KB（24 页）

// ── FPU/SIMD 暂存区大小（保守上限，覆盖 AVX-512 + AMX） ────────────────
// 实际 enabl 阶段可通过 CPUID 缩小，但分配阶段用此值保证安全。
constexpr uint64_t XSAVE_SIZE_MAX = 8192;   // 8 KB
struct per_processor_hardware_stack_t;
// ── GS 复合体结构 ─────────────────────────────────────────────────────────
struct gs_complex_t {
    // ══════════════════════════════════════════════════════════════════════
    // SLOT_IS_SLOT 区  [0x0000, 0x0800)
    // 每个槽位 uint64_t，索引定义见 GS_Slots_index_definitions.h
    // ══════════════════════════════════════════════════════════════════════
    uint64_t slots[256];                     // 0x0000 — 0x0800  (2048 B)

    // ══════════════════════════════════════════════════════════════════════
    // 动态区  [0x0800, max)
    // 所有字段 slot-aligned（8B 对齐），通过 gs_complex_t 偏移直接访问
    // ══════════════════════════════════════════════════════════════════════

    // ── 中断函数指针表 ─────────────────────────────────────────────────────
    // GS_BASE + offsetof(dispatch) + vec * 8 直接命中，无需解指针
    hard_interrupt_func_t dispatch[256];     // 0x0800 — 0x1000 (2048 B)

    // ── GDT + TSS ─────────────────────────────────────────────────────────
    x64_gdtentry        gdt[6];              // 0x1000 — 0x1030
    TSSDescriptorEntry  tss_descriptor;      // 0x1030 — 0x1040
    TSSentry            tss;                 // 0x1040 — 0x10A8 (104 B)

    // ── fpu_area 对齐垫片 ─────────────────────────────────────────────────
    uint8_t             _pad0[0x10C0 - 0x10A8];   // 0x10A8 — 0x10C0 (24 B)

    // ── FPU/SIMD 暂存区 ───────────────────────────────────────────────────
    // 64B 对齐，XSAVE64 / XRSTOR64 格式
    uint8_t             fpu_area[XSAVE_SIZE_MAX]; // 0x10C0 — 0x30C0 (8 KB)

    // ── 栈区对齐垫片（保证硬件栈起始于页边界） ─────────────────────────────
    uint8_t             _pad1[0x4000 - 0x30C0];   // 0x30C0 — 0x4000
    // (FRED per-level 栈指针预留位置可在 slot 区扩展)
    per_processor_hardware_stack_t* stacks_ptr;
};
struct per_processor_hardware_stack_t{//必须4096对齐
    uint8_t guard1[4096];//内容为0xff但是不被映射
    // ══════════════════════════════════════════════════════════════════════
    // 每处理器固定硬件栈
    // TSS.rsp0 / IST 指针在初始化阶段设为各栈的**顶部**
    // ══════════════════════════════════════════════════════════════════════

    // rsp0 — 核心态入口栈（CPL3→0 切换时自动加载到 RSP）
    uint8_t             stack_rsp0[RSP0_STACKSIZE];  // 32 KB

    uint8_t guard2[4096];//内容为0xff但是不被映射
    // IST1 — Double Fault 栈
    uint8_t             stack_ist1[DF_STACKSIZE];    // 8 KB

    uint8_t guard3[4096];//内容为0xff但是不被映射
    // IST2 — Machine Check 栈
    uint8_t             stack_ist2[MC_STACKSIZE];    // 12 KB

    uint8_t guard4[4096];//内容为0xff但是不被映射
    // IST3 — NMI 栈
    uint8_t             stack_ist3[NMI_STACKSIZE];   // 12 KB

    uint8_t guard5[4096];//内容为0xff但是不被映射
    // IST4 — Breakpoint / Debug 栈
    uint8_t             stack_ist4[BP_DBG_STACKSIZE];// 12 KB

};
constexpr uint64_t RSP0_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_rsp0);
constexpr uint64_t IST1_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist1);
constexpr uint64_t IST2_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist2);
constexpr uint64_t IST3_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist3);
constexpr uint64_t IST4_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist4);
constexpr uint64_t RSP0_BOTTOM_OFF = RSP0_BASE_OFF+RSP0_STACKSIZE-0x40;
constexpr uint64_t IST1_BOTTOM_OFF = IST1_BASE_OFF+DF_STACKSIZE-0x40;
constexpr uint64_t IST2_BOTTOM_OFF = IST2_BASE_OFF+MC_STACKSIZE-0x40;
constexpr uint64_t IST3_BOTTOM_OFF = IST3_BASE_OFF+NMI_STACKSIZE-0x40;
constexpr uint64_t IST4_BOTTOM_OFF = IST4_BASE_OFF+BP_DBG_STACKSIZE-0x40;
/**
 * 这个per_processor_hardware_stack_t在'/home/PS/PS_git/OS_pj_uefi/kernel/src/include/arch/x86_64/boot.h'中增加新的vm_interval字段我的规划是
 * 物理区间上
 * per_processor_hardware_stack_t stack_copmlexs[logical_processor_count]
 * uint8_t gurad_tail[4096]
 * 逻辑区间上,占用vm_interval的虚拟地址区间，却只对栈真实占用的空间进行WB_RW的映射，guard页不映射捕获页错误
 */
// ── 编译期校验 ───────────────────────────────────────────────────────────

static_assert(sizeof(gs_complex_t::slots) == 256 * sizeof(uint64_t),
              "slot zone must be exactly 256 slots");

static_assert(offsetof(gs_complex_t, fpu_area) % 64 == 0,
              "fpu_area must be 64-byte aligned for XSAVE");

// 每个栈的首字节在各自区间开头（TSS.rsp0 应设为 stack_rsp0 + 总大小 = 顶部）
static_assert(offsetof(per_processor_hardware_stack_t, stack_rsp0) % 4096 == 0,
              "stack_rsp0 must be page-aligned");

// ── per-processor stride ─────────────────────────────────────────────────
constexpr uint64_t GS_COMPLEX_STRIDE = (sizeof(gs_complex_t) + 4095) & ~4095ULL;
// = 0x18000 = 98304 = 96 KB (24 pages)

// ── GDT/TSS 加载接口 ──────────────────────────────────────────────────
// 从 gs_complex_t 的内嵌 GDT + TSS 描述符中加载 GDT 和 TSS（LGDT + LTR）。
// 调用前需确保 GDT 条目、TSS 描述符和栈指针已就绪。
// BSP 和 AP 在正式资源加载阶段均调用此接口。
extern "C" void gs_complex_load_gdt_tss(gs_complex_t* complex);
