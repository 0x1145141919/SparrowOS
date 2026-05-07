#pragma once

#include <cstdint>

namespace Interrupt_context {
constexpr uint64_t interrupt_context_specify_no_magic = 0x8000000000000000;
constexpr uint64_t interrupt_context_specify_magic = 0x8000000000000001;

struct x64_context_no_errcode {
    uint64_t self_specify_magic;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rbp;
    uint64_t interrupt_context_specify_magic;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

struct x64_context {
    uint64_t self_specify_magic;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rbp;
    uint64_t interrupt_context_specify_magic;
    uint64_t errcode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
}

struct x64_Interrupt_saved_context_no_errcode {
    uint64_t self_specify_magic;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rbp;
    uint64_t interrupt_context_specify_magic;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

struct x64_Interrupt_saved_context {
    uint64_t self_specify_magic;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rbp;
    uint64_t interrupt_context_specify_magic;
    uint64_t errcode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
namespace panic_context {
    struct GDTR
{
    uint16_t limit;
    uint64_t base;
}__attribute__((packed));
struct IDTR
{
    uint16_t limit;
    uint64_t base;
}__attribute__((packed));
struct panic_error_specific_t{
    uint32_t hardware_errorcode;
    uint8_t interrupt_vec_num;
    uint8_t is_hadware_interrupt:1;//非policy中断都有上下文，但是
};    
struct x64_context {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsp;   
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rflags;
    uint64_t rip;
    GDTR gdtr;
    IDTR idtr;
    uint64_t IA32_EFER;
    uint64_t cr4;
    uint64_t cr3;
    uint64_t cr2;
    uint64_t cr0;
    uint64_t fs_base;
    uint64_t gs_base;
    panic_error_specific_t specific;
    uint16_t gs;         // ...
    uint16_t fs;
    uint16_t ss;
    uint16_t ds;
    uint16_t es;
    uint16_t cs;
};
}
