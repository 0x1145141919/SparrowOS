#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "util/kout.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kptrace.h"
#include "util/OS_utils.h"
#include "panic.h"

logical_idt template_idt[256];
IDTEntry global_idt[256];
hard_interrupt_func_t*all_processors_interrupt_functions=nullptr;
extern void kthread_call_cpp_enter(x64_standard_context *frame,uint8_t vec);
extern void ipi_cpp_enter(x64_standard_context *frame,uint8_t vec);
extern void asm_panic_cpp_enter(x64_standard_context *frame,uint8_t vec);
extern void suprious_interrupt_cpp_enter(x64_standard_context *frame,uint8_t vec);

// 软中断函数表 - 在 idt_vec_dispatch_mgr::Init() 中初始化
soft_interrupt_func_t soft_interrupt_functions[256];

extern "C" const IDTR global_idtr = {
    .limit = sizeof(global_idt) - 1,
    .base = (uint64_t)global_idt
};

// 移除了 static，使其可以被其他编译单元通过 extern 声明调用
void template_idt_apply_region(uint8_t from_vec, uint8_t to_vec)
{
    for (uint16_t v = from_vec; v <= to_vec; v++) {
        logical_idt* src = &template_idt[v];
        if (!src->handler)
            continue;
        uint64_t addr = (uint64_t)src->handler;
        global_idt[v].offset_low   = addr & 0xFFFF;
        global_idt[v].offset_mid   = (addr >> 16) & 0xFFFF;
        global_idt[v].offset_high  = (addr >> 32) & 0xFFFFFFFF;
        global_idt[v].segment_selector = 0x08;
        global_idt[v].ist_index    = src->ist_index;
        global_idt[v].type         = src->type ? src->type : 0xE;
        global_idt[v].dpl          = src->dpl;
        global_idt[v].present      = 1;
        global_idt[v].reserved1    = 0;
        global_idt[v].reserved2    = 0;
        global_idt[v].reserved3    = 0;
    }
}
void x86_smp_processors_container::exceptions_idt_init(){
    template_idt[ivec::DIVIDE_ERROR].handler=(void*)&div_by_zero_bare_enter;
    template_idt[ivec::NMI].handler=(void*)&nmi_bare_enter;
    template_idt[ivec::NMI].ist_index=3;
    template_idt[ivec::BREAKPOINT].handler=(void*)&breakpoint_bare_enter;
    template_idt[ivec::BREAKPOINT].ist_index=4;
    template_idt[ivec::BREAKPOINT].dpl=3;
    template_idt[ivec::OVERFLOW].handler=(void*)&overflow_bare_enter;
    template_idt[ivec::OVERFLOW].dpl=3;
    template_idt[ivec::INVALID_OPCODE].handler=(void*)&invalid_opcode_bare_enter;
    template_idt[ivec::DOUBLE_FAULT].handler=(void*)&double_fault_bare_enter;
    template_idt[ivec::DOUBLE_FAULT].ist_index=1;
    template_idt[ivec::INVALID_TSS].handler=(void*)&invalid_tss_bare_enter;
    template_idt[ivec::GENERAL_PROTECTION_FAULT].handler=(void*)&general_protection_bare_enter;
    template_idt[ivec::PAGE_FAULT].handler=(void*)&page_fault_bare_enter;
    template_idt[ivec::MACHINE_CHECK].handler=(void*)&machine_check_bare_enter;
    template_idt[ivec::MACHINE_CHECK].ist_index=2;
    template_idt[ivec::SIMD_FLOATING_POINT_EXCEPTION].handler=(void*)&simd_floating_point_bare_enter;
    template_idt[ivec::VIRTUALIZATION_EXCEPTION].handler=(void*)&virtualization_bare_enter;
    template_idt_apply_region(0, 31);
}
extern "C" void exceptions_init(){
    x86_smp_processors_container::exceptions_idt_init();
}
