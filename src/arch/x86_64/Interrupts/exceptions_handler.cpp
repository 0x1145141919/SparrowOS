#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "panic.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "util/kptrace.h"
#include "util/arch/x86-64/cpuid_intel.h"
static void double_fault_handler(x64_standard_context_v2* frame,uint64_t errcode){
    panic_info_inshort inshort={
        .is_bug=1,
        .is_policy=0,
        .is_hw_fault=0,
        .is_mem_corruption=0,
        .is_escalated=0
    };
    panic_context::x64_context panic_context;
    panic_frame(frame,&panic_context);
    Panic::panic(default_panic_behaviors_flags,"kernel_context cause #DF(Double Fault)", &panic_context,&inshort,KURD_t());
}

static void page_fault_handler(x64_standard_context_v2* frame,uint64_t errcode,vaddr_t liner_addr){
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态,根据错误码处理(可能是缺页异常)
        // TODO: 实现用户态缺页处理
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #PF(Page Fault)", &panic_context,&inshort,KURD_t());
    }
}

static void general_protection_handler(x64_standard_context_v2* frame,uint64_t errcode){
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态,根据错误码处理
        // TODO: 实现用户态 GPF 处理
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #GP(General Protection)", &panic_context,&inshort,KURD_t());
    }
}

static void invalid_tss_handler(x64_standard_context_v2* frame,uint64_t errcode){
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态,根据错误码处理
        // TODO: 实现用户态 Invalid TSS 处理
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        // 内核态, panic
        bsp_kout<<"[PANIC] Invalid TSS (#TS), errcode: "<<errcode<<kendl;
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #TS(Invalid TSS)", &panic_context,&inshort,KURD_t());
    }
}

void div_by_zero_cpp_enter(x64_standard_context_v2 *frame)
{
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态，根据
    }else if((IDT_CS(frame) & 0x3) == 0x0){
        // 内核态,panic
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #DE", &panic_context,&inshort,KURD_t());
    }
}

void debug_cpp_enter(x64_standard_context_v2 *frame)
{
}

void nmi_cpp_enter(x64_standard_context_v2 *frame)
{
    bsp_kout<<"NMI listening"<<kendl;
}

void breakpoint_cpp_enter(x64_standard_context_v2 *frame)
{
}

void overflow_cpp_enter(x64_standard_context_v2 *frame)
{
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态，根据
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #OF(Overflow)", &panic_context,&inshort,KURD_t());
    }
}

void invalid_opcode_cpp_enter(x64_standard_context_v2 *frame)
{
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态，根据
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        // 内核态, panic
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #UD(Invalid Opcode)", &panic_context,&inshort,KURD_t());
    }
}

void double_fault_cpp_enter(x64_standard_context_v2 *frame)
{
    double_fault_handler(frame, frame->core_ctx.idtctx.num.errocode);
}

void invalid_tss_cpp_enter(x64_standard_context_v2 *frame)
{
    invalid_tss_handler(frame, frame->core_ctx.idtctx.num.errocode);
}

void general_protection_cpp_enter(x64_standard_context_v2 *frame)
{
    general_protection_handler(frame, frame->core_ctx.idtctx.num.errocode);
}

void page_fault_cpp_enter(x64_standard_context_v2 *frame)
{
    vaddr_t linear_addr;
    asm volatile("mov %%cr2, %0" : "=r"(linear_addr));
    page_fault_handler(frame, frame->core_ctx.idtctx.num.errocode, linear_addr);
}

void machine_check_cpp_enter(x64_standard_context_v2 *frame)
{
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态，根据
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=0,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=1,
            .is_escalated=0
        };
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #MC(Machine Check)", &panic_context,&inshort,KURD_t());
    }
}

void simd_floating_point_cpp_enter(x64_standard_context_v2 *frame)
{
    if((IDT_CS(frame) & 0x3) == 0x3){
        // 用户态，根据
    } else if((IDT_CS(frame) & 0x3) == 0x0){
        panic_info_inshort inshort={
            .is_bug=0,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=1,
            .is_escalated=0
        };
        // 内核态, panic
        panic_context::x64_context panic_context;
        panic_frame(frame,&panic_context);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #XM(SIMD Floating Point)", &panic_context,&inshort,KURD_t());
    }
}

void virtualization_cpp_enter(x64_standard_context_v2 *frame)
{
}

void Control_Protection_cpp_enter(x64_standard_context_v2 *frame)
{
}

void asm_panic_cpp_enter(x64_standard_context_v2 *frame)
{
    panic_info_inshort inshort={
            .is_bug=0,
            .is_policy=1,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
    panic_context::x64_context panic_context;
    panic_frame(frame,&panic_context);
    Panic::panic(default_panic_behaviors_flags,"[ASM PANIC]",&panic_context,&inshort,raw_analyze(frame->rax));
}

void suprious_interrupt_cpp_enter(x64_standard_context_v2 *frame)
{
    bsp_kout<<"[suprious_interrupt_cpp_enter] fake interrupt detected on processor "<<fast_get_processor_id()<<kendl;
}

extern void fred_vec_demux_hw_dispatch(x64_standard_context_v2* frame, uint8_t vec);
extern void fred_vec_demux_soft_dispatch(x64_standard_context_v2* frame, uint8_t vec);

// ── FRED 异常路由器 ──
static void fred_exceptions_router(x64_standard_context_v2* context)
{
    uint8_t vec = context->core_ctx.fred.vec;
    uint64_t event_data = context->core_ctx.fred.event_data;

    switch (vec) {
    case x86_exceptions::DIVIDE_ERROR:
        div_by_zero_cpp_enter(context); return;
    case x86_exceptions::DEBUG:
        debug_cpp_enter(context); return;
    case x86_exceptions::BREAKPOINT:
        breakpoint_cpp_enter(context); return;
    case x86_exceptions::OVERFLOW:
        overflow_cpp_enter(context); return;
    case x86_exceptions::INVALID_OPCODE:
        invalid_opcode_cpp_enter(context); return;
    case x86_exceptions::DOUBLE_FAULT:
        double_fault_handler(context, context->core_ctx.fred.errcode); return;
    case x86_exceptions::INVALID_TSS:
        invalid_tss_handler(context, context->core_ctx.fred.errcode); return;
    case x86_exceptions::GENERAL_PROTECTION_FAULT:
        general_protection_handler(context, context->core_ctx.fred.errcode); return;
    case x86_exceptions::PAGE_FAULT:
        page_fault_handler(context, context->core_ctx.fred.errcode, (vaddr_t)event_data); return;
    case x86_exceptions::MACHINE_CHECK:
        machine_check_cpp_enter(context); return;
    case x86_exceptions::SIMD_FLOATING_POINT_EXCEPTION:
        simd_floating_point_cpp_enter(context); return;
    case x86_exceptions::VIRTUALIZATION_EXCEPTION:
        virtualization_cpp_enter(context); return;
    default:
        break;
    }
}

void fred_common_enter(x64_standard_context_v2* context)
{
    switch (context->core_ctx.fred.type) {
    case 0:   // 外部中断
    {
        fred_vec_demux_hw_dispatch(context, context->core_ctx.fred.vec);
        return;
    }
    case 2:   // NMI
    {
        nmi_cpp_enter(context);
        return;
    }
    case 3:   // 异常
        fred_exceptions_router(context);
        return;
    case 4:   // 软中断
    {
        fred_vec_demux_soft_dispatch(context, context->core_ctx.fred.vec);
        return;
    }
    case 7:   // SYSCALL/SYSENTER
        // TODO: 用户态系统调用入口
        return;
    default:
        return;
    }
}

extern "C" void fred_user_cpp_enter(x64_standard_context_v2*context){
    fred_common_enter(context);
}
extern "C" void fred_supervisor_cpp_entry(x64_standard_context_v2*context){
    fred_common_enter(context);
}

// ── filt_frame: FRED → x64_standard_context_v2 ──
void filt_frame(x64_standard_context_v2* frame, x64_fred_context* raw)
{
    frame->rax = raw->rax;
    frame->rbx = raw->rbx;
    frame->rcx = raw->rcx;
    frame->rdx = raw->rdx;
    frame->rsi = raw->rsi;
    frame->rdi = raw->rdi;
    frame->r8 = raw->r8;
    frame->r9 = raw->r9;
    frame->r10 = raw->r10;
    frame->r11 = raw->r11;
    frame->r12 = raw->r12;
    frame->r13 = raw->r13;
    frame->r14 = raw->r14;
    frame->r15 = raw->r15;
    frame->rbp = raw->rbp;

    // FRED frame 的 RIP/CS/RFLAGS/RSP/SS 通过 fred_complex 复制到 core_ctx
    // 注意: fred_complex+8 与 iret_complex_context 布局兼容
    frame->core_ctx.fred = raw->fred;
}

// ── filt_frame: 带错误码 IDT → x64_standard_context_v2 ──
void filt_frame(x64_standard_context_v2* frame, x64_errcode_exception_frame* raw)
{
    frame->rax = raw->rax;
    frame->rbx = raw->rbx;
    frame->rcx = raw->rcx;
    frame->rdx = raw->rdx;
    frame->rsi = raw->rsi;
    frame->rdi = raw->rdi;
    frame->r8 = raw->r8;
    frame->r9 = raw->r9;
    frame->r10 = raw->r10;
    frame->r11 = raw->r11;
    frame->r12 = raw->r12;
    frame->r13 = raw->r13;
    frame->r14 = raw->r14;
    frame->r15 = raw->r15;
    frame->rbp = raw->rbp;

    // 将 raw 的 errcode + iret_context 映射到 core_ctx.idtctx
    frame->core_ctx.idtctx.num.errocode = raw->errcode;
    frame->core_ctx.idtctx.iret.rip    = raw->iret_context.rip;
    frame->core_ctx.idtctx.iret.cs     = raw->iret_context.cs;
    frame->core_ctx.idtctx.iret.rflags = raw->iret_context.rflags;
    frame->core_ctx.idtctx.iret.rsp    = raw->iret_context.rsp;
    frame->core_ctx.idtctx.iret.ss     = raw->iret_context.ss;
}


