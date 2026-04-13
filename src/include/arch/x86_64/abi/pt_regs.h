#pragma once
#include<cstdint>
/**
 * 几个关于magic的abi规范：
 * 1. self_specify_magic的高32位为0时标记无错误码的中断上下文，低32位标记向量号
 * 2.interrupt_context_specify_magic，设计意图是在rbp中rbp+8的是非法地址，
 * 但是是特定魔数，由此可以保证是那两种类型中断栈结构，由此又可以回溯
 * 为了兼容五级分页规定[56:63]为0x80,[0:55]是自由安排
 */
struct iret_complex_context{
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;    // 长模式强制无条件压入
    uint64_t ss;     // 长模式强制无条件压入
};
struct fred_complex{
    uint64_t errcode:16;
    uint64_t reserved1:48;
    uint64_t rip;
    uint64_t cs:16;
    uint64_t stack_level:2;
    uint64_t WFE:1;
    uint64_t reserved2:45;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss:16;
    uint64_t STB:1;
    uint64_t SYS:1;
    uint64_t NMI:1;
    uint64_t reserved3:13;
    uint64_t vec:8;
    uint64_t reserved4:8;
    uint64_t type:4;
    uint64_t reserved5:4;
    uint64_t NEC:1;
    uint64_t Long:1;
    uint64_t nested_event:1;
    uint64_t reserved6:1;
    uint64_t instruction_length:4;
    uint64_t event_data;
    uint64_t reserved7;
};
static_assert(sizeof(fred_complex)==64,"fred_complex must size in 64byte");
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
    uint64_t gs_kernel_base;
    uint16_t gs;         // ...
    uint16_t fs;
    uint16_t ss;
    uint16_t ds;
    uint16_t es;
    uint16_t cs;
};
}
struct x64_standard_context{
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
    iret_complex_context iret_complex;
};
struct x64_errcode_exception_frame {
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
    uint64_t errcode;
    iret_complex_context iret_context;
};
struct x64_fred_context{
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
    fred_complex fred;
};
void filt_frame(x64_standard_context*frame,x64_errcode_exception_frame*raw);
void filt_frame(x64_standard_context*frame,x64_fred_context*raw);
void panic_frame(x64_standard_context*frame,panic_context::x64_context*panic_frame);
