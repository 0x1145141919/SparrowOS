#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/fixed_interrupt_vectors.h"
#include "util/kout.h"

// ═══════════════════════════════════════════════════════════════════════════
// 全局 IDT 资源
// ═══════════════════════════════════════════════════════════════════════════
//
// template_idt / global_idt 定义位于 x86_vecs_deliver_mgr.cpp。
// 本文件提供：
//   1. template_idt_apply_region  — 模板 → 真实 IDT 条目转换
//   2. global_idtr                 — LIDT 使用的 IDT 寄存器值
//
// 调用时序：
//   _kernel_Init (BSP asm):
//     → vec_demux_early_init() [extern "C"]
//         → vec_demux::early_init()
//             → 填 template_idt[0..31] 异常入口
//             → 算术计算 template_idt[32..255] 地址
//             → template_idt_apply_region(0, 255)    写入 global_idt
//     → lidt [global_idtr]                            加载 IDT
//
//   kernel_start() (MM_READY 后):
//     → vec_demux::late_init()
//         → 清零 tokens 表
//         → 初始化 soft_interrupt_functions
//         → 初始化 ipi_descrioptors
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
        global_idt[v].type          = src->type ? src->type : 0xE;
        global_idt[v].dpl           = src->dpl;
        global_idt[v].present       = 1;
        global_idt[v].reserved1     = 0;
        global_idt[v].reserved2     = 0;
        global_idt[v].reserved3     = 0;
    }
}
