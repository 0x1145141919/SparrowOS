#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/fixed_interrupt_vectors.h"
#include "util/kout.h"

// ═══════════════════════════════════════════════════════════════════════════
// 全局 IDT 资源
// ═══════════════════════════════════════════════════════════════════════════
//
// template_idt / global_idt 定义位于 x86_vecs_deliver_mgr.cpp（vectors 32–255
// 的递送逻辑依赖它们）。本文件仅提供：
//   1. template_idt_apply_region  — 模板 → 真实 IDT 条目转换
//   2. global_idtr                 — LIDT 使用的 IDT 寄存器值
//   3. exceptions_init             — 异常向量（vectors 0–31）的 IDT 初始化
//
// 时序关系：
//   _kernel_Init (BSP asm):
//     → exceptions_init()     写 template_idt[0..31] + template_idt_apply_region(0,31)
//     → lidt [global_idtr]    加载 IDT（此时 global_idt[0..31] 就绪）
//     → kernel_start()
//       → idt_vec_dispatch_mgr::Init()  写 template_idt[32..255] + template_idt_apply_region(32,255)
// ═══════════════════════════════════════════════════════════════════════════

// ── x86_vecs_deliver_mgr.cpp 提供的全局定义 ────────────────────────────
extern logical_idt template_idt[256];
extern IDTEntry    global_idt[256];

// ============================================================================
// global_idtr — IDT 寄存器值，供 LIDT 使用
// ============================================================================
extern "C" const IDTR global_idtr = {
    .limit = sizeof(global_idt) - 1,
    .base  = (uint64_t)global_idt
};

// ============================================================================
// template_idt_apply_region — 将 template_idt[] 的指定区间写入 global_idt[]
// ============================================================================
//
// 将逻辑 IDT 条目（handler 地址 + 属性）转换为架构 IDTEntry 格式（64-bit IDT
// gate descriptor）并写入 global_idt。
//
void template_idt_apply_region(uint8_t from_vec, uint8_t to_vec)
{
    for (uint16_t v = from_vec; v <= to_vec; v++) {
        logical_idt* src = &template_idt[v];
        if (!src->handler)
            continue;
        uint64_t addr = (uint64_t)src->handler;
        global_idt[v].offset_low    = addr & 0xFFFF;
        global_idt[v].offset_mid    = (addr >> 16) & 0xFFFF;
        global_idt[v].offset_high   = (addr >> 32) & 0xFFFFFFFF;
        global_idt[v].segment_selector = 0x08;
        global_idt[v].ist_index     = src->ist_index;
        global_idt[v].type          = src->type ? src->type : 0xE;  // 默认 32-bit 中断门
        global_idt[v].dpl           = src->dpl;
        global_idt[v].present       = 1;
        global_idt[v].reserved1     = 0;
        global_idt[v].reserved2     = 0;
        global_idt[v].reserved3     = 0;
    }
}

// ============================================================================
// exceptions_init — 填充异常向量（0–31）的 IDT 模板
// ============================================================================
//
// 为每个异常指定：
//   - handler → 对应的 bare_enter 汇编入口
//   - ist_index → 专用硬件栈（硬件栈已由 init.elf Phase 4.5 在 hdstacks 中分配）
//   - dpl → 用户态可触发的异常设为 3（如 #BP, #OF）
//
// 注意：所有 IDT 条目在 _kernel_Init 的 LIDT 之后立即可用。
// 向量 32–255 的填充在 idt_vec_dispatch_mgr::Init() 中完成。
//
static void exceptions_idt_init()
{
    // ── 异常 0–19 的标准分配 ─────────────────────────────────────────
    template_idt[ivec::DIVIDE_ERROR].handler   = (void*)&div_by_zero_bare_enter;

    template_idt[ivec::NMI].handler            = (void*)&nmi_bare_enter;
    template_idt[ivec::NMI].ist_index          = 3;   // IST3 — NMI 专用栈

    template_idt[ivec::BREAKPOINT].handler     = (void*)&breakpoint_bare_enter;
    template_idt[ivec::BREAKPOINT].ist_index   = 4;   // IST4 — Breakpoint/Debug
    template_idt[ivec::BREAKPOINT].dpl         = 3;   // 用户态 int3

    template_idt[ivec::OVERFLOW].handler       = (void*)&overflow_bare_enter;
    template_idt[ivec::OVERFLOW].dpl           = 3;   // 用户态 into

    template_idt[ivec::INVALID_OPCODE].handler = (void*)&invalid_opcode_bare_enter;

    template_idt[ivec::DOUBLE_FAULT].handler    = (void*)&double_fault_bare_enter;
    template_idt[ivec::DOUBLE_FAULT].ist_index  = 1;   // IST1 — Double Fault 栈

    template_idt[ivec::INVALID_TSS].handler    = (void*)&invalid_tss_bare_enter;

    template_idt[ivec::GENERAL_PROTECTION_FAULT].handler = (void*)&general_protection_bare_enter;

    template_idt[ivec::PAGE_FAULT].handler     = (void*)&page_fault_bare_enter;

    template_idt[ivec::MACHINE_CHECK].handler   = (void*)&machine_check_bare_enter;
    template_idt[ivec::MACHINE_CHECK].ist_index = 2;   // IST2 — Machine Check 栈

    template_idt[ivec::SIMD_FLOATING_POINT_EXCEPTION].handler = (void*)&simd_floating_point_bare_enter;

    template_idt[ivec::VIRTUALIZATION_EXCEPTION].handler = (void*)&virtualization_bare_enter;

    // ── 应用模板到全局 IDT ───────────────────────────────────────────
    template_idt_apply_region(0, 31);
}

extern "C" void exceptions_init()
{
    exceptions_idt_init();
}
