#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "arch/x86_64/Interrupt_system/fixed_interrupt_vectors.h"
namespace LAPIC_PARAMS_ENUM{
    enum TIMER_MODE_T:uint8_t
    {
        ONE_SHOT = 0,
        PERIODIC = 1,
        TSC_DDLINE = 2
    };
    enum DELIVERY_MODE_T:uint8_t
    {
        FIXED = 0,
        LOWEST_PRIORITY = 1,
        SMI = 2,
        NMI = 4,
        INIT = 5,
        EXTINT = 7
    };
    enum PIN_POLARITY_T:uint8_t
    {
        ACTIVE_HIGH = 0,
        ACTIVE_LOW = 1
    };
    enum TRIGGER_MODE_T:uint8_t
    {
        EDGE = 0,
        LEVEL = 1
    };
    enum DELIVERY_STATUS_T:uint8_t
    {
        IDLE = 0,
        SEND_PENDING = 1
    };  
    enum MASK_T:uint8_t
    {
        NO = 0,
        YES = 1
    };
    enum DESTINATION_T:uint8_t
    {
        PHYSICAL = 0,
        LOGICAL = 1
    };
    enum DESTINATION_SHORTHAND_T:uint8_t
    {
        NO_SHORTHAND = 0,
        SELF = 1,
        ALL_INCLUDING_SELF = 2,
        ALL_EXCLUDING_SELF = 3
    };
    enum INTERUPT_LEVEL:uint8_t
    {
        DE_ASSERT = 0,
        ASSERT = 1
    };
    
}
namespace x2apic
{
    namespace ESR_BITS {
    // 错误标志位 (位 0-7)
    constexpr uint8_t SEND_CHECKSUM_ERROR    = 0;  // 发送校验和错误
    constexpr uint8_t RECEIVE_CHECKSUM_ERROR  = 1;  // 接收校验和错误
    constexpr uint8_t SEND_ACCEPT_ERROR       = 2;  // 发送接受错误
    constexpr uint8_t RECEIVE_ACCEPT_ERROR    = 3;  // 接收接受错误
    constexpr uint8_t REDIRECTABLE_IPI        = 4;  // 可重定向IPI
    constexpr uint8_t SEND_ILLEGAL_VECTOR     = 5;  // 发送非法向量
    constexpr uint8_t RECEIVE_ILLEGAL_VECTOR  = 6;  // 接收非法向量
    constexpr uint8_t ILLEGAL_REGISTER_ADDR   = 7;  // 非法寄存器地址
    
    // 位 8-31 为保留位
    constexpr uint8_t RESERVED_START = 8;
    constexpr uint8_t RESERVED_END = 31;
}

// ESR 位掩码常量
namespace ESR_MASKS {
    constexpr uint32_t SEND_CHECKSUM_ERROR    = 1 << ESR_BITS::SEND_CHECKSUM_ERROR;
    constexpr uint32_t RECEIVE_CHECKSUM_ERROR = 1 << ESR_BITS::RECEIVE_CHECKSUM_ERROR;
    constexpr uint32_t SEND_ACCEPT_ERROR      = 1 << ESR_BITS::SEND_ACCEPT_ERROR;
    constexpr uint32_t RECEIVE_ACCEPT_ERROR   = 1 << ESR_BITS::RECEIVE_ACCEPT_ERROR;
    constexpr uint32_t REDIRECTABLE_IPI       = 1 << ESR_BITS::REDIRECTABLE_IPI;
    constexpr uint32_t SEND_ILLEGAL_VECTOR    = 1 << ESR_BITS::SEND_ILLEGAL_VECTOR;
    constexpr uint32_t RECEIVE_ILLEGAL_VECTOR = 1 << ESR_BITS::RECEIVE_ILLEGAL_VECTOR;
    constexpr uint32_t ILLEGAL_REGISTER_ADDR  = 1 << ESR_BITS::ILLEGAL_REGISTER_ADDR;
    
    // 保留位掩码
    constexpr uint32_t RESERVED_MASK = 0xFFFFFF00;  // 位 8-31
    constexpr uint32_t ERROR_FLAGS_MASK = 0xFF;      // 位 0-7
    
    // 错误标志组合掩码
    constexpr uint32_t CHECKSUM_ERRORS = SEND_CHECKSUM_ERROR | RECEIVE_CHECKSUM_ERROR;
    constexpr uint32_t ACCEPT_ERRORS = SEND_ACCEPT_ERROR | RECEIVE_ACCEPT_ERROR;
    constexpr uint32_t ILLEGAL_VECTOR_ERRORS = SEND_ILLEGAL_VECTOR | RECEIVE_ILLEGAL_VECTOR;
    constexpr uint32_t ALL_ERROR_FLAGS = 0xFF;
}

    union timer_lvt_entry
    {
    struct params
    {
    uint8_t vector;
    uint8_t reserved1:4;
    uint8_t deliver_status:1;
    uint8_t reserved2:3;
    uint8_t masked:1;
    uint8_t timermode:2;
    uint8_t reserved3:5;
    uint8_t reserved4;
    uint32_t reserved5;
    };
    params param;
    uint64_t raw;
    }__attribute__((packed));
constexpr timer_lvt_entry ddline_timer{
    .param{
        .vector =ivec::LAPIC_TIMER,// 此处暂不写具体动作
        .reserved1 = 0,
        .deliver_status = LAPIC_PARAMS_ENUM::DELIVERY_STATUS_T::IDLE,
        .reserved2 = 0,
        .masked = LAPIC_PARAMS_ENUM::MASK_T::NO,
        .timermode = LAPIC_PARAMS_ENUM::TIMER_MODE_T::TSC_DDLINE,        
        .reserved3 = 0,
        .reserved4 = 0,
        .reserved5 = 0
    }
};
union devide_reg_t
{
    uint64_t raw; 
    struct params
    {
        uint8_t param_low2bit:2;
        uint8_t reserved1:1;
        uint8_t param_high1bit:1;
        uint8_t reserved2:4;
        uint64_t reserved3:56;
    };
    params param;
};
typedef uint32_t initcout_reg_t;
typedef uint32_t current_reg_t;
union x2apic_destination_t {
        uint32_t raw;
        uint32_t id;
        struct logical_params
        {
            uint16_t cluster_bitmap;
            uint16_t cluster_idx;
        };
        logical_params l_dest;
    };
union x2apic_icr_t { 
    uint64_t raw;
    struct params
    { 
        uint8_t vector;
        uint8_t delivery_mode:3;
        uint8_t destination_mode:1;
        uint8_t reserved1:2;
        uint8_t level:1;
        uint8_t trigger_mode:1;
        uint8_t reserved2:2;
        uint8_t destination_shorthand:2;
        uint16_t reserved3:12;
        x2apic_destination_t destination;
    };
    params param;
};
constexpr x2apic_icr_t broadcast_exself_icr{
    .param{
        .vector = ivec::IPI,
        .delivery_mode = LAPIC_PARAMS_ENUM::DELIVERY_MODE_T::FIXED,
        .destination_mode = LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
        .reserved1 = 0,
        .level = 0,
        .trigger_mode=0,
        .reserved2 = 0,
        .destination_shorthand=LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::ALL_EXCLUDING_SELF,
        .reserved3 = 0,
        .destination = {.raw = 0xffffffff},
    }
};
    // ==================== LVT error 寄存器 ====================
union lvt_error_entry {
    struct params {
        uint8_t vector;          // 位 7:0   - 中断向量
        uint8_t reserved0 :4;   // 位 11:8    - 保留
        uint8_t delivery_status : 1; // 位 12  - 投递状态
        uint8_t reserved1 : 3;    // 位 15:13 - 保留
        uint8_t masked : 1;       // 位 16    - 掩码位
        uint8_t reserved2 : 7;    // 位 23:17 - 保留
        uint8_t reserved3;        // 位 31:24 - 保留
    } __attribute__((packed));
    params param;
    uint32_t raw;
};
static_assert(sizeof(lvt_error_entry) == 4, "LVT CMCI must be 4 bytes");

// ==================== LVT LINT0/LINT1 寄存器 ====================
union lvt_lint_entry {
    struct params {
        uint8_t vector;          // 位 7:0   - 中断向量
        uint8_t delivery_mode : 3; // 位 10:8  - 投递模式
        uint8_t reserved0 : 1;   // 位 11    - 保留
        uint8_t delivery_status : 1; // 位 12  - 投递状态
        uint8_t pin_polarity : 1; // 位 13    - 管脚极性 (0: Active High, 1: Active Low)
        uint8_t remote_irr : 1;   // 位 14    - Remote IRR
        uint8_t trigger_mode : 1;  // 位 15   - 触发模式 (0: Edge, 1: Level)
        uint8_t masked : 1;       // 位 16    - 掩码位
        uint8_t reserved1 : 7;    // 位 23:17 - 保留
        uint8_t reserved2;        // 位 31:24 - 保留
    } __attribute__((packed));
    params param;
    uint32_t raw;
};
static_assert(sizeof(lvt_lint_entry) == 4, "LVT LINT must be 4 bytes");

// ==================== LVT CMCI/Performance/Thermal 寄存器 ====================
union lvt_general_entry {
    struct params {
        uint8_t vector;          // 位 7:0   - 中断向量
        uint8_t delivery_mode : 3; // 位 10:8  - 投递模式
        uint8_t reserved0 : 1;   // 位 11    - 保留
        uint8_t delivery_status : 1; // 位 12  - 投递状态
        uint8_t reserved1 : 3;    // 位 16:13 - 保留 (包括13-15位)
        uint8_t masked : 1;       // 位 16    - 掩码位
        uint8_t reserved2 : 7;    // 位 23:17 - 保留
        uint8_t reserved3;        // 位 31:24 - 保留
    } __attribute__((packed));
    params param;
    uint32_t raw;
};
static_assert(sizeof(lvt_general_entry) == 4, "LVT General must be 4 bytes");
    
    class x2apic_driver{
    private:
    public:
    static void raw_config_timer(timer_lvt_entry entry);
    static void raw_config_timer_init_count(initcout_reg_t count);
    static void raw_config_timer_divider(devide_reg_t reg);
    static current_reg_t get_timer_current_count();
    static void raw_send_ipi(x2apic_icr_t icr);
    static void broadcast_exself_fixed_ipi(void(*ipi_handler)());
    static void raw_error_lvt_config(lvt_error_entry entry);
    static void raw_lint0_lvt_config(lvt_lint_entry entry);
    static void raw_lint1_lvt_config(lvt_lint_entry entry);
    static void raw_perf_lvt_config(lvt_general_entry entry);
    static void raw_thermal_lvt_config(lvt_general_entry entry);
    static void raw_cmci_lvt_config(lvt_general_entry entry);
    static void write_eoi();
    /**
     * x2apic相关接口，操作时必须确认本核心
     * 使用头文件里写死的偏移量
     * 1.rdtsc
     * 2.raw_config_timer//通用模式
     * 2.1.raw_config_timer_lvt
     * 2.2.raw_config_timer_init_count
     * 2.3.raw_config_timer_divider
     * 2.4.get_timer_current_count()
     * 3.set_tsc//特化tsc-deadline模式
     * 3.1 private set_tsc_ddline(uint64_t time)
     * 3.2 set_tsc_vec(idtvector_t ,uint64_t ddline)
     * 3.3 cancel_tsc_ddline();
     * 4.set_priority
     * 5.ipi系列
     * 5.1 raw_send_ipi(..infopackage..)
     * 5.2 broadcast_exself_fixed_ipi(void(*)())
     * 6.自我ipi
     */
    };
    class lapic_timer_one_shot{
        public:
        static void processor_regist();
        static void set_clock_by_stamp(uint64_t stamp_mius);
        static void set_clock_by_offset(uint64_t offset_mius);
        static uint32_t get_current_clock();//获得硬件计时器的值，在这里就是current_count 
        static void cancel_clock();
        static bool is_alarm_valid();  //传递硬件级别的是否关闭判据
    };
    class lapic_timer_tsc_ddline{
        public:
        static void processor_regist();
        static void set_clock_by_stamp(uint64_t stamp_mius);
        static void set_clock_by_offset(uint64_t offset_mius);
        //static miusecond_time_stamp_t get_current_clock();,不需要，因为有rdtsc 
        static void cancel_clock();
        static bool is_alarm_valid(); 
    };
}
