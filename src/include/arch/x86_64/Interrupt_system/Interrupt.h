#pragma once
#include <stdint.h>
#include "fixed_interrupt_vectors.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "abi/os_error_definitions.h"
#include "abi/arch_code.h"
extern void (*global_ipi_handler)();
typedef void (*soft_interrupt_func_t)(x64_standard_context_v2* context);
struct ipi_descrioptor_t{
    soft_interrupt_func_t func;
    bool is_no_return;
};
constexpr uint64_t TOKEN_FLAG_MASK_TOKEN_SCHEDULE=0x1;
struct interrupt_token_t{
    __uint128_t token_private;
    uint64_t flags;
    uint64_t (*func)(interrupt_token_t*token);
};
/**
 * 中断管理器，管理着每个cpu的中断描述符表和本地apic
 * 当然，调用时必须上报其apic__id
 */
 // namespace gdtentry

extern "C" void div_by_zero_cpp_enter(x64_standard_context_v2*frame);
extern "C" void debug_cpp_enter(x64_standard_context_v2* frame);
extern "C" void nmi_cpp_enter(x64_standard_context_v2* frame);
extern "C" void breakpoint_cpp_enter(x64_standard_context_v2* frame);
extern "C" void overflow_cpp_enter(x64_standard_context_v2* frame);
extern "C" void invalid_opcode_cpp_enter(x64_standard_context_v2* frame);        // #UD
extern "C" void double_fault_cpp_enter(x64_errcode_exception_frame* frame); // #DF
extern "C" void invalid_tss_cpp_enter(x64_errcode_exception_frame* frame);   // #TS    
extern "C" void general_protection_cpp_enter(x64_errcode_exception_frame* frame); // #GP
extern "C" void page_fault_cpp_enter(x64_errcode_exception_frame* frame);         // #PF
extern "C" void machine_check_cpp_enter(x64_standard_context_v2* frame);        // #MC
extern "C" void simd_floating_point_cpp_enter(x64_standard_context_v2* frame);    // #XM
extern "C" void virtualization_cpp_enter(x64_standard_context_v2* frame);     // #VE
extern "C" void Control_Protection_cpp_enter(x64_standard_context_v2* frame);
extern "C" [[noreturn]] void timer_cpp_enter(x64_standard_context_v2* frame);
// 汇编定义的异常处理入口点typedef void (*soft_interrupt_func_t)(x64_standard_context_v2* context);
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
extern uint32_t logical_processor_count;
extern uint32_t legacy_rotate_interrupt_alloc_id;