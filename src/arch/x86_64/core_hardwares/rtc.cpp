/**
 * @file rtc.cpp
 * @brief MC146818 CMOS RTC 驱动
 *
 * 通过 x86 CMOS 端口 (0x70/0x71) 读取硬件实时时钟。
 * 仅读不写，不修改 RTC 状态。
 */
#include "arch/x86_64/core_hardwares/rtc.h"
#include <sys/io.h>

// ── CMOS RTC 端口与寄存器 ─────────────────────────────────────

#define RTC_ADDR_PORT  0x70
#define RTC_DATA_PORT  0x71

#define RTC_REG_SECONDS    0x00
#define RTC_REG_MINUTES    0x02
#define RTC_REG_HOURS      0x04
#define RTC_REG_DAY        0x07
#define RTC_REG_MONTH      0x08
#define RTC_REG_YEAR       0x09
#define RTC_REG_CENTURY    0x32
#define RTC_REG_STATUSA    0x0A
#define RTC_REG_STATUSB    0x0B

#define RTC_UIP_BIT  0x80   // Update In Progress (Status A bit 7)

// ── 辅助 ────────────────────────────────────────────────────────

static uint8_t cmos_read(uint8_t reg) {
    outb(reg, RTC_ADDR_PORT);
    return inb(RTC_DATA_PORT);
}

static bool is_update_in_progress() {
    return cmos_read(RTC_REG_STATUSA) & RTC_UIP_BIT;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

static uint8_t fix_hour_12to24(uint8_t hour_reg, bool is_pm_flag) {
    // 12-hour mode: bit 7 = PM flag, bits 0-6 = hour (1-12)
    uint8_t h = hour_reg & 0x7F;
    if (h == 12) h = 0;
    if (is_pm_flag) h += 12;
    return h;
}

// ── 公开接口 ────────────────────────────────────────────────────

void rtc_read_time(rtc_time_t* out) {
    if (!out) return;

    // 等待 UIP 清除
    for (int retry = 0; retry < 10; retry++) {
        if (!is_update_in_progress()) break;
        // 忙等 ~1us (pause 指令 + 空转)
        for (volatile int i = 0; i < 100; i++) asm("pause");
    }

    // 在一次 UIP 窗口内读取所有寄存器
    uint8_t status_b = cmos_read(RTC_REG_STATUSB);
    bool is_binary = (status_b & 0x04) != 0;
    bool is_24h   = (status_b & 0x02) != 0;

    uint8_t sec     = cmos_read(RTC_REG_SECONDS);
    uint8_t min     = cmos_read(RTC_REG_MINUTES);
    uint8_t hr      = cmos_read(RTC_REG_HOURS);
    uint8_t day     = cmos_read(RTC_REG_DAY);
    uint8_t month   = cmos_read(RTC_REG_MONTH);
    uint8_t year    = cmos_read(RTC_REG_YEAR);
    uint8_t century = cmos_read(RTC_REG_CENTURY);

    if (!is_binary) {
        sec    = bcd_to_bin(sec);
        min    = bcd_to_bin(min);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
        century = bcd_to_bin(century);
        if (!is_24h) {
            bool pm = (hr & 0x80) != 0;
            hr = bcd_to_bin(hr & 0x7F);
            hr = fix_hour_12to24(hr, pm);
        } else {
            hr = bcd_to_bin(hr);
        }
    } else {
        if (!is_24h) {
            bool pm = (hr & 0x80) != 0;
            hr = fix_hour_12to24(hr, pm);
        }
    }

    out->year   = (uint16_t)(century * 100 + year);
    if (out->year < 1970) out->year += 100;  // 世纪回绕修正
    out->month  = month;
    out->day    = day;
    out->hour   = hr;
    out->minute = min;
    out->second = sec;
}

// ── 纪元转换 ────────────────────────────────────────────────────

// 计算自 1970-01-01 以来的天数（不考虑闰秒）
static uint64_t days_since_epoch(uint16_t y, uint8_t m, uint8_t d) {
    uint64_t days = 0;
    for (uint16_t yr = 1970; yr < y; yr++) {
        days += is_leap(yr) ? 366 : 365;
    }
    for (uint8_t mo = 1; mo < m; mo++) {
        days += rtc_days_in_month(y, mo);
    }
    days += (d - 1);
    return days;
}

uint64_t rtc_to_epoch_sec(const rtc_time_t* t) {
    if (!t) return 0;
    uint64_t days = days_since_epoch(t->year, t->month, t->day);
    return days * 86400ULL + t->hour * 3600ULL + t->minute * 60ULL + t->second;
}
