#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "gSTResloveAPIs.h"
#include "util/Ktemplats.h"
namespace ACPI_MADT {
    typedef enum :uint8_t {
          LocalAPIC = 0,
          IOAPIC = 1,
          ISOverride = 2,
          NMI_Source = 3,
          LAPIC_NMI = 4,
          x2LocalAPIC = 0x9,
          Lx2APIC_NMI = 0xA
      }MADT_entrytype;
      struct Local_APIC_entry
      {
        MADT_entrytype type;
        uint8_t length;
        uint8_t ACPI_id;
        uint8_t APIC_id;
        uint32_t flags;
      }__attribute__((packed));
      struct io_apic_structure {
    MADT_entrytype  type;                    // Byte 0: 1 I/O APIC structure
    uint8_t  length;                  // Byte 1: 12
    uint8_t  io_apic_id;              // Byte 2: The I/O APIC's ID
    uint8_t  reserved;                // Byte 3: 0
    uint32_t io_apic_address;         // Byte 4-7: The 32-bit physical address
    uint32_t global_system_interrupt_base; // Byte 8-11: The global system interrupt base
} __attribute__((packed));
struct interrupt_source_override {
    MADT_entrytype  type;                    // Byte 0: 2 (Interrupt Source Override)
    uint8_t  length;                  // Byte 1: 10
    uint8_t  bus;                     // Byte 2: 0 (Constant, meaning ISA)
    uint8_t  source;                  // Byte 3: Bus-relative interrupt source (IRQ)
    uint32_t global_system_interrupt; // Byte 4-7: The Global System Interrupt
    uint16_t flags;                   // Byte 8-9: MPS INTI flags
} __attribute__((packed));
struct nmi_source_structure {
    MADT_entrytype  type;                    // Byte 0: 3 (NMI Source)
    uint8_t  length;                  // Byte 1: 8
    uint16_t flags;                   // Byte 2-3: Same as MPS INTI flags
    uint32_t global_system_interrupt; // Byte 4-7: The Global System Interrupt
} __attribute__((packed));
struct local_apic_nmi_structure {
    MADT_entrytype  type;                 // Byte 0: 4 (Local APIC NMI Structure)
    uint8_t  length;               // Byte 1: 6
    uint8_t  acpi_processor_uid;  // Byte 2: ACPI Processor UID
    uint16_t flags;                // Byte 3-4: MPS INTI flags
    uint8_t  local_apic_lint;      // Byte 5: Local APIC LINT#
} __attribute__((packed));
struct processor_local_x2apic_structure {
    MADT_entrytype  type;                 // Byte 0: 9 (Processor Local x2APIC structure)
    uint8_t  length;               // Byte 1: 16
    uint16_t reserved;             // Byte 2-3: Reserved - Must be zero
    uint32_t x2apic_id;            // Byte 4-7: The processor's local x2APIC ID
    uint32_t flags;                // Byte 8-11: Same as Local APIC flags
    uint32_t acpi_processor_uid;   // Byte 12-15: ACPI Processor UID
} __attribute__((packed));
struct local_x2apic_nmi_structure {
    MADT_entrytype  type;                    // Byte 0: 0x0A (Local x2APIC NMI Structure)
    uint8_t  length;                  // Byte 1: 12
    uint16_t flags;                   // Byte 2-3: Same as MPS INTI flags
    uint32_t acpi_processor_uid;      // Byte 4-7: ACPI Processor UID
    uint8_t  local_x2apic_lint;       // Byte 8: Local x2APIC LINT#
    uint8_t  reserved[3];             // Byte 9-11: Reserved - Must be zero
} __attribute__((packed));
};
namespace APICtb_analyzed_structures{
    enum interrupt_polarity_type_t:uint8_t{
        Specify_by_bus_polarity=0,
        Active_high=1,
        Reserved_polarity=2,
        Active_low=3
    };
    enum interrupt_trigger_mode_type_t:uint8_t{
        Specify_by_bus_trigger_mode=0,
        edge_trigger=1,
        Reserved_trigger_mode=2,
        level_trigger=3
    };
    struct interrupt_sources_flags{
        interrupt_polarity_type_t interrupt_polarity;
        interrupt_trigger_mode_type_t interrupt_trigger_mode;
    };
    struct processor_x64_lapic_struct{
        uint32_t apicid;
        uint32_t true_is_x2apic_false_is_xapic_bit:1;
        uint32_t is_bsp:1;
    };
    struct all_processor_nmi_struct{
        uint8_t lint_number;
        interrupt_sources_flags flags;
    };
    struct io_apic_structure{
        uint64_t regbase_addr;
        uint32_t gsi_base;
        uint8_t ioapic_id;        
    };
    struct ISA_interrupt_ovveride_struct{
        uint8_t source;//对应的irq
        uint32_t gsi;
        interrupt_sources_flags flags;
    };
    struct nmi_sourece_struct{
        uint32_t gsi;
        interrupt_sources_flags flags;  
    };

    processor_x64_lapic_struct xapic(ACPI_MADT::Local_APIC_entry* entry);
    processor_x64_lapic_struct xapic(ACPI_MADT::processor_local_x2apic_structure* entry);
    io_apic_structure ioapic(ACPI_MADT::io_apic_structure* entry);
    ISA_interrupt_ovveride_struct isa_interrupt_ovveride(ACPI_MADT::interrupt_source_override* entry);
    nmi_sourece_struct nmi_sourece(ACPI_MADT::nmi_source_structure* entry);
    all_processor_nmi_struct all_processor_nmi(ACPI_MADT::local_apic_nmi_structure* entry);
    all_processor_nmi_struct all_processor_nmi(ACPI_MADT::local_x2apic_nmi_structure* entry);
}

class APIC_table_analyzer {
    public:
    Ktemplats::list_doubly<APICtb_analyzed_structures::processor_x64_lapic_struct>*processor_x64_list;
    Ktemplats::list_doubly<APICtb_analyzed_structures::io_apic_structure>*io_apic_list;
    Ktemplats::list_doubly<APICtb_analyzed_structures::ISA_interrupt_ovveride_struct>*isa_interrupt_ovveride_list;
    Ktemplats::list_doubly<APICtb_analyzed_structures::nmi_sourece_struct>*nmi_sourece_list;
    Ktemplats::list_doubly<APICtb_analyzed_structures::all_processor_nmi_struct>*all_processor_nmi_list;
    APIC_table_analyzer(MADT_Table* madt);
    ~APIC_table_analyzer();
};
extern APIC_table_analyzer*gAnalyzer;
