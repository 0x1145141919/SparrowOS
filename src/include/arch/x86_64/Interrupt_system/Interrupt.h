#pragma once
#include <stdint.h>
#include "fixed_interrupt_vectors.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "abi/os_error_definitions.h"
#include "abi/arch_code.h"
extern void (*global_ipi_handler)();
/**
 * 中断管理器，管理着每个cpu的中断描述符表和本地apic
 * 当然，调用时必须上报其apic__id
 */
 // namespace gdtentry
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;    // 栈指针（仅特权级变化时压入）
    uint64_t ss;     // 栈段选择子（仅特权级变化时压入）
};

extern "C" void div_by_zero_cpp_enter(x64_standard_context*frame);
extern "C" void debug_cpp_enter(x64_standard_context* frame);
extern "C" void nmi_cpp_enter(x64_standard_context* frame);
extern "C" void breakpoint_cpp_enter(x64_standard_context* frame);
extern "C" void overflow_cpp_enter(x64_standard_context* frame);
extern "C" void invalid_opcode_cpp_enter(x64_standard_context* frame);        // #UD
extern "C" void double_fault_cpp_enter(x64_errcode_exception_frame* frame); // #DF
extern "C" void invalid_tss_cpp_enter(x64_errcode_exception_frame* frame);   // #TS    
extern "C" void general_protection_cpp_enter(x64_errcode_exception_frame* frame); // #GP
extern "C" void page_fault_cpp_enter(x64_errcode_exception_frame* frame);         // #PF
extern "C" void machine_check_cpp_enter(x64_standard_context* frame);        // #MC
extern "C" void simd_floating_point_cpp_enter(x64_standard_context* frame);    // #XM
extern "C" void virtualization_cpp_enter(x64_standard_context* frame);     // #VE
extern "C" void Control_Protection_cpp_enter(x64_standard_context* frame);
extern "C" void timer_cpp_enter(x64_standard_context* frame);
extern "C" void ipi_cpp_enter(x64_standard_context* frame);
extern "C" void asm_panic_cpp_enter(x64_standard_context* frame);
extern "C" void kthread_call_cpp_enter(x64_standard_context* frame);
// 汇编定义的异常处理入口点
extern "C" char div_by_zero_bare_enter;
extern "C" char breakpoint_bare_enter;
extern "C" char nmi_bare_enter;
extern "C" char overflow_bare_enter;
extern "C" char invalid_opcode_bare_enter;
extern "C" char general_protection_bare_enter;
extern "C" char double_fault_bare_enter;
extern "C" char page_fault_bare_enter;
extern "C" char machine_check_bare_enter;
extern "C" char invalid_tss_bare_enter;
extern "C" char simd_floating_point_bare_enter;
extern "C" char virtualization_bare_enter;
extern "C" char timer_bare_enter;
extern "C" char ipi_bare_enter;
extern "C" char asm_panic_bare_enter;
extern "C" char kthread_call_bare_enter;
extern uint32_t logical_processor_count;
extern uint32_t legacy_rotate_interrupt_alloc_id;