#include "Interrupt_system/loacl_processor.h"
#include "util/kout.h"
#include "util/arch/x86-64/cpuid_intel.h"
void illeagale_interrupt_post(uint16_t vector)
{
    bsp_kout<<now<<"[x64_local_processor]illegal interrupt on vector"<<vector<<"and apicid:"<<query_x2apicid()<<kendl;
}
template<uint8_t Vec>
__attribute__((interrupt))
void illegal_interrupt_handler(interrupt_frame* frame){
    illeagale_interrupt_post(Vec);
}
logical_idt template_idt[256];

template<uint8_t Vec>
struct illegal_idt_filler {
    static void fill(logical_idt* idt) {
        idt[Vec].handler   = (void*)illegal_interrupt_handler<Vec>;
        idt[Vec].type      = 0xE;
        idt[Vec].ist_index = 0;
        idt[Vec].dpl       = 0;

        illegal_idt_filler<Vec - 1>::fill(idt);
    }
};
template<>
struct illegal_idt_filler<0> {
static void fill(logical_idt* idt) {
        idt[0].handler   = (void*)illegal_interrupt_handler<0>;
        idt[0].type      = 0xE;
        idt[0].ist_index = 0;
        idt[0].dpl       = 0;
    }
};

void x86_smp_processors_container::template_idt_init(){
    illegal_idt_filler<255>::fill(template_idt);
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
    template_idt[ivec::LAPIC_TIMER].handler=(void*)&timer_bare_enter;
    template_idt[ivec::LAPIC_TIMER].ist_index=5;
    template_idt[ivec::IPI].handler=(void*)&ipi_bare_enter;
    template_idt[ivec::KTHREAD_CALL].handler=(void*)&kthread_call_bare_enter;
    template_idt[ivec::KTHREAD_CALL].ist_index=5;
}