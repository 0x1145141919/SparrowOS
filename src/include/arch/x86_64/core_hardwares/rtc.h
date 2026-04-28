#pragma once
#include <stdint.h>
<<<<<<< HEAD

/**
 * @brief 解读后的 RTC 时间结构
 */
struct rtc_time_t {
    uint16_t year;      // 完整年份，如 2026
    uint8_t  month;     // 1-12
    uint8_t  day;       // 1-31
    uint8_t  hour;      // 0-23
    uint8_t  minute;    // 0-59
    uint8_t  second;    // 0-59
};
=======
#include "ktime.h"   // for macro_tm
>>>>>>> agent_kshell

/**
 * @brief 读取 CMOS RTC 时间
 *
<<<<<<< HEAD
 * 通过 CMOS 端口 (0x70/0x71) 读取 MC146818 RTC 时间。
 * 等待 UIP 位清除后，在单次读取窗口内获取所有时间寄存器，
 * 自动处理 BCD→二进制转换和 12/24 小时制。
 *
 * @param out 输出时间结构，若读取失败则全部写 0
 */
void rtc_read_time(rtc_time_t* out);

/**
 * @brief 将 rtc_time_t 转换为 Unix 纪元秒（自 1970-01-01 00:00:00 UTC）
 */
uint64_t rtc_to_epoch_sec(const rtc_time_t* t);

/**
 * @brief 闰年判断
 */
=======
 * 通过 CMOS 端口 (0x70/0x71) 读取 MC146818 RTC。
 * 自动处理 UIP、BCD→二进制、12/24 小时制。
 *
 * @param out 输出 macro_tm，失败时全部填 0
 */
void rtc_read_time(macro_tm* out);

/**
 * @brief macro_tm → Unix 纪元秒（自 1970-01-01 00:00:00 UTC）
 */
uint64_t macro_tm_to_epoch_sec(const macro_tm* t);

/**
 * @brief Unix 纪元秒 + 毫秒 → macro_tm
 */
void epoch_sec_to_macro_tm(uint64_t epoch_sec, uint16_t millisec, macro_tm* out);

// ── 日历辅助（inline，通用） ─────────────────────────────────────

>>>>>>> agent_kshell
static inline bool rtc_is_leap(uint16_t y) {
    return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0);
}

<<<<<<< HEAD
/**
 * @brief 月份天数
 */
=======
>>>>>>> agent_kshell
static inline uint8_t rtc_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    uint8_t d = dpm[month - 1];
    if (month == 2 && rtc_is_leap(year)) d = 29;
    return d;
}
