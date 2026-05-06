/**
 * @file rtc.cpp
 * @brief MC146818 CMOS RTC 驱动（仅读取，不写硬件）
 */
#include "arch/x86_64/core_hardwares/rtc.h"
#include <sys/io.h>

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
    out->day    = day;
    out->hour   = hr;
    out->minute = min;
    out->second = sec;
}

// ── 纪元转换 ────────────────────────────────────────────────────

static uint64_t days_since_epoch(uint16_t y, uint8_t m, uint8_t d) {
    uint64_t days = 0;
    for (uint16_t yr = 1970; yr < y; yr++)
        days += rtc_is_leap(yr) ? 366 : 365;
    for (uint8_t mo = 1; mo < m; mo++)
        days += rtc_days_in_month(y, mo);
    days += (d - 1);
    return days;
}

uint64_t macro_tm_to_epoch_sec(const macro_tm* t) {
    if (!t) return 0;
    uint64_t days = days_since_epoch(t->year, t->month, t->day);
    return days * 86400ULL + t->hour * 3600ULL + t->minute * 60ULL + t->second;
}

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
