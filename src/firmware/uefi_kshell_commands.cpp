/**
 * @file uefi_kshell_commands.cpp
 * @brief kshell UEFI 运行时服务命令实现
 *
 * 遵行 Docs/kshell_firmware_system_commands_design.md。
 * 当前仅实现 UEFI 运行时服务命令，ACPI 命令暂不实现。
 * 无 libc/libc++ 依赖。
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "util/lock.h"
#include "util/OS_utils.h"
#include "firmware/UefiRunTimeServices.h"
#include <efi.h>

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

// ── 辅助 ────────────────────────────────────────────────────────

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

static bool tok_eq(const token_t& t, const char* s) {
    size_t n = strlen_in_kernel(s);
    return (t.len == n) && (strncmp(t.str, s, n) == 0);
}

static int parse_uint(const token_t& t, uint64_t* out) {
    if (t.len == 0 || t.len > 20) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < t.len; i++) {
        if (t.str[i] < '0' || t.str[i] > '9') return -1;
        v = v * 10 + (uint64_t)(t.str[i] - '0');
    }
    *out = v;
    return 0;
}

// ── 日历验证 ────────────────────────────────────────────────────

static bool is_leap(uint16_t y) {
    return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0);
}

static uint8_t days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    uint8_t d = dpm[month - 1];
    if (month == 2 && is_leap(year)) d = 29;
    return d;
}

static bool validate_datetime(uint16_t y, uint8_t mo, uint8_t d,
                               uint8_t h, uint8_t mi, uint8_t s,
                               uint32_t ns, int16_t tz)
{
    if (y < 1998 || y > 2099) return false;
    if (mo < 1 || mo > 12) return false;
    if (d < 1 || d > days_in_month(y, mo)) return false;
    if (h > 23) return false;
    if (mi > 59) return false;
    if (s > 59) return false;
    if (ns > 999999999) return false;
    if (tz != EFI_UNSPECIFIED_TIMEZONE && (tz < -1440 || tz > 1440)) return false;
    return true;
}

// ── 时间显示辅助 ───────────────────────────────────────────────

static void print_time_full(const EFI_TIME& t) {
    bsp_kout << t.Year << "-";
    if (t.Month < 10) bsp_kout << "0";
    bsp_kout << t.Month << "-";
    if (t.Day < 10) bsp_kout << "0";
    bsp_kout << t.Day << "  ";
    if (t.Hour < 10) bsp_kout << "0";
    bsp_kout << t.Hour << ":";
    if (t.Minute < 10) bsp_kout << "0";
    bsp_kout << t.Minute << ":";
    if (t.Second < 10) bsp_kout << "0";
    bsp_kout << t.Second << ".";
    uint64_t ns = t.Nanosecond;
    uint64_t div = 100000000;
    for (int i = 0; i < 9; i++) {
        bsp_kout << (char)('0' + (ns / div));
        ns %= div;
        div /= 10;
    }

    if (t.TimeZone == EFI_UNSPECIFIED_TIMEZONE) {
        bsp_kout << "  TZ=unspecified";
    } else {
        int16_t abs_tz = t.TimeZone < 0 ? -t.TimeZone : t.TimeZone;
        int16_t tz_h = abs_tz / 60;
        int16_t tz_m = abs_tz % 60;
        bsp_kout << "  UTC" << (t.TimeZone < 0 ? "-" : "+");
        if (tz_h < 10) bsp_kout << "0";
        bsp_kout << tz_h << ":";
        if (tz_m < 10) bsp_kout << "0";
        bsp_kout << tz_m;
    }

    if (t.Daylight & EFI_TIME_IN_DAYLIGHT)   bsp_kout << " [DST]";
    if (t.Daylight & EFI_TIME_ADJUST_DAYLIGHT) bsp_kout << " [ADJ]";
}

static void print_time_simple(const EFI_TIME& t) {
    bsp_kout << t.Year << "-";
    if (t.Month < 10) bsp_kout << "0";
    bsp_kout << t.Month << "-";
    if (t.Day < 10) bsp_kout << "0";
    bsp_kout << t.Day << "  ";
    if (t.Hour < 10) bsp_kout << "0";
    bsp_kout << t.Hour << ":";
    if (t.Minute < 10) bsp_kout << "0";
    bsp_kout << t.Minute << ":";
    if (t.Second < 10) bsp_kout << "0";
    bsp_kout << t.Second;
}

// ── 确认辅助 ────────────────────────────────────────────────────

static bool confirm_with_word(const char* prompt, const char* expected) {
    bsp_kout << "[WARNING] " << prompt << kendl;
    bsp_kout << "Type '" << expected << "' to confirm: ";

    char buf[64];
    size_t pos = 0;
    while (pos < sizeof(buf) - 1) {
        i8042_blockable_keyboard_listening(buf + pos);
        char c = buf[pos];
        if (c == '\r' || c == '\n') break;
        if (c == '\b' || c == 127) {
            if (pos > 0) { pos--; bsp_kout << "\b \b"; }
            continue;
        }
        if (c == 3) { bsp_kout << "^C" << kendl; return false; }
        bsp_kout << c;
        pos++;
    }
    buf[pos] = '\0';
    bsp_kout << kendl;
    return (strcmp_in_kernel(buf, expected) == 0);
}

// ═══════════════════════════════════════════════════════════════════
//  命令实现
// ═══════════════════════════════════════════════════════════════════

// ── uefitime ────────────────────────────────────────────────────

KURD_t cmd_uefitime(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    enum { FULL, SIMPLE, TS } mode = FULL;
    if (line->token_count >= 2) {
        if (tok_eq(line->tokens[1], "simple"))    mode = SIMPLE;
        else if (tok_eq(line->tokens[1], "timestamp")) mode = TS;
    }

    EFI_TIME t = EFI_RT_SVS::rt_time_get();
    if (t.Year == 0 && t.Month == 0 && t.Day == 0) {
        bsp_kout << "[ERROR] Failed to get UEFI time" << kendl;
        return ok;
    }

    bsp_kout << "UEFI Time: ";
    switch (mode) {
        case FULL:   print_time_full(t);      break;
        case SIMPLE: print_time_simple(t);     break;
        case TS:
            bsp_kout << "Year=" << t.Year << " Month=" << t.Month
                     << " Day=" << t.Day << " Hour=" << t.Hour
                     << " Minute=" << t.Minute << " Second=" << t.Second;
            break;
    }
    bsp_kout << kendl;
    return ok;
}

// ── uefisettime ────────────────────────────────────────────────

KURD_t cmd_uefisettime(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: uefisettime YYYY-MM-DD HH:MM:SS [-f]" << kendl;
        return ok;
    }

    bool force = false;
    for (uint16_t i = 3; i < line->token_count; i++) {
        if (tok_eq(line->tokens[i], "-f")) force = true;
    }

    // 解析日期 YYYY-MM-DD
    const token_t& date_tok = line->tokens[1];
    if (date_tok.len != 10) {
        bsp_kout << "[ERROR] Date format: YYYY-MM-DD" << kendl;
        return ok;
    }
    uint64_t y, mo, d;
    token_t yt = {date_tok.str, 4};
    token_t mot = {date_tok.str + 5, 2};
    token_t dt = {date_tok.str + 8, 2};
    if (date_tok.str[4] != '-' || date_tok.str[7] != '-') {
        bsp_kout << "[ERROR] Date format: YYYY-MM-DD" << kendl;
        return ok;
    }
    if (parse_uint(yt, &y) || parse_uint(mot, &mo) || parse_uint(dt, &d)) {
        bsp_kout << "[ERROR] Invalid date" << kendl;
        return ok;
    }

    // 解析时间 HH:MM:SS
    const token_t& time_tok = line->tokens[2];
    if (time_tok.len != 8) {
        bsp_kout << "[ERROR] Time format: HH:MM:SS" << kendl;
        return ok;
    }
    uint64_t h, mi, s;
    token_t ht = {time_tok.str, 2};
    token_t mit = {time_tok.str + 3, 2};
    token_t st = {time_tok.str + 6, 2};
    if (time_tok.str[2] != ':' || time_tok.str[5] != ':') {
        bsp_kout << "[ERROR] Time format: HH:MM:SS" << kendl;
        return ok;
    }
    if (parse_uint(ht, &h) || parse_uint(mit, &mi) || parse_uint(st, &s)) {
        bsp_kout << "[ERROR] Invalid time" << kendl;
        return ok;
    }

    if (!validate_datetime((uint16_t)y, (uint8_t)mo, (uint8_t)d,
                           (uint8_t)h, (uint8_t)mi, (uint8_t)s,
                           0, EFI_UNSPECIFIED_TIMEZONE)) {
        bsp_kout << "[ERROR] Invalid date/time value" << kendl;
        return ok;
    }

    bsp_kout << "Setting UEFI time to: ";
    bsp_kout << y << "-";
    if (mo < 10) bsp_kout << "0";
    bsp_kout << mo << "-";
    if (d < 10) bsp_kout << "0"; bsp_kout << d << "  ";
    if (h < 10) bsp_kout << "0"; bsp_kout << h << ":";
    if (mi < 10) bsp_kout << "0"; bsp_kout << mi << ":";
    if (s < 10) bsp_kout << "0"; bsp_kout << s << kendl;

    if (!force) {
        if (!confirm_with_word("Setting system time may affect running services.", "SETTIME")) {
            bsp_kout << "[uefisettime] Cancelled." << kendl;
            return ok;
        }
    }

    EFI_TIME new_time;
    ksetmem_8(&new_time, 0, sizeof(new_time));
    new_time.Year       = (uint16_t)y;
    new_time.Month      = (uint8_t)mo;
    new_time.Day        = (uint8_t)d;
    new_time.Hour       = (uint8_t)h;
    new_time.Minute     = (uint8_t)mi;
    new_time.Second     = (uint8_t)s;
    new_time.Nanosecond = 0;
    new_time.TimeZone   = EFI_UNSPECIFIED_TIMEZONE;
    new_time.Daylight   = 0;

    EFI_STATUS st = EFI_RT_SVS::rt_time_set(new_time);
    if (st == EFI_SUCCESS) {
        bsp_kout << "[uefisettime] Time set successfully." << kendl;
    } else {
        bsp_kout << "[ERROR] rt_time_set failed, status=0x" << HEX << st << DEC << kendl;
    }
    return ok;
}

// ── uefireboot ─────────────────────────────────────────────────

KURD_t cmd_uefireboot(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (!confirm_with_word("This will REBOOT the system immediately!", "REBOOT")) {
        bsp_kout << "[uefireboot] Cancelled." << kendl;
        return ok;
    }
    bsp_kout << "[uefireboot] Rebooting (warm reset)..." << kendl;
    EFI_RT_SVS::rt_hotreset();
    return ok;
}

// ── ueficreset ─────────────────────────────────────────────────

KURD_t cmd_ueficreset(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (!confirm_with_word("This will COLD RESET the system immediately!", "REBOOT")) {
        bsp_kout << "[ueficreset] Cancelled." << kendl;
        return ok;
    }
    bsp_kout << "[ueficreset] Rebooting (cold reset)..." << kendl;
    EFI_RT_SVS::rt_coldreset();
    return ok;
}

// ── uefishutdown ───────────────────────────────────────────────

KURD_t cmd_uefishutdown(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (!confirm_with_word("This will SHUTDOWN the system immediately!", "SHUTDOWN")) {
        bsp_kout << "[uefishutdown] Cancelled." << kendl;
        return ok;
    }
    bsp_kout << "[uefishutdown] Shutting down..." << kendl;
    EFI_RT_SVS::rt_shutdown();
    return ok;
}
