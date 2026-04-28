/**
 * @file rtc.cpp
<<<<<<< HEAD
 * @brief MC146818 CMOS RTC 驱动
 *
 * 通过 x86 CMOS 端口 (0x70/0x71) 读取硬件实时时钟。
 * 仅读不写，不修改 RTC 状态。
=======
 * @brief MC146818 CMOS RTC 驱动（仅读取，不写硬件）
>>>>>>> agent_kshell
 */
#include "arch/x86_64/core_hardwares/rtc.h"
#include <sys/io.h>

<<<<<<< HEAD
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
=======
#define RTC_ADDR  0x70
#define RTC_DATA  0x71

#define REG_SEC    0x00
#define REG_MIN    0x02
#define REG_HOUR   0x04
#define REG_DAY    0x07
#define REG_MONTH  0x08
#define REG_YEAR   0x09
#define REG_CENT   0x32
#define REG_STATUSA 0x0A
#define REG_STATUSB 0x0B
#define UIP_BIT    0x80

static uint8_t cmos_in(uint8_t reg) {
    outb(reg, RTC_ADDR);
    return inb(RTC_DATA);
}

static bool uip_busy() {
    return cmos_in(REG_STATUSA) & UIP_BIT;
}

static uint8_t bcd2bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

void rtc_read_time(macro_tm* out) {
    if (!out) return;
    out->year = out->month = out->day = 0;
    out->hour = out->minute = out->second = 0;
    out->millisecond = 0;
    out->utc_offset = 0x7FFF;

    for (int i = 0; i < 10; i++) {
        if (!uip_busy()) break;
        for (volatile int j = 0; j < 100; j++) asm("pause");
    }

    uint8_t sb   = cmos_in(REG_STATUSB);
    bool bin     = (sb & 0x04) != 0;
    bool h24     = (sb & 0x02) != 0;

    uint8_t sec  = cmos_in(REG_SEC);
    uint8_t min  = cmos_in(REG_MIN);
    uint8_t hr   = cmos_in(REG_HOUR);
    uint8_t day  = cmos_in(REG_DAY);
    uint8_t mon  = cmos_in(REG_MONTH);
    uint8_t yr   = cmos_in(REG_YEAR);
    uint8_t cen  = cmos_in(REG_CENT);

    if (!bin) {
        sec = bcd2bin(sec);
        min = bcd2bin(min);
        day = bcd2bin(day);
        mon = bcd2bin(mon);
        yr  = bcd2bin(yr);
        cen = bcd2bin(cen);
        if (!h24) {
            bool pm = (hr & 0x80) != 0;
            hr  = bcd2bin(hr & 0x7F);
            if (hr == 12) hr = 0;
            if (pm) hr += 12;
        } else {
            hr = bcd2bin(hr);
        }
    } else if (!h24) {
        bool pm = (hr & 0x80) != 0;
        hr &= 0x7F;
        if (hr == 12) hr = 0;
        if (pm) hr += 12;
    }

    uint16_t year_full = (uint16_t)(cen * 100 + yr);
    if (year_full < 1970) year_full += 100;

    out->year   = year_full;
    out->month  = mon;
>>>>>>> agent_kshell
    out->day    = day;
    out->hour   = hr;
    out->minute = min;
    out->second = sec;
}

// ── 纪元转换 ────────────────────────────────────────────────────

<<<<<<< HEAD
// 计算自 1970-01-01 以来的天数（不考虑闰秒）
static uint64_t days_since_epoch(uint16_t y, uint8_t m, uint8_t d) {
    uint64_t days = 0;
    for (uint16_t yr = 1970; yr < y; yr++) {
        days += is_leap(yr) ? 366 : 365;
    }
    for (uint8_t mo = 1; mo < m; mo++) {
        days += rtc_days_in_month(y, mo);
    }
=======
static uint64_t days_since_epoch(uint16_t y, uint8_t m, uint8_t d) {
    uint64_t days = 0;
    for (uint16_t yr = 1970; yr < y; yr++)
        days += rtc_is_leap(yr) ? 366 : 365;
    for (uint8_t mo = 1; mo < m; mo++)
        days += rtc_days_in_month(y, mo);
>>>>>>> agent_kshell
    days += (d - 1);
    return days;
}

<<<<<<< HEAD
uint64_t rtc_to_epoch_sec(const rtc_time_t* t) {
=======
uint64_t macro_tm_to_epoch_sec(const macro_tm* t) {
>>>>>>> agent_kshell
    if (!t) return 0;
    uint64_t days = days_since_epoch(t->year, t->month, t->day);
    return days * 86400ULL + t->hour * 3600ULL + t->minute * 60ULL + t->second;
}
<<<<<<< HEAD
=======

void epoch_sec_to_macro_tm(uint64_t epoch_sec, uint16_t millisec, macro_tm* out) {
    if (!out) return;
    out->millisecond = millisec;
    out->utc_offset  = 0x7FFF;

    #define D400 (365*400 + 97)
    #define D100 (365*100 + 24)
    #define D4   (365*4   + 1)

    uint64_t days  = epoch_sec / 86400ULL;
    uint64_t rsec  = epoch_sec % 86400ULL;

    uint16_t y = 1970;
    while (days >= D400) { y += 400; days -= D400; }
    while (days >= D100) { y += 100; days -= D100; }
    while (days >= D4)   { y += 4;   days -= D4;   }
    while (true) {
        uint16_t yd = rtc_is_leap(y) ? 366 : 365;
        if (days < yd) break;
        days -= yd; y++;
    }

    uint8_t mo = 1;
    while (true) {
        uint8_t dm = rtc_days_in_month(y, mo);
        if (days < dm) break;
        days -= dm; mo++;
    }

    out->year   = y;
    out->month  = mo;
    out->day    = (uint8_t)(days + 1);
    out->hour   = (uint8_t)(rsec / 3600ULL);
    rsec %= 3600ULL;
    out->minute = (uint8_t)(rsec / 60ULL);
    out->second = (uint8_t)(rsec % 60ULL);
}
>>>>>>> agent_kshell
