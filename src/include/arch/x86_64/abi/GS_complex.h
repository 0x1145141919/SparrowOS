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
// gs_complex_t 使用 __attribute__((packed)) + alignas() 精确控制布局，
// 不依赖手动 hex 偏移常数。所有成员位置由 offsetof 导出。
//
// per_processor_hardware_stack_t 保留原始 guard 页设计：
//   各栈前后有 4K guard 页（映射时不映射），用于捕获栈溢出。
//
// 物理区间（每个逻辑处理器）：
//   per_processor_hardware_stack_t hw_stacks[logical_processor_count]
//   uint8_t guard_tail[4096]
//
// 逻辑区间占用 vm_interval 的虚拟地址区间，只对栈真实占用空间
// 进行 WB_RW 映射，guard 页不映射以捕获页错误。

// ── FPU/SIMD 暂存区大小（保守上限，覆盖 AVX-512 + AMX） ────────────────
constexpr uint64_t XSAVE_SIZE_MAX = 8192;   // 8 KB

struct per_processor_hardware_stack_t;
struct pcid_entry_t{
    void* addrSpace;
    uint64_t last_accees_microsecond_timestamp;
};
struct pcid_complex_t{
    pcid_entry_t entries[6];//0号槽清0
    uint8_t now_using_pcid_idx;
};
// ── GS 复合体结构 ─────────────────────────────────────────────────────────
struct  alignas(4096) gs_complex_t {
    // ═══════════════════════════════════════════════════════════════════════
    // SLOT_IS_SLOT 区 — 固定 256 × uint64_t，索引见 GS_Slots_index_definitions.h
    // ═══════════════════════════════════════════════════════════════════════
    uint64_t slots[256];

    // ── 中断函数指针表 ─────────────────────────────────────────────────────
    interrupt_token_t tokens[256];
    spinlock_cpp_t    tokens_lock;  // 保护 tokens[] 读写

    // ── GDT + TSS ─────────────────────────────────────────────────────────
    x64_gdtentry        gdt[6];
    TSSDescriptorEntry  tss_descriptor;
    TSSentry            tss;

    // ── FPU/SIMD 暂存区（64B 对齐，满足 XSAVE64/XRSTOR64 要求） ──────────
    alignas(64) uint8_t fpu_area[XSAVE_SIZE_MAX];
    alignas(64) __uint128_t local_ipi_complex;
    uint64_t padding[6]; // 填充至 64B，方便umwait监视
    pcid_complex_t pcid_complex;
    // ── 硬件栈区指针 ───────────────────────────────────────────────────────
    per_processor_hardware_stack_t* stacks_ptr;
};

static_assert(sizeof(gs_complex_t)%4096 == 0, "gs_complex_t must be 4096-byte aligned");
// ── 每处理器硬件栈区 ─────────────────────────────────────────────────────
// 必须严格布局控制，guard 页内容不映射，仅占用物理/虚拟地址空间。
struct __attribute__((packed)) per_processor_hardware_stack_t {
    uint8_t guard1[4096];            // rsp0 前 guard

    // rsp0 — 核心态入口栈（CPL3→0 切换时自动加载到 RSP）
    uint8_t stack_rsp0[RSP0_STACKSIZE];

    uint8_t guard2[4096];            // stack_rsp0 → IST1 间 guard

    // IST1 — Double Fault 栈
    uint8_t stack_ist1[DF_STACKSIZE];

    uint8_t guard3[4096];

    // IST2 — Machine Check 栈
    uint8_t stack_ist2[MC_STACKSIZE];

    uint8_t guard4[4096];

    // IST3 — NMI 栈
    uint8_t stack_ist3[NMI_STACKSIZE];

    uint8_t guard5[4096];

    // IST4 — Breakpoint / Debug 栈
    uint8_t stack_ist4[BP_DBG_STACKSIZE];
};

// ── 栈偏移常数（基于 offsetof，编译器保证精确） ──────────────────────────
constexpr uint64_t RSP0_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_rsp0);
constexpr uint64_t IST1_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist1);
constexpr uint64_t IST2_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist2);
constexpr uint64_t IST3_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist3);
constexpr uint64_t IST4_BASE_OFF = offsetof(per_processor_hardware_stack_t, stack_ist4);

// 栈底 = 基址 + 大小 - 安全裕量 (0x40, red zone margin)
constexpr uint64_t RED_ZONE         = 0x40;
constexpr uint64_t RSP0_BOTTOM_OFF  = RSP0_BASE_OFF + RSP0_STACKSIZE - RED_ZONE;
constexpr uint64_t IST1_BOTTOM_OFF  = IST1_BASE_OFF + DF_STACKSIZE   - RED_ZONE;
constexpr uint64_t IST2_BOTTOM_OFF  = IST2_BASE_OFF + MC_STACKSIZE   - RED_ZONE;
constexpr uint64_t IST3_BOTTOM_OFF  = IST3_BASE_OFF + NMI_STACKSIZE  - RED_ZONE;
constexpr uint64_t IST4_BOTTOM_OFF  = IST4_BASE_OFF + BP_DBG_STACKSIZE - RED_ZONE;

// ── 编译期校验 ───────────────────────────────────────────────────────────
static_assert(sizeof(gs_complex_t::slots) == 256 * sizeof(uint64_t),
              "slot zone must be exactly 256 slots");

static_assert(offsetof(gs_complex_t, fpu_area) % 64 == 0,
              "fpu_area must be 64-byte aligned for XSAVE");

static_assert(sizeof(gs_complex_t) % 4096 == 0,
              "gs_complex_t size must be 4096-byte aligned (alignas(4096))");

static_assert(offsetof(per_processor_hardware_stack_t, stack_rsp0) % 4096 == 0,
              "stack_rsp0 must be page-aligned");

// ── per-processor stride ─────────────────────────────────────────────────
// gs_complex_t 已 alignas(4096)，sizeof 自然为 4096 倍数
constexpr uint64_t GS_COMPLEX_STRIDE = (sizeof(gs_complex_t) + 4095) & ~4095ULL;

// ── 读取 GS 段基址 ───────────────────────────────────────────────────
// RDGSBASE 无特权级限制，直接读 IA32_GS_BASE MSR 到通用寄存器。
// 返回值即指向当前核 gs_complex_t 实例的指针。
static inline gs_complex_t* get_gs_base()
{
    void* ptr;
    asm volatile("rdgsbase %0" : "=r"(ptr) ::);
    return (gs_complex_t*)ptr;
}

// ── GDT/TSS 加载接口 ──────────────────────────────────────────────────
// 从 gs_complex_t 的内嵌 GDT + TSS 描述符中加载 GDT 和 TSS（LGDT + LTR）。
// 调用前需确保 GDT 条目、TSS 描述符和栈指针已就绪。
// BSP 和 AP 在正式资源加载阶段均调用此接口。
extern "C" void gs_complex_load_gdt_tss(gs_complex_t* complex);
