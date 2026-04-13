#pragma once
#include "stdint.h"
#include "firmware/ACPI_APIC.h"
#include "memory/AddresSpace.h"
#include "DMAR.h"
namespace COREHARDWARES_LOCATIONS{
    constexpr uint8_t LOCATION_CODE_IO_APIC=0x03;
    namespace IO_APIC_DRIVERS_EVENTS {
        constexpr uint8_t INIT=0;
        constexpr uint8_t RTE_REGIST=1;
        namespace RTE_REGIST_RESULTS{
            namespace FAIL_RESULTS{ 
                constexpr uint16_t FAIL_BAD_PARAM=0x01;
            }
        }
        constexpr uint8_t RTE_UNREGIST=2;
        namespace RTE_UNREGIST_RESULTS{
            namespace FAIL_RESULTS{ 
                constexpr uint16_t FAIL_BAD_PARAM=0x01;
            }
        }
    }
};
class ioapic_driver {
    uint8_t ioapic_id;
    uint8_t max_rte_num;
    vm_interval ioapic_regs_interval;
    KURD_t default_kurd=KURD_t(0,0,module_code::DEVICES_CORE,COREHARDWARES_LOCATIONS::LOCATION_CODE_IO_APIC,0,0,err_domain::ARCH);
    KURD_t default_fail=set_result_fail_and_error_level(default_kurd);
    KURD_t default_fatal=set_fatal_result_level(default_kurd);
    KURD_t default_success=KURD_t(result_code::SUCCESS,0,module_code::DEVICES_CORE,COREHARDWARES_LOCATIONS::LOCATION_CODE_IO_APIC,0,level_code::INFO,err_domain::ARCH);
    struct regs_head{
        uint32_t select_reg;
        uint32_t reserved[3];
        uint32_t window_reg;
    };
    regs_head* head;
    union RTE_remmap_union {
        uint64_t value;
        struct {
            uint64_t vector:8;
            uint64_t delivery_mode:3;
            uint64_t index_15:1;
            uint64_t delivery_status:1;
            uint64_t pin_polarity:1;
            uint64_t remote_irr:1;
            uint64_t trigger_mode:1;
            uint64_t mask:1;
            uint64_t reserved:31;
            uint64_t interrupt_format:1;
            uint64_t remmap_idx_0_14:15;
        }filed;
        static_assert(sizeof(filed)==8,"RTE_remmap_union size error");
    };
    dmar::driver* belonged_dmar;
    uint64_t get_rte_raw(uint8_t rte);
    void set_rte_raw(uint8_t irq,uint64_t value);
public:
    ioapic_driver(APICtb_analyzed_structures::io_apic_structure* entry);
    KURD_t irq_regist(uint8_t rte,uint16_t remmap_idx,bool polarity);
    KURD_t irq_unregist(uint8_t rte);
};
extern ioapic_driver *main_router;
extern spinlock_cpp_t interrupt_manage_lock;//中断信息通路相关数据结构进行修改时的锁，由于频率低下故用全局大锁