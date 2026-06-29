#pragma once
#include "stdint.h"

/*
 * 向量号布局：
 *   0–31    — x86_exceptions            CPU 异常（架构固定）
 *   32–224  — [free pool]               alloc_vec 自由分配
 *   225–227 — x86_softinterrupt_abi     软中断（int N，不返回，UD2 sentinel）
 *   228–249 — [free pool]               空档
 *   250–251 — return_ipi_vec            系统 IPI（tokens，返回型）
 *   252–254 — runaway_ipi_vec           系统 IPI（soft 风格，不返回型）
 *   255     — SUPRIOUS_INTERRUPT        软中断（虚假中断检测）
 *
 * 约束：软中断、系统 IPI、硬件中断三组向量两两不可相交。
 */
namespace x86_exceptions{
    static constexpr uint8_t DIVIDE_ERROR = 0;
    static constexpr uint8_t DEBUG = 1;
    static constexpr uint8_t NMI = 2;
    static constexpr uint8_t BREAKPOINT = 3;
    static constexpr uint8_t OVERFLOW = 4;
    static constexpr uint8_t BOUND_RANGE_EXCEEDED = 5;
    static constexpr uint8_t INVALID_OPCODE = 6;
    static constexpr uint8_t DEVICE_NOT_AVAILABLE = 7;
    static constexpr uint8_t DOUBLE_FAULT = 8;
    static constexpr uint8_t COPROCESSOR_SEGMENT_OVERRUN = 9;
    static constexpr uint8_t INVALID_TSS = 10;
    static constexpr uint8_t SEGMENT_NOT_PRESENT = 11;
    static constexpr uint8_t STACK_SEGMENT_FAULT = 12;
    static constexpr uint8_t GENERAL_PROTECTION_FAULT = 13;
    static constexpr uint8_t PAGE_FAULT = 14;
    static constexpr uint8_t X87_FPU_ERROR = 16;
    static constexpr uint8_t ALIGNMENT_CHECK = 17;
    static constexpr uint8_t MACHINE_CHECK = 18;
    static constexpr uint8_t SIMD_FLOATING_POINT_EXCEPTION = 19;
    static constexpr uint8_t VIRTUALIZATION_EXCEPTION = 20;
    static constexpr uint8_t CONTROL_PROTECTION_EXCEPTION = 21;
};
// fred/idt 时代都兼容的 ABI
namespace x86_softinterrupt_abi{
    static constexpr uint8_t ASM_PANIC = 225;        // int 225 — 内核恐慌
    static constexpr uint8_t KTHREAD_CALL = 226;     // int 226 — 内核线程交出执行流
    static constexpr uint8_t USER_ABI_ENTER = 227;   // int 227 — 用户态入口
            // int 255 — 虚假中断
    // IDT 下 255 完全占用；FRED 下硬件中断不混淆 type=2，255 仍可用于虚假中断检测
};
// IPI（soft_interrupt_functions 风格，不返回）
namespace ipi_vecs{
    static constexpr uint8_t IPI_RUNAWAY = 254;
    static constexpr uint8_t IPI_RETURNABLE  = 253;
    static constexpr uint8_t IPI_HALT    = 252;
    static constexpr uint8_t IPI_RESCHED    = 251;
};
static constexpr uint8_t SUPRIOUS_INTERRUPT = 255;  
