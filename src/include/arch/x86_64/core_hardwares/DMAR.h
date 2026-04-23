//虚拟化相关模块最好实体机测试，理论上也可以嵌套虚拟化
//此文件目标解析DMAR表，主要放表的数据结构以及解析工具函数
#pragma once
#include "stdint.h"
#include "firmware/ACPI_APIC.h"
#include "memory/AddresSpace.h"

struct pcie_location{
    uint16_t segment_num;
    uint8_t bus_num;
    uint8_t device_num:5;
    uint8_t func_num:3;
};
namespace COREHARDWARES_LOCATIONS{
    constexpr uint8_t LOCATION_CODE_DMAR=0x02;
    namespace DMAR_DRIVERS_EVENTS {
        constexpr uint8_t INIT=0;
        namespace INIT_RESULTS {
            namespace FATAL_REASONS{
                constexpr uint16_t CAP_DIDNT_MET_LOWEST_DEMAND=0x01;
                constexpr uint16_t TIME_OUT_COMMAND_SET=0x02;
                constexpr uint16_t VECTOR_REGIST_FAIL=0x03;
            }
        }
        constexpr uint8_t ALLOC_ENTRY=0x01;
        namespace ALLOC_ENTRY_RESULTS { 
            namespace FAIL_REASONS{
                constexpr uint16_t FAIL_NO_AVALIABLE_ENTRY=0x01;
            }
        }
    }
};

namespace dmar{
    static constexpr uint32_t interrupt_remapp_table_default_size=1<<20;
    constexpr uint32_t interrupt_remapp_table_default_S=0xF;
    extern uint8_t iommu_fault_alloced_vector;//中断重映射只会在BSP上被分配，只会发向BSP
    namespace acpi{
    enum sub_table_type:uint16_t{
        DRHD=0,
        RMRR=1,
        ATSR=2,
        RHSA=3,
        ANDD=4,
        SATC=5,
        SIDP=6
    };
    enum scope_type:uint8_t{
        PCI_ENDPOINT=1,
        PCI_BRIDGE=2,
        IOAPIC=3,
        HPET=4
    };
    struct sub_table_head{
        uint16_t table_type;
        uint16_t length;
    }__attribute__((packed));
    struct DMAR_head{
        struct ACPI_Table_Header Header;
        uint8_t host_addr_width;
        uint8_t flag_interrupt_remap_enable:1;
        uint8_t flag_X2APIC_OPT_OUT:1;
        uint8_t flag_DMA_CTRL_PLATFORM_OPT_IN_FLAG:1;
        uint8_t flag_reserved:5;
        uint8_t reserved[10];
    }__attribute__((packed));
    struct device_scope{
        uint8_t type;
        uint8_t length;
        uint8_t flags;
        uint8_t reserved;
        uint8_t enumerator_type;//
        uint8_t start_bus_num;
    }__attribute__((packed));
    struct device_scope_simp{//为了简单暂时只支持这个极简scope
        uint8_t type;
        uint8_t length;//必须为8,极简scope
        uint8_t flags;
        uint8_t reserved;
        uint8_t enumerator_type;//
        uint8_t start_bus_num;
        uint8_t device_num;
        uint8_t func_num;
    }__attribute__((packed));
    struct DRHD_table{
        sub_table_head head;
        uint8_t flag_INCLUDE_PCI_ALL:1;
        uint8_t flag_reserved:7;
        uint8_t mmio_size_specify;//大小为(1<<(12+mmio_size_specify))
        uint16_t pcie_seg_number;
        uint64_t register_base_addr;
    };
};
    enum interrupt_mode_type_t{
        fixed=0,
        lowest_priority=1,
        SMI=2,
        NMI=4,
        INIT=5,
        EXTINT=6
    };
    enum source_validation_type_t{
        no_verify=0,
        precise_verify=1,
        interval_verify=2,
        reserved=3
    };
    namespace translation_structs{
        struct irte{
        uint32_t present:1;
        uint32_t fpd:1;
        uint32_t destination_mode:1;
        uint32_t redirection_hint:1;
        uint32_t trigger_mode:1;
        uint32_t delivery_mode:3;//interrupt_mode_type要用这个解析
        uint32_t reserved1:7;
        uint32_t IRTE_mode:1;
        uint32_t vec:8;
        uint32_t reserved2:8;
        uint32_t destination;
        uint64_t source_id:16;
        uint64_t source_qualifier:2;
        uint64_t source_validation_type:2;
        uint64_t reserved3:44;
    }__attribute__((packed));
        union legacy_root_entry{ //数据定义错误
            uint64_t unit[2];
            struct{
                uint64_t present:1;
                uint64_t reserved:63;
                uint64_t reserved1;
            }field;
        };
        union legacy_context_entry{//数据定义错误
            uint64_t unit[2];
            struct{
                uint64_t present:1;
                uint64_t FPD:1;
                uint64_t translation_type:2;
                uint64_t reserved:60;
                uint64_t addr_width:3;
                uint64_t reserved1:5;
                uint64_t domain_identifier:16;
                uint64_t reserved2:40;
            }field;
            static_assert(sizeof(field)==16,"legacy_context_entry size error");
        };
        static_assert(sizeof(irte)==16,"irte size error");
    };
    
    struct regist_remmap_struct_simp{
        pcie_location location;
        uint32_t target_processor_id;
        uint16_t vec;//后续增加
        dmar::interrupt_mode_type_t interrupt_mode;
    };
    struct regist_remmap_struct{//暂时只支持端点全匹配
        pcie_location location;
        uint16_t vec;//后续增加
        dmar::interrupt_mode_type_t delivery_mode;
        uint32_t destination;
        uint32_t destination_mode:1;
        uint64_t trigger_mode:1;
        uint64_t redirection_hint:1;
    };
    namespace regs_specify{
        namespace extended_caps_specify{
            constexpr uint64_t mask_queue_invalidation=1ull<<1;
            constexpr uint64_t mask_dtlb=1ull<<2;
            constexpr uint64_t mask_iremap=1ull<<3;
            constexpr uint64_t mask_iremap_x2apicsupport=1ull<<4;
            constexpr uint64_t mask_smallest_cap_set=mask_iremap_x2apicsupport|mask_dtlb|
            mask_queue_invalidation|mask_iremap;
        };
        union fault_recording_reg{
            uint64_t value[2];
            struct __attribute__((packed)){
            uint64_t reserved0:12;
            uint64_t fi:52;
            uint64_t SID:16;
            uint64_t reserved2:12;
            uint64_t T2:1;
            uint64_t PRIV:1;
            uint64_t EXE:1;
            uint64_t PP:1;
            uint64_t FR:8;
            uint64_t PV:20;
            uint64_t AT:2;
            uint64_t T1:1;
            uint64_t F:1;
            }fields;
            static_assert(sizeof(fields)==16,"fault_recording_reg size error");
        };
        namespace global_command_status_specify{
            constexpr uint32_t mask_compatibility_format_interrupt=1<<23;
            constexpr uint32_t mask__set_iremapp_tbptr=1<<24;
            constexpr uint32_t mask_interrupt_remap_enable=1<<25;
            constexpr uint32_t mask_queue_invalidatiion_enable=1<<26;
            constexpr uint32_t mask_write_buffer_flush=1<<27;
            constexpr uint32_t mask_set_root_table_ptr=1<<30;
            constexpr uint32_t mask_dma_remapping_enable=1<<31;
        };
        namespace imap_tbptr_reg_specify{
            constexpr uint32_t mask_iremap_x2apic_support=1<<11;
        };
    };
    
    class driver{
    
    vm_interval regs_interval;
    vm_interval interrupt_remmaptable_interval;
    KURD_t default_kurd=KURD_t(0,0,module_code::DEVICES_CORE,COREHARDWARES_LOCATIONS::LOCATION_CODE_DMAR,0,0,err_domain::ARCH);
    KURD_t default_success=[]()->KURD_t{
        KURD_t default_kurd;
        default_kurd.level=level_code::INFO;
        default_kurd.result=result_code::SUCCESS;
        return default_kurd;
    }();
    KURD_t default_err=set_result_fail_and_error_level(default_kurd);
    KURD_t default_fatal=set_fatal_result_level(default_kurd);
    uint16_t fault_record_regs_count=0;

    struct fault_record_reg_raw{
        uint64_t value[2];
    };
    fault_record_reg_raw*fault_regs_bases;
    union fault_record{
        fault_record_reg_raw raw;
        struct{
            uint64_t reserved0:12;
            uint64_t FI:52;
            uint64_t SID:16;
            uint64_t reserved2:12;
            uint64_t T2:1;
            uint64_t PRIV:1;
            uint64_t EXE:1;
            uint64_t PASID_P:1;
            uint64_t FAULT_REASON:8;
            uint64_t PASID_VALUE:20;
            uint64_t ADDR_TYPE:2;
            uint64_t T1:1;
            uint64_t F:1;
        }fields;
        static_assert(sizeof(fields)==16,"fault_record size error");
    };
    struct head_regs{
        uint32_t version;
        uint32_t reserved_1;
        uint64_t cap_regs;
        union cap_reg_union{
            uint64_t value;
            struct __attribute__((packed)){
                uint64_t ND:3;
                uint64_t reserved1:1;
                uint64_t RWBF:1;
                uint64_t PLMR:1;
                uint64_t PHMR:1;
                uint64_t CM:1;
                uint64_t SAGAW:5;
                uint64_t reserved2:3;
                uint64_t MGAW:6;
                uint64_t ZLR:1;
                uint64_t DEP:1;
                uint64_t FRO:10;
                uint64_t SSLPS:4;
                uint64_t reserved3:1;
                uint64_t PSI:1;
                uint64_t NFR:8;
                uint64_t MAMV:6;
                uint64_t DWD:1;
                uint64_t DRD:1;
                uint64_t FS1GP:1;
                uint64_t reserved4:2;
                uint64_t PI:1;
                uint64_t FS5LP:1;
                uint64_t ECMDS:1;
                uint64_t ESIRTPS:1;
                uint64_t ESRTPS:1;
            }fields;
            static_assert(sizeof(fields)==8,"cap_reg_union size error");
        };
        
        uint64_t extend_regs;
        uint32_t global_command;
        uint32_t global_status;
        uint64_t root_table;
        uint64_t context_command;
        uint32_t reserved_2;
        uint32_t fault_status;
        union fault_status_union
        {
            uint32_t value;
            struct __attribute__((packed)){
            uint32_t PFO:1;
            uint32_t PPF:1;
            uint32_t reserved:2;
            uint32_t IQE:1;
            uint32_t ICE:1;
            uint32_t ITE:1;
            uint32_t DEP:1;
            uint32_t FRI:8;
            uint32_t reserved2:16;
        }fields;
        static_assert(sizeof(fields)==4,"fault_status_union size error");
        };
        uint32_t fault_event_control;
        union fault_event_control_union
        {
            uint32_t value;
            struct __attribute__((packed)){
            uint32_t reserved:30;
            uint32_t IP:1;
            uint32_t IM:1;
        }fields;
        static_assert(sizeof(fields)==4,"fault_event_control_union size error");
        };
        uint32_t fault_event_data;
        uint32_t fault_event_addr;
        uint32_t fault_event_addr_high;
        uint32_t reserved_3[7];
        uint32_t protected_memory_enable;
        uint32_t protected_memory_low_base;
        uint32_t protected_memory_low_limit;
        uint64_t protected_memory_high_base;
        uint64_t protected_memory_high_limit;
        uint64_t invalidation_queue_head;
        uint64_t invalidation_queue_tail;
        uint64_t invalidation_queue_addr;
        uint32_t reserved_4;
        uint32_t invalidation_completion_status;
        uint32_t invalidation_completion_event_control;
        uint32_t invalidation_completion_event_data;
        uint32_t invalidation_completion_event_addr;
        uint32_t invalidation_completion_event_addr_high; 
        uint64_t invalidation_queue_record;
        uint64_t Interrupt_remapping_table_addr;
        uint64_t page_request_queue_head;
        uint64_t page_request_queue_tail;
        uint64_t page_request_queue_addr;
        uint32_t reserved; 
        uint32_t page_request_status;
        uint32_t page_request_event_control;
        uint32_t page_request_event_data;
        uint32_t page_request_event_addr;
        uint32_t page_request_event_addr_high;
    }__attribute__((packed));
    static_assert(sizeof(head_regs)==0xF0,"head_regs size error");
    head_regs* regs;
    acpi::DRHD_table* drhd;
    translation_structs::irte* interrupt_remmaptable;
    translation_structs::legacy_root_entry* legacy_root_entry;

    void set_command_enable_iremap();
    void set_command_disable_iremap();
    void set_command_disable_compatiable_format_interrupt();
    void set_command_set_iremap_tbptr();
    void command_disable_traslation();
    void command_enable_traslation();
    void command_enable_root_table();
    public:
    KURD_t device_regist(pcie_location location);//根据一个BDF进行相关上下文表的注册，现在统一初始化为PT,FDP简化设计
    driver(acpi::DRHD_table* drhd);//构造会强制开启对应的重映射硬件的下游设备的中断重映射，dma重映射,以及msi的错误报告中断
    // 提供公共访问方法以获取中断重映射表
    translation_structs::irte* get_interrupt_remmaptable() const { return interrupt_remmaptable; }
    KURD_t regist_interrupt_simp(regist_remmap_struct arg,uint16_t&idx);//支持持投递到一个CPU
    KURD_t disable_remappentry(uint16_t idx);
    KURD_t err_handle();//
};
    extern driver** dmar_table;
    extern uint32_t dmars_count;
    extern uint32_t main_dmar_id;
    struct special_location{
        pcie_location location;
        uint32_t dmar_id;
    };
    extern special_location* special_locations;
    extern uint32_t special_locations_count;
    extern uint32_t ioapic_idx;//只会支持一个ioapic,用于给legacy设备，其它完全走msi/msix
    extern uint32_t hpet_idx;
    int Init(acpi::DMAR_head*head);
    KURD_t regist_interrupt_simp(regist_remmap_struct arg,uint16_t&idx,uint32_t&dmar_id);
    KURD_t disable_remappentry(uint16_t idx,uint32_t dmar_id);
};


