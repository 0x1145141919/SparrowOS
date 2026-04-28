#pragma once
#include <stdint.h>
#include "abi/os_error_definitions.h"
#include "abi/arch_code.h"
typedef  uint64_t miusecond_time_stamp_t;
constexpr uint64_t fs_per_ns=1000000ull;
constexpr uint64_t FS_per_mius=1000000000ull;
<<<<<<< HEAD
struct macro_tm {
    uint8_t tm_sec;     /* 秒 (0-60, 允许闰秒) */
    uint8_t tm_min;     /* 分 (0-59) */
    uint8_t tm_hour;    /* 时 (0-23) */
    uint8_t tm_mday;    /* 一个月中的第几天 (1-31) */
    uint8_t tm_mon;     /* 月份 (0-11, 0 = 一月) */
    int8_t tm_isdst;   /* 夏令时标志 (正数: 启用, 0: 未启用, 负数: 不可用) */
    uint32_t tm_year;    /* 年份 - 1900 */
    uint16_t tm_wday;    /* 一周中的第几天 (0-6, 0 = 周日) */
    uint16_t tm_yday;    /* 一年中的第几天 (0-365) */
};
namespace ktime
{
    macro_tm GetTime_in_os();  
=======

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

namespace ktime
{
    macro_tm GetTime_in_os();
>>>>>>> agent_kshell
    int modify_time(macro_tm time);
    uint64_t get_microsecond_stamp();
    uint64_t get_nanosecond_stamp();
    void microsecond_polling_delay_by_hpet(uint64_t microseconds);
    namespace heart_beat_alarm
    {
    void processor_regist();//在x86_64上必须要先tsc_regist后再运行
    void set_clock_by_stamp(miusecond_time_stamp_t stamp);
    void set_clock_by_offset(miusecond_time_stamp_t offset);
    void cancel_clock();
    bool is_alarm_set(); 
    }    
}