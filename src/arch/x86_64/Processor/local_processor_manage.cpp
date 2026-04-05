#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/AP_Init_error_observing_protocol.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "firmware/gSTResloveAPIs.h"
#include "firmware/ACPI_APIC.h"
#include "util/kout.h"
#include "linker_symbols.h"
#include "arch/x86_64/Interrupt_system/fixed_interrupt_vectors.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/OS_utils.h"
#include "memory/AddresSpace.h"
#include "memory/all_pages_arr.h"
#include "memory/phyaddr_accessor.h"
#include "memory/FreePagesAllocator.h"
#include "memory/kpoolmemmgr.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/bitmap.h"
#include "panic.h"
#include <util/kptrace.h>
#include "ktime.h"

x64_local_processor *x86_smp_processors_container::local_processor_interrupt_mgr_array[x86_smp_processors_container::max_processor_count];
constexpr TSSDescriptorEntry kspace_TSS_entry = {
    .limit = sizeof(TSSentry) ,
    .base0 = 0,
    .base1 = 0,
    .type = 0x9, // 64位可用TSS
    .zero = 0,
    .dpl = 0,
    .p = 1,
    .limit1 = (sizeof(TSSentry) - 1) >> 16,
    .avl = 0,
    .reserved = 0,
    .g = 0, // 字节粒度
    .base2 = 0,
    .base3 = 0,
    .reserved2 = 0
};

int x86_smp_processors_container::regist_core(uint32_t processor_id) 
{
    uint64_t bsp_specify=rdmsr(msr::apic::IA32_APIC_BASE);
    if(bsp_specify&(1<<8)){
        local_processor_interrupt_mgr_array[0]=new x64_local_processor(0);
        return OS_SUCCESS;
    }else{
        alloc_flags_t flags=default_flags;
        flags.force_first_linekd_heap=true;
        local_processor_interrupt_mgr_array[processor_id]=new(flags) x64_local_processor(processor_id);
        return OS_SUCCESS;
    }
    return OS_RESOURCE_CONFILICT;

}

struct load_resources_struct{
        GDTR* gdtr;
        IDTR* idtr;
        uint64_t K_CS_selector;
        uint64_t K_DS_selector;
        uint64_t TSS_selector;
    };
extern logical_idt template_idt[256];
extern "C" void runtime_processor_regist(load_resources_struct* resources);
x64_local_processor::x64_local_processor(uint32_t alloced_id)
{
    x2apicid_t id=query_x2apicid();
    this->apic_id=id;
    this->processor_id=alloced_id;
    gdt.entries[K_cs_idx]=kspace_CS_entry;
    gdt.entries[K_ds_ss_idx]=kspace_DS_SS_entry;
    gdt.entries[U_cs_idx]=userspace_CS_entry;
    gdt.entries[U_ds_ss_idx]=userspace_DS_SS_entry;
    gdt.tss_entry=kspace_TSS_entry;
    TSSDescriptorEntry& tss_entry=gdt.tss_entry;
    tss_entry.base0=static_cast<uint32_t>(reinterpret_cast<uint64_t>(&this->tss))&base0_mask;
    tss_entry.base1=static_cast<uint32_t>(reinterpret_cast<uint64_t>(&this->tss)>>16)&base1_mask;
    tss_entry.base2=static_cast<uint32_t>(reinterpret_cast<uint64_t>(&this->tss)>>24)&base2_mask;
    tss_entry.base3=static_cast<uint32_t>(reinterpret_cast<uint64_t>(&this->tss)>>32)&base3_mask;
    KURD_t kurd=KURD_t();
    GDTR gdtr={
        .limit=sizeof(gdt)-1,
        .base=reinterpret_cast<uint64_t>(&gdt)        
    };
    uint16_t kcs_selector=K_cs_idx << 3;
    auto tran_to_phy_IDT_ENTRY=[](logical_idt entry)->IDTEntry{
        IDTEntry result={0};
        result.offset_low=static_cast<uint16_t>(reinterpret_cast<uint64_t>(entry.handler)&0xFFFF);
        result.offset_mid=static_cast<uint16_t>((reinterpret_cast<uint64_t>(entry.handler)>>16)&0xFFFF);
        result.offset_high=static_cast<uint32_t>((reinterpret_cast<uint64_t>(entry.handler)>>32)&0xFFFFFFFF);
        result.type=entry.type;
        result.ist_index=entry.ist_index;
        result.dpl=entry.dpl;
        result.present=1;
        result.segment_selector=K_cs_idx<<3;
        return result;
    };
    for(uint16_t i=0;i<256;i++){
        idt[i]=tran_to_phy_IDT_ENTRY(template_idt[i]);
    }
    IDTR idtr={
        .limit=256*sizeof(IDTEntry)-1,
        .base=(uint64_t)idt
    };
    uint16_t tss_selector = gdt_headcount << 3;
    load_resources_struct resources={
        .gdtr=&gdtr,
        .idtr=&idtr,
        .K_CS_selector=K_cs_idx<<3,
        .K_DS_selector=K_ds_ss_idx<<3,
        .TSS_selector=tss_selector
    };
    wrmsr_func(msr::syscall::IA32_FS_BASE,(uint64_t)&fs_slot);
    vaddr_t old_gs=rdmsr(msr::syscall::IA32_GS_BASE);
    ksystemramcpy((uint64_t*)old_gs,gs_slot,sizeof(uint64_t)*GS_SLOT_MAX_ENTRY_COUNT);
    runtime_processor_regist(&resources);
    fs_slot[STACK_PROTECTOR_CANARY_IDX]=0xDEADBEEF;
    
    gs_slot[PROCESSOR_SELF_RESOURCES_COMPELX_GS_INDEX]=(uint64_t)this;
    gs_slot[PROCESSOR_ID_GS_INDEX]=static_cast<uint64_t>(this->processor_id);
    wrmsr_func(msr::syscall::IA32_GS_BASE,(uint64_t)&gs_slot);
    wrmsr_func(msr::syscall::IA32_KERNEL_GS_BASE,(uint64_t)&gs_slot);
    tss.rsp0=stack_alloc(&kurd,7);
    tss.ist[0]=0;
    tss.ist[1]=stack_alloc(&kurd,2);
    tss.ist[2]=stack_alloc(&kurd,3);
    tss.ist[3]=stack_alloc(&kurd,3);
    tss.ist[4]=stack_alloc(&kurd,3);
    tss.ist[5]=stack_alloc(&kurd,3);
    if (!tss.rsp0 || !tss.ist[1] || !tss.ist[2] || !tss.ist[3]|| !tss.ist[4]||kurd.result!=result_code::SUCCESS) {
        panic_info_inshort inshort={
            .is_bug=true,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false        
        };
        Panic::panic(default_panic_behaviors_flags,
            "[x64_local_processor]stack remap failed\n",
        nullptr,&inshort,kurd);
    }
    if(is_x2apic_supported()){
        uint64_t ia32_apic_base=rdmsr(msr::apic::IA32_APIC_BASE);
        ia32_apic_base|=(1<<11);
        ia32_apic_base|=(1<<10);
        uint64_t tpr = 1;
        asm volatile("mov %0, %%cr8" : : "r"(tpr) : "memory");
        wrmsr_func(msr::apic::IA32_APIC_BASE,ia32_apic_base);
        wrmsr_func(msr::apic::IA32_X2APIC_SVR,0x1ff);
    }else{
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false        
            };
        Panic::panic(default_panic_behaviors_flags,
            "[x64_local_processor]x2apic not supported",nullptr,&inshort,KURD_t());
    }
}

KURD_t x64_local_processor::default_kurd()
{
    return KURD_t(0,0,module_code::INTERRUPT,INTERRUPT_SUB_MODULES_LOCATIONS::LOCATION_CODE_PROCESSORS,0,0,err_domain::ARCH);
}
KURD_t x64_local_processor::default_success()
{
    KURD_t result=default_kurd();
    result.level=level_code::INFO;
    result.result=result_code::SUCCESS;
    return result;
}
KURD_t x64_local_processor::default_fail()
{
    KURD_t result=default_kurd();
    result=set_result_fail_and_error_level(result);
    return result;
}
KURD_t x64_local_processor::default_fatal()
{
    KURD_t result=default_kurd();
    result=set_fatal_result_level(result);
    return result;
}
KURD_t x86_smp_processors_container::default_kurd()
{
    return KURD_t(0,0,module_code::INTERRUPT,INTERRUPT_SUB_MODULES_LOCATIONS::LOCATION_CODE_PROCESSORS,0,0,err_domain::ARCH);
}
KURD_t x86_smp_processors_container::default_success()
{
    KURD_t result=default_kurd();
    result.level=level_code::INFO;
    result.result=result_code::SUCCESS;
    return result;
}
KURD_t x86_smp_processors_container::default_fail()
{
    KURD_t result=default_kurd();
    result=set_result_fail_and_error_level(result);
    return result;
}
KURD_t x86_smp_processors_container::default_fatal()
{
    KURD_t result=default_kurd();
    result=set_fatal_result_level(result);
    return result;
}
void x64_local_processor::unsafe_handler_register_without_vecnum_chech(uint8_t vector, void *handler)
{
    idt[vector].offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(handler) & 0xFFFF);
    idt[vector].offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(handler) >> 16) & 0xFFFF);
    idt[vector].offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(handler) >> 32) & 0xFFFFFFFF);
    idt[vector].present = 1;
    idt[vector].reserved3 = 0;
    idt[vector].reserved2 = 0;
    idt[vector].reserved1 = 0;
}

void x64_local_processor::unsafe_handler_unregister_without_vecnum_chech(uint8_t vector)
{
    x64_local_processor::unsafe_handler_register_without_vecnum_chech(vector,template_idt[vector].handler);
}
bool x64_local_processor::handler_unregister(uint8_t vector){
        if(vector < 32 || vector >= ivec::BOTTOM_FOR_SYSTEM_RESERVED_VECS){
            return false;
        }
        unsafe_handler_unregister_without_vecnum_chech(vector);
        return true;
    }
bool x64_local_processor::handler_register(uint8_t vector,void*handler){
    if(vector < 32 || vector >= ivec::BOTTOM_FOR_SYSTEM_RESERVED_VECS){
        return false;
    }
    unsafe_handler_register_without_vecnum_chech(vector,handler);
    return true;
}

void x64_local_processor::GS_slot_write(uint32_t idx, uint64_t content)
{
    if(idx == 0 || idx >= GS_SLOT_MAX_ENTRY_COUNT){
        return;
    }
    gs_slot[idx]=content;
}

uint64_t x64_local_processor::GS_slot_get(uint32_t idx)
{
    if(idx == 0 || idx >= GS_SLOT_MAX_ENTRY_COUNT){
        return 0;
    }
    return gs_slot[idx];
}

uint32_t x64_local_processor::get_apic_id()
{
    return apic_id;
}
uint32_t x64_local_processor::get_processor_id()
{
    return processor_id;
}
uint64_t x64_local_processor::get_tss_rsp0()
{
    return tss.rsp0;
}
extern "C" uint64_t get_current_processor_rsp0()
{
    x64_local_processor* curr = (x64_local_processor*)read_gs_u64(PROCESSOR_SELF_RESOURCES_COMPELX_GS_INDEX);
    if (!curr) {
        return 0;
    }
    return curr->get_tss_rsp0();
}
x64_local_processor *x86_smp_processors_container::get_processor_mgr_by_processor_id(prcessor_id_t id)
{
    // 遍历local_processor_interrupt_mgr_array寻找对应的x64_local_processor*
    for (uint32_t i = 0; i < max_processor_count; i++) {
        x64_local_processor* processor = local_processor_interrupt_mgr_array[i];
        if (processor != nullptr && processor->get_processor_id() == id) {
            return processor;
        }
    }
    return nullptr;
}

x64_local_processor* x86_smp_processors_container::get_processor_mgr_by_apic_id(x2apicid_t apic_id)
{
    // 遍历local_processor_interrupt_mgr_array寻找对应的x64_local_processor*
    for (uint32_t i = 0; i < max_processor_count; i++) {
        x64_local_processor* processor = local_processor_interrupt_mgr_array[i];
        if (processor != nullptr && processor->get_apic_id() == apic_id) {
            return processor;
        }
    }
    return nullptr;
}
