#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "abi/os_error_definitions.h"
#include "abi/boot.h"

namespace COREHARDWARES_LOCATIONS {
    constexpr uint8_t LOCATION_CODE_HPET = 0x01;
    namespace HPET_DRIVER_EVENTS {
        constexpr uint8_t INIT = 0;
        namespace INIT_RESULTS {
            namespace FAIL_REASONS {
                constexpr uint16_t INVALID_ACPI_ADDR  = 0x01;
                constexpr uint16_t ACPI_ADDR_NOT_ALIGN = 0x02;
                constexpr uint16_t ALLREADE_INIT      = 0x03;
                constexpr uint16_t COUNTER_PERIOD_ZERO = 0x04;
            }
        }
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * ACPI HPET Table
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace HPET { namespace ACPItb {
    struct ACPI_Table_Header {
        char     Signature[4];
        uint32_t Length;
        uint8_t  Revision;
        uint8_t  Checksum;
        char     OEMID[6];
        char     OEM_Table_ID[8];
        uint32_t OEM_Revision;
        char     Creator_ID[4];
        uint32_t Creator_Revision;
    } __attribute__((packed));

    struct HPET_Table {
        ACPI_Table_Header Header;
        uint32_t  Hardware_Rev_ID;
        uint32_t  reserved;
        phyaddr_t Base_Address;   // HPET 寄存器物理基址
        uint32_t  reserved2;
    } __attribute__((packed));
}}

/* ═══════════════════════════════════════════════════════════════════════════
 * HPET MMIO 寄存器布局 (head_regs_t)
 *
 * 遵照 IA-PC HPET Spec Rev 1.0a §2.1.1 / §2.3.1，64-bit 对齐，
 * 总大小 1024 字节。
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace HPET { namespace reg_layout {

    // ── General Capabilities & ID Register (RO) §2.3.4 ────────────────
    union GCAP_ID {
        uint64_t raw;
        struct {
            uint64_t rev_id             : 8;  // [7:0]
            uint64_t num_tim_cap        : 5;  // [12:8]  定时器数 = num_tim_cap + 1
            uint64_t count_size_cap     : 1;  // [13]    0=32-bit 计数器, 1=64-bit
            uint64_t reserved           : 1;  // [15:14]
            uint64_t leg_rt_cap         : 1;  // [15]
            uint64_t vendor_id          : 16; // [31:16]
            uint64_t counter_clk_period : 32; // [63:32] femptoseconds (10^-15 s)
        };
    };
    static_assert(sizeof(GCAP_ID) == 8, "GCAP_ID size");

    // ── General Configuration Register (RW) §2.3.5 ────────────────────
    union GEN_CONFIG {
        uint64_t raw;
        struct {
            uint64_t enable_cnf         : 1;  // [0]     总体使能
            uint64_t leg_rt_cnf         : 1;  // [1]     LegacyReplacement 路由
            uint64_t reserved           : 14; // [15:2]
            uint64_t vendor_reserved    : 8;  // [23:16] 厂商保留，OS 不可修改
            uint64_t reserved2          : 40; // [63:24]
        };
    };
    static_assert(sizeof(GEN_CONFIG) == 8, "GEN_CONFIG size");

    // ── General Interrupt Status Register (R/WC) §2.3.6 ───────────────
    union GEN_INT_STS {
        uint64_t raw;
        struct {
            uint64_t t0_int_sts         : 1;
            uint64_t t1_int_sts         : 1;
            uint64_t t2_int_sts         : 1;
            uint64_t tn_int_sts         : 29;
            uint64_t reserved           : 32;
        };
    };
    static_assert(sizeof(GEN_INT_STS) == 8, "GEN_INT_STS size");

    // ── Timer N Configuration & Capability Register (RW) §2.3.8 ────────
    union TIMER_CONFIG_CAP {
        uint64_t raw;
        struct {
            uint64_t reserved0          : 1;  // [0]
            uint64_t tn_int_type_cnf    : 1;  // [1]     0=edge, 1=level
            uint64_t tn_int_enb_cnf     : 1;  // [2]     中断使能
            uint64_t tn_type_cnf        : 1;  // [3]     0=one-shot, 1=periodic
            uint64_t tn_per_int_cap     : 1;  // [4]     periodic capable
            uint64_t tn_size_cap        : 1;  // [5]     0=32-bit cmp, 1=64-bit cmp
            uint64_t tn_val_set_cnf     : 1;  // [6]     value set
            uint64_t reserved1          : 1;  // [7]
            uint64_t tn_32moe_cnf       : 1;  // [8]     32-bit mode enable
            uint64_t tn_route_cap       : 15; // [23:9]  IOxAPIC 路由能力位图
            uint64_t tn_fsb_int_dly_cnf : 1;  // [24]    FSB 延迟
            uint64_t tn_fsb_en_cnf      : 1;  // [25]    FSB 消息使能
            uint64_t reserved2          : 6;  // [31:26]
            uint64_t tn_int_route_cnf   : 5;  // [36:32] 中断路由选择
            uint64_t reserved3          : 27; // [63:37]
        };
    };
    static_assert(sizeof(TIMER_CONFIG_CAP) == 8, "TIMER_CONFIG_CAP size");

    // ── MMIO 寄存器块 ─────────────────────────────────────────────────
    struct head_regs_t {
        volatile uint64_t capabilities_id;        // 0x000 GCAP_ID (RO)
        uint64_t          reserved_008[1];        // 0x008
        volatile uint64_t general_config;         // 0x010 GEN_CONFIG (RW)
        uint64_t          reserved_018[1];        // 0x018
        volatile uint64_t general_int_status;     // 0x020 GEN_INT_STATUS (R/WC)
        uint64_t          reserved_028[25];       // 0x028–0x0E8
        volatile uint64_t main_counter_value;     // 0x0F0 MAIN_CNT (RW)
        uint64_t          reserved_0F8[1];        // 0x0F8

        // Timer 0  (0x100–0x11F)
        volatile uint64_t timer0_config_cap;      // 0x100
        volatile uint64_t timer0_comparator;      // 0x108
        volatile uint64_t timer0_fsb_route;       // 0x110
        uint64_t          timer0_reserved[1];     // 0x118

        // Timer 1  (0x120–0x13F)
        volatile uint64_t timer1_config_cap;      // 0x120
        volatile uint64_t timer1_comparator;      // 0x128
        volatile uint64_t timer1_fsb_route;       // 0x130
        uint64_t          timer1_reserved[1];     // 0x138

        // Timer 2  (0x140–0x15F)
        volatile uint64_t timer2_config_cap;      // 0x140
        volatile uint64_t timer2_comparator;      // 0x148
        volatile uint64_t timer2_fsb_route;       // 0x150
        uint64_t          timer2_reserved[1];     // 0x158

    } __attribute__((packed));
    static_assert(sizeof(head_regs_t) == 0x160, "head_regs size must be 0x160 (timer 0-2)");
    static_assert(offsetof(head_regs_t, general_config)     == 0x010, "GEN_CONFIG offset");
    static_assert(offsetof(head_regs_t, main_counter_value)  == 0x0F0, "MAIN_CNT offset");
    static_assert(offsetof(head_regs_t, timer0_config_cap)   == 0x100, "T0_CONFIG offset");
    static_assert(offsetof(head_regs_t, timer2_fsb_route)    == 0x150, "T2_FSB offset");

}} // namespace HPET::reg_layout

// ── 向后兼容：offset / bit 常量（供 init.elf 的 mem_loads.cpp 使用） ──
namespace HPET { namespace regs {
    constexpr uint32_t offset_General_Capabilities_and_ID            = 0x000;
    constexpr uint32_t offset_General_Config                         = 0x010;
    constexpr uint32_t offset_main_counter_value                     = 0x0F0;
    constexpr uint32_t offset_Timer0_Configuration_and_Capabilities  = 0x100;
    constexpr uint32_t size_per_timer                                = 0x020;
    constexpr uint8_t  COMPARATOR_COUNT_LEFT_OFFSET                  = 8;
    constexpr uint8_t  COMPARATOR_COUNT_MASK                         = 0x1F;
    constexpr uint8_t  COUNTER_CLK_PERIOD_LEFT_OFFSET                = 32;
    constexpr uint64_t GCONFIG_ENABLE_BIT                            = 1ULL << 0;
    constexpr uint64_t Tn_INT_ENB_CNF_BIT                            = 1ULL << 2;
}}

/* ═══════════════════════════════════════════════════════════════════════════
 * HPET 驱动程序
 *
 * 作为系统只读时钟源：
 *   1. Init 时清零主计数器并启动，读取 counter_clk_period 算出飞秒/微秒换算
 *   2. get_time_stamp_in_us() 返回自使能以来的微秒数
 *   3. 不提供中断定时器编程（后续 timer 配置可选）
 * ═══════════════════════════════════════════════════════════════════════════ */
class HPET_driver {
    HPET::reg_layout::head_regs_t* regs;
    uint64_t hpet_timer_period_fs;     // 每 tick 飞秒（来自 GCAP_ID[63:32]）

    KURD_t default_kurd();
    KURD_t default_success();

public:
    HPET_driver() : regs(nullptr), hpet_timer_period_fs(0) {}

    /* 初始化：映射 MMIO → 读 period → 清零主计数器 → 使能 */
    KURD_t Init(vm_interval* entry);

    /* 返回自计数器使能以来的微秒数 */
    uint64_t get_time_stamp_in_us();

    /* 低开销：直接返回主计数器原始值 */
    uint64_t get_raw_count() {
        return regs ? regs->main_counter_value : 0;
    }

    /* 调试：打印全部寄存器 */
    void dump_regs();
};

extern HPET_driver* readonly_timer;
