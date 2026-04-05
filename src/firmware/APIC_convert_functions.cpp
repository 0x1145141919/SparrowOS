#include "firmware/ACPI_APIC.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/OS_utils.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
namespace APICtb_analyzed_structures {
    processor_x64_lapic_struct xapic(ACPI_MADT::Local_APIC_entry* entry) {
        processor_x64_lapic_struct result;
        result.apicid = entry->APIC_id;
        result.true_is_x2apic_false_is_xapic_bit = 0; // 表示这是xAPIC而不是x2APIC
        uint32_t self_apicid=query_x2apicid();
        uint64_t apic_base=rdmsr(msr::apic::IA32_APIC_BASE);
        if((self_apicid==entry->APIC_id)&&(apic_base&(1<<8)))result.is_bsp = 1;
        result.is_bsp = 0; // 如要求，默认设置为0
        return result;
    }

    processor_x64_lapic_struct xapic(ACPI_MADT::processor_local_x2apic_structure* entry) {
        processor_x64_lapic_struct result;
        result.apicid = entry->x2apic_id;
        result.true_is_x2apic_false_is_xapic_bit = 1; // 表示这是x2APIC
        uint32_t self_apicid=query_x2apicid();
        uint64_t apic_base=rdmsr(msr::apic::IA32_APIC_BASE);
        if((self_apicid==entry->x2apic_id)&&(apic_base&(1<<8)))result.is_bsp = 1;
        result.is_bsp = 0; // 如要求，默认设置为0
        return result;
    }

    io_apic_structure ioapic(ACPI_MADT::io_apic_structure* entry) {
        io_apic_structure result;
        result.regbase_addr = entry->io_apic_address; // 转换为64位地址
        result.gsi_base = entry->global_system_interrupt_base;
        result.ioapic_id = entry->io_apic_id;
        return result;
    }

    ISA_interrupt_ovveride_struct isa_interrupt_ovveride(ACPI_MADT::interrupt_source_override* entry) {
        ISA_interrupt_ovveride_struct result;
        result.source = entry->source;
        result.gsi = entry->global_system_interrupt;
        
        // 解析标志位
        uint16_t flags = entry->flags;
        result.flags.interrupt_polarity = static_cast<interrupt_polarity_type_t>(flags & 0x3);
        result.flags.interrupt_trigger_mode = static_cast<interrupt_trigger_mode_type_t>((flags >> 2) & 0x3);
        
        return result;
    }

    nmi_sourece_struct nmi_sourece(ACPI_MADT::nmi_source_structure* entry) {
        nmi_sourece_struct result;
        result.gsi = entry->global_system_interrupt;
        
        // 解析标志位
        uint16_t flags = entry->flags;
        result.flags.interrupt_polarity = static_cast<interrupt_polarity_type_t>(flags & 0x3);
        result.flags.interrupt_trigger_mode = static_cast<interrupt_trigger_mode_type_t>((flags >> 2) & 0x3);
        
        return result;
    }

    all_processor_nmi_struct all_processor_nmi(ACPI_MADT::local_apic_nmi_structure* entry) {
        all_processor_nmi_struct result;
        result.lint_number = entry->local_apic_lint;
        
        // 解析标志位
        uint16_t flags = entry->flags;
        result.flags.interrupt_polarity = static_cast<interrupt_polarity_type_t>(flags & 0x3);
        result.flags.interrupt_trigger_mode = static_cast<interrupt_trigger_mode_type_t>((flags >> 2) & 0x3);
        
        return result;
    }

    all_processor_nmi_struct all_processor_nmi(ACPI_MADT::local_x2apic_nmi_structure* entry) {
        all_processor_nmi_struct result;
        result.lint_number = entry->local_x2apic_lint;
        
        // 解析标志位
        uint16_t flags = entry->flags;
        result.flags.interrupt_polarity = static_cast<interrupt_polarity_type_t>(flags & 0x3);
        result.flags.interrupt_trigger_mode = static_cast<interrupt_trigger_mode_type_t>((flags >> 2) & 0x3);
        
        return result;
    }
}