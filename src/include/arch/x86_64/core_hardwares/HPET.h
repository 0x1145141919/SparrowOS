#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "abi/os_error_definitions.h"
#include "abi/boot.h"
namespace COREHARDWARES_LOCATIONS{
    constexpr uint8_t LOCATION_CODE_HPET=0x01;
    namespace HPET_READONLY_DRIVERS_EVENTS {
        constexpr uint8_t INIT=0;
        namespace INIT_RESULTS {
            namespace FAIL_REASONS{
                constexpr uint16_t INVALID_ACPI_ADDR = 0x01;
                constexpr uint16_t ACPI_ADDR_NOT_ALIGN = 0x02;
                constexpr uint16_t ALLREADE_INIT = 0x03;
            }
        }
    }
};
namespace HPET { 
    namespace ACPItb {
      struct ACPI_Table_Header {
        char        Signature[4];        // 4字节 @偏移0: 表标识符的ASCII字符串表示
        uint32_t Length;            // 4字节 @偏移4: 整个表的长度（包含表头）
        uint8_t  Revision;          // 1字节 @偏移8: 对应签名字段的结构版本号
        uint8_t  Checksum;          // 1字节 @偏移9: 整个表的校验和（包含本字段，总和必须为0）
        char        OEMID[6];            // 6字节 @偏移10: OEM提供的厂商标识字符串
        char        OEM_Table_ID[8];     // 8字节 @偏移16: OEM提供的特定数据表标识字符串
        uint32_t OEM_Revision;      // 4字节 @偏移24: OEM提供的版本号
        char        Creator_ID[4];       // 4字节 @偏移28: 创建此表的工具厂商ID
        uint32_t Creator_Revision;  // 4字节 @偏移32: 创建此表的工具版本号
    } __attribute__((packed));  
    struct HPET_Table {
        struct ACPI_Table_Header Header;
        uint32_t  Hardware_Rev_ID;         // 硬件修订ID
        uint32_t  reserved;
        phyaddr_t Base_Address;            // HPET寄存器的物理基地址
        uint32_t reserved2;
    }__attribute__((packed));
}
    namespace regs {
        constexpr uint32_t offset_General_Capabilities_and_ID = 0x000;
        constexpr uint32_t offset_General_Config = 0x010;
        constexpr uint32_t genral_status_offset = 0x020;
        constexpr uint32_t offset_main_counter_value = 0x0F0;
        constexpr uint32_t offset_Timer0_Configuration_and_Capabilities = 0x100;
        constexpr uint32_t size_per_timer = 0x20;
        constexpr uint8_t COMPARATOR_COUNT_LEFT_OFFSET = 8;
        constexpr uint8_t COMPARATOR_COUNT_MASK = 0x1F;
        constexpr uint8_t COUNTER_CLK_PERIOD_LEFT_OFFSET = 32;
        constexpr uint32_t COUNTER_CLK_PERIOD_FS_MASK = 0xFFFFFFFF;
        constexpr uint64_t GCONFIG_ENABLE_BIT = 1 << 0;
        constexpr uint64_t Tn_INT_ENB_CNF_BIT = 1 << 2;
    }
    namespace error_code {
        constexpr uint32_t NO_ERROR = 0;
        constexpr uint32_t ERROR_INVALID_ACPI_ADDR = 1;
        constexpr uint32_t ERROR_ACPI_ADDR_NOT_ALIGN = 2;
        constexpr uint32_t ERROR_INVALID_SIZE = 3;
        constexpr uint32_t ERROR_INVALID_HANDLE = 4;
        constexpr uint32_t ERROR_INVALID_OPERATION = 5;
        constexpr uint32_t ERROR_INVALID_STATE = 6;
    }
}
/**
 * 此类在彻底gKernelSpace->unsafe_load_pml4_to_cr3后马上初始化上线，最为系统的时钟源
 * 理论上可以让其下线，但是设计上没可能，不过从对称性上还是实现折构函数
 */
/**
 * 通过phy_reg_base等三个全0标记未初始化，防止second_stage_init被误调用
 */
class HPET_driver_only_read_time_stamp {
    phyaddr_t phy_reg_base;
    vaddr_t virt_reg_base;
    uint32_t hpet_timer_period_fs;
    uint8_t comparator_count;
    KURD_t default_kurd();
    KURD_t default_success();
    public:
    HPET_driver_only_read_time_stamp(loaded_VM_interval* entry);
    ~HPET_driver_only_read_time_stamp();
    uint64_t get_time_stamp_in_mius();
};
extern HPET_driver_only_read_time_stamp*readonly_timer;