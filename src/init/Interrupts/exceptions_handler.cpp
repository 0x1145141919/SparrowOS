#include "arch/x86_64/abi/pt_regs.h"

namespace {
[[noreturn]] void halt_forever()
{
    for (;;) {
        asm volatile("cli; hlt");
    }
}
}

extern "C" void div_by_zero_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void breakpoint_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void nmi_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void overflow_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void invalid_opcode_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void general_protection_cpp_enter(x64_errcode_exception_frame* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void double_fault_cpp_enter(x64_errcode_exception_frame* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void page_fault_cpp_enter(x64_errcode_exception_frame* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void machine_check_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void invalid_tss_cpp_enter(x64_errcode_exception_frame* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void simd_floating_point_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}

extern "C" void virtualization_cpp_enter(x64_standard_context* frame)
{
    (void)frame;
    halt_forever();
}
