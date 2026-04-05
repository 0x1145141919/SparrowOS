#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "panic.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "util/kptrace.h"
void (*global_ipi_handler)()=nullptr;
void div_by_zero_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    Interrupt_context::x64_context_no_errcode*context=(Interrupt_context::x64_context_no_errcode*)(frame);
    if(context->self_specify_magic!=0){
        Panic::panic(default_panic_behaviors_flags,"div_by_zero_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((context->cs&0x3)==0x3){//用户态，根据

    }else if((context->cs&0x3)==0x0){//内核态,panic
        panic_context::x64_context panic_context=Panic::convert_to_panic_context(frame);
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

void debug_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{

}

void nmi_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{

}

void breakpoint_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{

}
void overflow_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    Interrupt_context::x64_context_no_errcode* context = (Interrupt_context::x64_context_no_errcode*)(frame);
    if(context->self_specify_magic != 0x4){
        Panic::panic(default_panic_behaviors_flags,"overflow_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((context->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((context->cs & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #OF(Overflow)", &panic_context,&inshort,KURD_t());
    }
}

void invalid_opcode_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    Interrupt_context::x64_context_no_errcode* context = (Interrupt_context::x64_context_no_errcode*)(frame);
    if(context->self_specify_magic != 0x6){
        Panic::panic(default_panic_behaviors_flags,"invalid_opcode_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((context->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((context->cs & 0x3) == 0x0){
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        // 内核态, panic
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #UD(Invalid Opcode)", &panic_context,&inshort,KURD_t());
    }
}

void double_fault_cpp_enter(x64_Interrupt_saved_context *frame)
{
    if(frame->self_specify_magic != (0x8 | (1ULL << 32))){
        Panic::panic(default_panic_behaviors_flags,"double_fault_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
    bsp_kout<<"double_fault_cpp_enter"<<kendl;
    panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame, 8);
    Panic::panic(default_panic_behaviors_flags,"kernel_context cause #DF(Double Fault)", &panic_context,&inshort,KURD_t());
}

void invalid_tss_cpp_enter(x64_Interrupt_saved_context *frame)
{
    if(frame->self_specify_magic != (0xA | (1ULL << 32))){
        Panic::panic(default_panic_behaviors_flags,"invalid_tss_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((frame->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->cs & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame, 0xA);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #TS(Invalid TSS)", &panic_context,&inshort,KURD_t());
    }
}

void general_protection_cpp_enter(x64_Interrupt_saved_context *frame)
{
    if(frame->self_specify_magic != (0xD | (1ULL << 32))){
        Panic::panic(default_panic_behaviors_flags,"general_protection_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((frame->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->cs & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
        GlobalKernelStatus=PANIC;
        symbol_entry*lastest_rip =ksymmanager::get_entry_near_addr(frame->rip);
        bsp_kout<<"lastest_rip:"<<(void*)lastest_rip->address<<" at "<<lastest_rip->name <<kendl;
        else_trace((void*)frame->rbp);
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame, 0xD);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #GP(General Protection)", &panic_context,&inshort,KURD_t());
    }
}

void page_fault_cpp_enter(x64_Interrupt_saved_context *frame)
{
    if(frame->self_specify_magic != (0xE | (1ULL << 32))){
        Panic::panic(default_panic_behaviors_flags,"page_fault_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((frame->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->cs & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=1,
            .is_escalated=0
        };
        GlobalKernelStatus=PANIC;
        symbol_entry*lastest_rip =ksymmanager::get_entry_near_addr(frame->rip);
        bsp_kout<<"kernel oops in page_fault_cpp_enter"<<kendl;
        bsp_kout<<"lastest_rip:"<<(void*)lastest_rip->address<<" at "<<lastest_rip->name <<kendl;
        else_trace((void*)frame->rbp);
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame, 0xE);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #PF(Page Fault)", &panic_context,&inshort,KURD_t());
    }
}

void machine_check_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    if(frame->self_specify_magic != (0x12 | (1ULL << 32))){
        Panic::panic(default_panic_behaviors_flags,"machine_check_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((frame->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->cs & 0x3) == 0x0){
        // 内核态, panic
        panic_info_inshort inshort={
            .is_bug=0,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=1,
            .is_escalated=0
        };
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #MC(Machine Check)", &panic_context,&inshort,KURD_t());
    }
}

void simd_floating_point_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    if(frame->self_specify_magic != (0x13 | (1ULL << 32))){
       Panic::panic(default_panic_behaviors_flags,"simd_floating_point_cpp_enter bad magic",nullptr,nullptr,KURD_t());
    }
    if((frame->cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->cs & 0x3) == 0x0){
        panic_info_inshort inshort={
            .is_bug=0,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=1,
            .is_escalated=0
        };
        // 内核态, panic
        panic_context::x64_context panic_context = Panic::convert_to_panic_context(frame);
        Panic::panic(default_panic_behaviors_flags,"kernel_context cause #XM(SIMD Floating Point)", &panic_context,&inshort,KURD_t());
    }
}

void virtualization_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{

}

void Control_Protection_cpp_enter(x64_Interrupt_saved_context *frame)
{
    
}



void ipi_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    x2apic::x2apic_driver::write_eoi();   
    global_ipi_handler();
}

void asm_panic_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    panic_info_inshort inshort={
            .is_bug=0,
            .is_policy=1,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
    panic_context::x64_context panic_context=Panic::convert_to_panic_context(frame);
    KURD_t raw_analyze(uint64_t raw);
    Panic::panic(default_panic_behaviors_flags,"[ASM PANIC]",&panic_context,&inshort,raw_analyze(frame->rax));
}
