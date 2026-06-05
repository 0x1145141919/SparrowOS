#pragma once
#include <stdint.h>
#include "abi/os_error_definitions.h"
#include "abi/arch_code.h"
#include "exec_env_detect.h"

typedef  uint64_t miusecond_time_stamp_t;

constexpr uint64_t fs_per_ns   = 1000000ull;      // femtoseconds per nanosecond
constexpr uint64_t FS_per_mius = 1000000000ull;    // femtoseconds per microsecond

/**
 * @brief 内核宏观时间结构（通用日历/时间，不依赖 UEFI 方言）
 */
struct macro_tm {
    uint16_t year;          // 完整年份，如 2026
    uint8_t  month;         // 1-12
    uint8_t  day;           // 1-31
    uint8_t  hour;          // 0-23
    uint8_t  minute;        // 0-59
    uint8_t  second;        // 0-59
    uint16_t millisecond;   // 0-999
    int16_t  utc_offset;    // UTC 偏移（分钟），0x7FFF = 未指定
};

extern uint32_t tsc_fs_per_cycle;       // TSC 每 tick 的 femtoseconds（0 = 不可用）
extern bool     is_tsc_ddline_avaliabe;
extern bool     is_tsc_reliable;

namespace ktime
{
    macro_tm GetTime_in_os();
    int modify_time(macro_tm time);

    /*
     * 时间戳接口：
     *
     *   ┌──────────┬──────────────────────────────┬────────────────┐
     *   │ g_env    │ get_microsecond_stamp()       │ 时钟源         │
     *   ├──────────┼──────────────────────────────┼────────────────┤
     *   │ TCG      │ HPET（QEMU_CLOCK_VIRTUAL）    │ HPET MMIO      │
     *   │ KVM      │ TSC → ns 换算                │ pvclock/KVM    │
     *   │ BARE     │ TSC → ns 换算                │ CPUID 0x15     │
     *   └──────────┴──────────────────────────────┴────────────────┘
     *
     * get_microsecond_stamp() 是通用入口，里面按 g_env 分派。
     * get_nanosecond_stamp()  只在下层 tsc_fs_per_cycle!=0 时有效。
     */
    uint64_t get_microsecond_stamp();
    uint64_t get_nanosecond_stamp();    // 依赖 tsc_fs_per_cycle（TCG 下不可用）

    void microsecond_polling(uint64_t microseconds);

    namespace heart_beat_alarm
    {
        /*
         * 时钟中断模式：
         *   TCG          → lapic_timer_one_shot（HPET/虚拟时钟驱动）
         *   KVM/BARE     → lapic_timer_tsc_ddline（硬件 TSC deadline）
         *
         * processor_regist() 必须在 tsc_regist() 之后调用。
         */
        void processor_regist();
        void set_clock_by_stamp(miusecond_time_stamp_t stamp);
        void set_clock_by_offset(miusecond_time_stamp_t offset);
        void cancel_clock();
        bool is_alarm_set();
    }
}
