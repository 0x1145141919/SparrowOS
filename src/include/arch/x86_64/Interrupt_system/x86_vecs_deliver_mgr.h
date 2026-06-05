#pragma once
#include "Interrupt.h"
#include "arch/x86_64/abi/pt_regs.h"

// IDT 向量递送栈帧 — trampoline 在 GPR 与 iretq 之间插入了 vec
// 与 x64_standard_context 的区别：vec 位于 rbp 与 iret_complex 之间
struct x64_vec_demux_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rbp;
    uint64_t vec;
    iret_complex_context iret;
};

namespace Interrupt_module{
    constexpr uint8_t modloc_vec_demux=1;
    namespace vec_demux_events{
        namespace common_fail_reason_code{
            constexpr uint16_t INVALID_VEC=1;
            constexpr uint16_t INVALID_PROCESSOR_ID=2;
        }
        constexpr uint8_t init=0;
        namespace init_results{
            namespace fatal_reason_code{
                constexpr uint8_t SYMBOL_TABLE_UNAVAILABLE=1;
                constexpr uint8_t NOT_ALL_IDT_FOUND=2;
            }
        };
        constexpr uint8_t alloc_vec=1;
        namespace alloc_vec_results{
            namespace fail_reason_code{
                constexpr uint16_t NO_FREE_VEC=3;
                constexpr uint16_t BAD_FUNC_PTR=4;
                constexpr uint16_t SYM_NOT_FOUND=5;
            }
        }
        constexpr uint8_t free_vec=2;
        namespace free_vec_results{
            namespace fail_reason_code{
                constexpr uint16_t VEC_NOT_ALLOCED=1;
            }
        }
        constexpr uint8_t get_vec=3;
        constexpr uint8_t dispatch=4;
        namespace dispatch_results{
            namespace fatal_reason_code{
                constexpr uint16_t BAD_VEC_RECIEVED=1;
            }
        }
    }
}

class vec_demux{
    public:
    static void early_init();
    static void real_init();
    static uint8_t alloc_vec(interrupt_token_t* token,uint32_t processor_id,KURD_t&kurd);
    static uint8_t alloc_vec_by_apicid(interrupt_token_t* token,uint32_t x2_apicid,KURD_t&kurd);
    static KURD_t free_vec(uint8_t vec,uint32_t processor_id);
    static interrupt_token_t* get_vec(uint8_t vec,uint32_t processor_id,KURD_t& kurd);
};

constexpr uint8_t INVALID_INTERRUPT_VEC=0xFF;

// IDT 向量解复用入口 — asm vec_demux_common 调用此处
extern "C" void idt_vec_demux_entry(x64_vec_demux_frame* frame);
struct ipi_package_t{
    void*arg;
    uint64_t func;//函数指针刻意转成整数以避免类型系统干预
    uint32_t id;
    bool is_apicid;
    bool is_returnable;
};
// 统一外部接口 (IDT/FRED 共用)
extern "C" uint8_t out_interrupt_vec_alloc(interrupt_token_t* token,uint32_t processor_id,KURD_t*kurd);
extern "C" uint8_t out_interrupt_vec_alloc_by_apicid(interrupt_token_t* token,uint32_t x2_apicid,KURD_t*kurd);
extern "C" KURD_t out_interrupt_vec_free(uint8_t vec,uint32_t processor_id);
extern "C" interrupt_token_t* out_interrupt_vec_get(uint8_t vec,uint32_t processor_id,KURD_t* kurd);
extern "C" void broadcast_halt();//广播所有处理器的停机IPI
extern "C" void halt_on(uint32_t id, bool is_apicid);//is_apicid==true则id是apicid, 否则是processor_id
//抢占式ipi结果码定义：1为成功 2为抢占失败 3为超时 4为目标不存在 
extern "C" uint64_t fly_ipi_send(ipi_package_t*package);//返回ipi结果码
extern "C" __uint128_t ret_ipi_send(ipi_package_t*package);//高64bit是函数返回值，低64bit是ipi结果码
// FRED 分发器声明 (实现在 x86_vecs_deliver_mgr.cpp)
void fred_vec_demux_hw_dispatch(x64_standard_context* frame, uint8_t vec);
void fred_vec_demux_soft_dispatch(x64_standard_context* frame, uint8_t vec);
extern bool fred_support_catch_bit;