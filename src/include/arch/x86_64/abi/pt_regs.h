#pragma once
#include<cstdint>
/**
 * 几个关于magic的abi规范：
 * 1. self_specify_magic的高32位为0时标记无错误码的中断上下文，低32位标记向量号
 * 2.interrupt_context_specify_magic，设计意图是在rbp中rbp+8的是非法地址，
 * 但是是特定魔数，由此可以保证是那两种类型中断栈结构，由此又可以回溯
 * 为了兼容五级分页规定[56:63]为0x80,[0:55]是自由安排
 */
namespace Interrupt_context
 {
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
    uint64_t interrupt_context_specify_magic;// 用于标记无错误码的中断上下文,ptrace里面用于栈回溯开特例
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;    // 栈指针（仅特权级变化时压入）
    uint64_t ss;     // 栈段选择子（仅特权级变化时压入）
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
    uint64_t interrupt_context_specify_magic;// 用于标记有错误码的中断上下文,ptrace里面用于栈回溯开特例
    uint64_t errcode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;    // 栈指针（仅特权级变化时压入）
    uint64_t ss;     // 栈段选择子（仅特权级变化时压入）
};
}
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
struct x64_Interrupt_saved_context_no_errcode {//与Interrupt_context::x64_context_no_errcode必须完全等价，存在的意义是兼容C接口
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
    uint64_t interrupt_context_specify_magic;// 用于标记无错误码的中断上下文,ptrace里面用于栈回溯开特例
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;    // 栈指针（仅特权级变化时压入）
    uint64_t ss;     // 栈段选择子（仅特权级变化时压入）
};
struct x64_Interrupt_saved_context{//与Interrupt_context::x64_context必须完全等价，存在的意义是兼容C接口
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
    uint64_t interrupt_context_specify_magic;// 用于标记有错误码的中断上下文,ptrace里面用于栈回溯开特例
    uint64_t errcode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;    // 栈指针（仅特权级变化时压入）
    uint64_t ss;     // 栈段选择子（仅特权级变化时压入）
};