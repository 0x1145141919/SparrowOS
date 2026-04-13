#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "panic.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "util/kptrace.h"
#include "util/arch/x86-64/cpuid_intel.h"

void (*global_ipi_handler)()=nullptr;
void double_fault_handler(x64_standard_context* frame,uint64_t errcode){
    // 双重故障只能发生在内核态,直接 panic
    bsp_kout<<"[PANIC] Double Fault (#DF), errcode: "<<errcode<<kendl;
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
void page_fault_handler(x64_standard_context* frame,uint64_t errcode,vaddr_t liner_addr){
    if((frame->iret_complex.cs & 0x3) == 0x3){
        // 用户态,根据错误码处理(可能是缺页异常)
        // TODO: 实现用户态缺页处理
    } else if((frame->iret_complex.cs & 0x3) == 0x0){
        // 内核态, panic
        bsp_kout<<"[PANIC] Page Fault (#PF) at addr: "<<liner_addr<<", errcode: "<<errcode<<kendl;
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
void general_protection_handler(x64_standard_context* frame,uint64_t errcode){
    if((frame->iret_complex.cs & 0x3) == 0x3){
        // 用户态,根据错误码处理
        // TODO: 实现用户态 GPF 处理
    } else if((frame->iret_complex.cs & 0x3) == 0x0){
        // 内核态, panic
        bsp_kout<<"[PANIC] General Protection (#GP), errcode: "<<errcode<<kendl;
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
void invalid_tss_handler(x64_standard_context* frame,uint64_t errcode){
    if((frame->iret_complex.cs & 0x3) == 0x3){
        // 用户态,根据错误码处理
        // TODO: 实现用户态 Invalid TSS 处理
    } else if((frame->iret_complex.cs & 0x3) == 0x0){
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
void div_by_zero_cpp_enter(x64_standard_context *frame)
{
    if((frame->iret_complex.cs&0x3)==0x3){//用户态，根据

    }else if((frame->iret_complex.cs&0x3)==0x0){//内核态,panic
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

void debug_cpp_enter(x64_standard_context *frame)
{

}

void nmi_cpp_enter(x64_standard_context *frame)
{

}

void breakpoint_cpp_enter(x64_standard_context *frame)
{

}
void overflow_cpp_enter(x64_standard_context *frame)
{

    if((frame->iret_complex.cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->iret_complex.cs & 0x3) == 0x0){
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

void invalid_opcode_cpp_enter(x64_standard_context *frame)
{
    if((frame->iret_complex.cs & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->iret_complex.cs & 0x3) == 0x0){
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

void double_fault_cpp_enter(x64_errcode_exception_frame *frame)
{
    panic_info_inshort inshort={
            .is_bug=1,
            .is_policy=0,
            .is_hw_fault=0,
            .is_mem_corruption=0,
            .is_escalated=0
        };
    bsp_kout<<"double_fault_cpp_enter"<<kendl;
   x64_standard_context standard;
   filt_frame(&standard,frame);
    double_fault_handler(&standard,frame->errcode);
}

void invalid_tss_cpp_enter(x64_errcode_exception_frame *frame)
{
    x64_standard_context standard;
   filt_frame(&standard,frame);
        invalid_tss_handler(&standard,frame->errcode);
}

void general_protection_cpp_enter(x64_errcode_exception_frame *frame)
{

    x64_standard_context standard;
   filt_frame(&standard,frame);
        general_protection_handler(&standard,frame->errcode);
}

void page_fault_cpp_enter(x64_errcode_exception_frame *frame)
{
    x64_standard_context standard;
   filt_frame(&standard,frame);
   vaddr_t linear_addr;asm volatile("mov %%cr2, %0" : "=r"(linear_addr));
   page_fault_handler(&standard,frame->errcode,linear_addr);
   
}

void machine_check_cpp_enter(x64_standard_context *frame)
{

    if((frame->iret_complex.cs  & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->iret_complex.cs  & 0x3) == 0x0){
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

void simd_floating_point_cpp_enter(x64_standard_context *frame)
{

    if((frame->iret_complex.cs  & 0x3) == 0x3){
        // 用户态，根据
    } else if((frame->iret_complex.cs  & 0x3) == 0x0){
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

void virtualization_cpp_enter(x64_standard_context *frame)
{

}

void Control_Protection_cpp_enter(x64_errcode_exception_frame *frame)
{
    
}



void ipi_cpp_enter(x64_standard_context *frame)
{
    x2apic::x2apic_driver::write_eoi();   
    global_ipi_handler();
}

void asm_panic_cpp_enter(x64_standard_context *frame)
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
extern "C" void fred_user_cpp_enter(x64_fred_context*context){

}
extern "C" void fred_supervisor_cpp_entry(x64_fred_context*context){
    
}

void filt_frame(x64_standard_context*frame, x64_errcode_exception_frame*raw)
{
    // 从带错误码的异常帧中提取寄存器值到标准上下文
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
    
    // 复制IRET上下文
    frame->iret_complex.rip = raw->iret_context.rip;
    frame->iret_complex.cs = raw->iret_context.cs;
    frame->iret_complex.rflags = raw->iret_context.rflags;
    frame->iret_complex.rsp = raw->iret_context.rsp;
    frame->iret_complex.ss = raw->iret_context.ss;
}

void filt_frame(x64_standard_context*frame, x64_fred_context*raw)
{
    // 从FRED上下文提取寄存器值到标准上下文
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
    
    // 从FRED复杂结构中提取IRET相关字段
    frame->iret_complex.rip = raw->fred.rip;
    frame->iret_complex.cs = raw->fred.cs;
    frame->iret_complex.rflags = raw->fred.rflags;
    frame->iret_complex.rsp = raw->fred.rsp;
    frame->iret_complex.ss = raw->fred.ss;
}

void panic_frame(x64_standard_context*frame, panic_context::x64_context*panic_frame)
{
    // 直接从标准上下文中复制已有寄存器值
    panic_frame->rax = frame->rax;
    panic_frame->rbx = frame->rbx;
    panic_frame->rcx = frame->rcx;
    panic_frame->rdx = frame->rdx;
    panic_frame->rsi = frame->rsi;
    panic_frame->rdi = frame->rdi;
    panic_frame->r8 = frame->r8;
    panic_frame->r9 = frame->r9;
    panic_frame->r10 = frame->r10;
    panic_frame->r11 = frame->r11;
    panic_frame->r12 = frame->r12;
    panic_frame->r13 = frame->r13;
    panic_frame->r14 = frame->r14;
    panic_frame->r15 = frame->r15;
    panic_frame->rbp = frame->rbp;
    panic_frame->rip = frame->iret_complex.rip;
    panic_frame->cs = static_cast<uint16_t>(frame->iret_complex.cs);
    panic_frame->rflags = frame->iret_complex.rflags;
    panic_frame->rsp = frame->iret_complex.rsp;
    panic_frame->ss = static_cast<uint16_t>(frame->iret_complex.ss);
    
    // 使用内联汇编获取其他寄存器值
    uint16_t ds_val, es_val, fs_val, gs_val;
    uint64_t cr0_val, cr2_val, cr3_val, cr4_val, efer_val;
    uint64_t fs_base_val, gs_base_val;
    
    asm volatile("movw %%ds, %0" : "=r"(ds_val));
    asm volatile("movw %%es, %0" : "=r"(es_val));
    asm volatile("movw %%fs, %0" : "=r"(fs_val));
    asm volatile("movw %%gs, %0" : "=r"(gs_val));
    
    asm volatile("mov %%cr0, %0" : "=r"(cr0_val));
    asm volatile("mov %%cr2, %0" : "=r"(cr2_val));
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    asm volatile("mov %%cr4, %0" : "=r"(cr4_val));
    
    // 读取EFER MSR
    efer_val = rdmsr(0xC0000080); // IA32_EFER
    
    // 读取FS/GS基址
    fs_base_val = rdmsr(0xC0000100); // IA32_FS_BASE
    gs_base_val = rdmsr(0xC0000101); // IA32_GS_BASE
    
    // 获取GDTR和IDTR
    asm volatile("sgdt %0" : "=m"(panic_frame->gdtr));
    asm volatile("sidt %0" : "=m"(panic_frame->idtr));
    
    // 设置获取的值
    panic_frame->ds = ds_val;
    panic_frame->es = es_val;
    panic_frame->fs = fs_val;
    panic_frame->gs = gs_val;
    panic_frame->cr0 = cr0_val;
    panic_frame->cr2 = cr2_val;
    panic_frame->cr3 = cr3_val;
    panic_frame->cr4 = cr4_val;
    panic_frame->IA32_EFER = efer_val;
    panic_frame->fs_base = fs_base_val;
    panic_frame->gs_base = gs_base_val;
    panic_frame->gs_kernel_base=rdmsr(msr::syscall::IA32_KERNEL_GS_BASE);
}
