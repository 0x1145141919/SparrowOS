#pragma once
#include <stdint.h>
namespace AP_Init_error_observing_protocol{ 
    struct realmode_final_stack_frame {
    uint16_t magic;      // 最后入栈
    uint16_t cr0;
    uint16_t gs;         // ...
    uint16_t fs;
    uint16_t ss;
    uint16_t ds;
    uint16_t es;
    uint16_t di;         // 这些是pushad保存的
    uint16_t si;
    uint16_t bp;
    uint16_t sp;         // 这是pushad前的栈顶
    uint16_t dx;
    uint16_t cx;
    uint16_t bx;
    uint16_t ax;
    uint16_t cs;         // CPU自动保存
    uint16_t ip;
    uint32_t eflags;
};
struct pemode_final_stack_frame {
    uint32_t magic;      // 最后入栈
    uint32_t IA32_EFER;
    uint32_t cr4;
    uint32_t cr3;
    uint32_t cr2;
    uint32_t cr0;
    uint32_t gs;         // ...
    uint32_t fs;
    uint32_t ss;
    uint32_t ds;
    uint32_t es;
    uint32_t edi;       // pushad保存
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;       // 这是pushad前的esp
    uint32_t edx;
    uint32_t ecx;
    uint32_t ebx;
    uint32_t eax;
    uint32_t cs;        // CPU自动保存
    uint32_t eip;
    uint32_t eflags;
};
struct pemode_final_stack_frame_with_errcode{ 
    uint32_t magic;      // 最后入栈
    uint32_t IA32_EFER;
    uint32_t cr4;
    uint32_t cr3;
    uint32_t cr2;
    uint32_t cr0;
    uint32_t gs;         // ...
    uint32_t fs;
    uint32_t ss;
    uint32_t ds;
    uint32_t es;
    uint32_t edi;       // pushad保存
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;       // 这是pushad前的esp
    uint32_t edx;
    uint32_t ecx;
    uint32_t ebx;
    uint32_t eax;
    uint32_t errcode;
    uint32_t cs;        // CPU自动保存
    uint32_t eip;
    uint32_t eflags;
};
struct longmode_final_stack_frame {
    uint64_t magic;      // 最后入栈
    uint64_t IA32_EFER;
    uint64_t gs;         // ...
    uint64_t fs;
    uint64_t ss;
    uint64_t ds;
    uint64_t es;
    uint64_t cr4;
    uint64_t cr3;
    uint64_t cr2;
    uint64_t cr0;
    uint64_t r15;        // 通用寄存器
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t cs;        // CPU自动保存
    uint64_t rip;
    uint64_t rflags;
};
struct longmode_final_stack_frame_with_errcode{ 
    uint64_t magic;      // 最后入栈
    uint64_t IA32_EFER;
    uint64_t gs;         // ...
    uint64_t fs;
    uint64_t ss;
    uint64_t ds;
    uint64_t es;
    uint64_t cr4;
    uint64_t cr3;
    uint64_t cr2;
    uint64_t cr0;
    uint64_t r15;        // 通用寄存器
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t errcode;
    uint64_t cs;        // CPU自动保存
    uint64_t rip;
    uint64_t rflags;
};
constexpr uint16_t realmode_magic=0x15;
constexpr uint32_t PE_FINAL_STACK_NO_ERRCODE_TOP_MAGIC=0x11;
constexpr uint32_t PE_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC=0x12;
constexpr uint64_t LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC=0x13;
constexpr uint64_t LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC=0x14;
}
struct check_point{//不论长模式/保护模式/实模式都使用这个检查点结构
    uint32_t success_word;//ap和bsp约定成功的标志，不同的ap设计上此值必须不同，且必须不能为0，0xFFFFFFFF,推荐同一个ap不同检查点也不要只使用一个值
    uint8_t failure_flags;//[0]位标志是否失败，[1]标志是否触发异常，
    uint8_t failure_caused_excption_num;//触发异常的vector
    uint16_t failure_final_stack_top;//在停机前的栈顶
    uint16_t check_point_id;
    uint16_t reserved[3];
};
extern "C" check_point realmode_enter_checkpoint;
extern "C" check_point pemode_enter_checkpoint;
extern "C" char AP_realmode_start;
extern "C" uint32_t assigned_processor_id;
extern  check_point longmode_enter_checkpoint;
extern  check_point init_finish_checkpoint;

