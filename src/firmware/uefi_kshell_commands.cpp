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
#include "arch/x86_64/core_hardwares/i8042.h"
#include "ktime.h"
#include <efi.h>
#include <arch/x86_64/core_hardwares/lapic.h>
using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

// ── 辅助 ────────────────────────────────────────────────────────

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

static bool tok_eq(const token_t& t, const char* s) {
    size_t n = strlen_in_kernel(s);
    return (t.len == n) && (strcmp_in_kernel(t.str, s, n) == 0);
}

// 用于解析日期/时间子串（YYYY-MM-DD HH:MM:SS 格式中通过 str+len 构造的片段）。
// 这些是 ad-hoc 子串而非 parse_line 分出的 token，无法使用 token_to_uint64。
// 注意：不使用 token_t 类型，避免与框架 token 解析混淆。
static bool parse_digits(const char* s, size_t len, uint64_t* out) {
    if (len == 0 || len > 20) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return true;
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
        buff_t kb;
        i8042_blockable_keyboard_listening(&kb);
        for(uint16_t i = 0; i < kb.len; i++) {
            char c = kb.data[i];
            if (c == '\r' || c == '\n') goto done;
            if (c == '\b' || c == 127) {
                if (pos > 0) { pos--; bsp_kout << "\b \b"; }
                continue;
            }
            if (c == 3) { bsp_kout << "^C" << kendl; return false; }
            bsp_kout << c;
            if(pos < sizeof(buf) - 1) buf[pos++] = c;
        }
    }
done:
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
    if (!parse_digits(yt.str, 4, &y) || !parse_digits(mot.str, 2, &mo) || !parse_digits(dt.str, 2, &d)) {
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
    if (!parse_digits(ht.str, 2, &h) || !parse_digits(mit.str, 2, &mi) || !parse_digits(st.str, 2, &s)) {
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

    EFI_STATUS status = EFI_RT_SVS::rt_time_set(new_time);
    if (status == EFI_SUCCESS) {
        bsp_kout << "[uefisettime] Time set successfully." << kendl;
    } else {
        bsp_kout << "[ERROR] rt_time_set failed, status=0x" << HEX << (uint64_t)status << DEC << kendl;
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


void halt_foever(){
    asm volatile("cli;hlt");
}
// ── legacy_reboot ─────────────────────────────────────────────

KURD_t cmd_legacy_reboot(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    bool cold = false, warm = false;
    for (uint16_t i = 1; i < line->token_count; i++) {
        if (tok_eq(line->tokens[i], "--cold")) cold = true;
        if (tok_eq(line->tokens[i], "--warm")) warm = true;
    }
    if (cold && warm) {
        bsp_kout << "[legacy_reboot] --cold and --warm are mutually exclusive" << kendl;
        return ok;
    }

    if (!confirm_with_word("This will RESET the system via 0xcf9 port!", "RESET")) {
        bsp_kout << "[legacy_reboot] Cancelled." << kendl;
        return ok;
    }

    uint8_t val = warm ? 0x06 : 0x0E;
    // 0x0A = warm reset:  bit1=RST_CPU, bit3=SYS_RST  (cf9 spec)
    // 0x0E = cold reset:  bit1=RST_CPU, bit2=FULL_RST, bit3=SYS_RST
    bsp_kout << "[legacy_reboot] Issuing " << (cold ? "cold" : "warm")
             << " reset via port 0xcf9 (val=0x" << HEX << (uint32_t)val << DEC << ")..." << kendl;

    bsp_kout << "[legacy_reboot] Flushing caches..." << kendl;
    x2apic::x2apic_driver::broadcast_exself_fixed_ipi(halt_foever);
    __asm__ volatile("wbinvd");
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"((uint16_t)0xcf9));
    __asm__ volatile("1: hlt; jmp 1b");
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

// ── uefiptrs ───────────────────────────────────────────────────

KURD_t cmd_uefiptrs(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;
    EFI_RT_SVS::dump_func_ptrs();
    return ok;
}

// ── 打印 macro_tm ─────────────────────────────────────────────

static void print_macro_tm(const macro_tm& t) {
    bsp_kout << (uint64_t)t.year << "-";
    if (t.month < 10) bsp_kout << "0";
    bsp_kout << (uint64_t)t.month << "-";
    if (t.day < 10) bsp_kout << "0";
    bsp_kout << (uint64_t)t.day << "  ";
    if (t.hour < 10) bsp_kout << "0";
    bsp_kout << (uint64_t)t.hour << ":";
    if (t.minute < 10) bsp_kout << "0";
    bsp_kout << (uint64_t)t.minute << ":";
    if (t.second < 10) bsp_kout << "0";
    bsp_kout << (uint64_t)t.second;
    bsp_kout << "." << (uint64_t)t.millisecond;
    if (t.utc_offset != 0x7FFF) {
        int16_t abs_off = t.utc_offset < 0 ? -t.utc_offset : t.utc_offset;
        bsp_kout << "  UTC" << (t.utc_offset < 0 ? "-" : "+")
                 << (abs_off / 60) << ":" << (abs_off % 60);
    }
}

// ── get_macro_time ────────────────────────────────────────────

KURD_t cmd_get_macro_time(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    macro_tm t = ktime::GetTime_in_os();
    if (t.year == 0) {
        bsp_kout << "[ERROR] Time not available" << kendl;
        return ok;
    }
    print_macro_tm(t);
    bsp_kout << kendl;
    return ok;
}

// ── set_marcro_time ───────────────────────────────────────────

KURD_t cmd_set_marcro_time(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: set_marcro_time YYYY-MM-DD [HH:MM:SS]" << kendl;
        return ok;
    }

    uint16_t y = 0; uint8_t mo = 1, d = 1, h = 0, mi = 0, s = 0;

    // 日期 YYYY-MM-DD
    const token_t& dt = line->tokens[1];
    if (dt.len != 10 || dt.str[4] != '-' || dt.str[7] != '-') {
        bsp_kout << "[ERROR] Date format: YYYY-MM-DD" << kendl;
        return ok;
    }
    uint64_t tmp;
    token_t yt  = {dt.str, 4};
    token_t mot = {dt.str + 5, 2};
    token_t dat = {dt.str + 8, 2};
    if (!parse_digits(yt.str, 4, &tmp) || !parse_digits(mot.str, 2, &tmp) || !parse_digits(dat.str, 2, &tmp))
        return ok;
    y = (uint16_t)tmp; mo = (uint8_t)tmp; d = (uint8_t)tmp;

    // 可选时间 HH:MM:SS
    if (line->token_count >= 3) {
        const token_t& tt = line->tokens[2];
        if (tt.len == 8 && tt.str[2] == ':' && tt.str[5] == ':') {
            token_t ht  = {tt.str, 2};
            token_t mit = {tt.str + 3, 2};
            token_t st  = {tt.str + 6, 2};
            if (!parse_digits(ht.str, 2, &tmp) || !parse_digits(mit.str, 2, &tmp) || !parse_digits(st.str, 2, &tmp))
                return ok;
            h = (uint8_t)tmp; mi = (uint8_t)tmp; s = (uint8_t)tmp;
        }
    }

    // 验证
    if (!validate_datetime(y, mo, d, h, mi, s, 0, EFI_UNSPECIFIED_TIMEZONE)) {
        bsp_kout << "[ERROR] Invalid date/time" << kendl;
        return ok;
    }

    macro_tm target;
    target.year        = y;
    target.month       = mo;
    target.day         = d;
    target.hour        = h;
    target.minute      = mi;
    target.second      = s;
    target.millisecond = 0;
    target.utc_offset  = 0x7FFF;

    ktime::modify_time(target);
    bsp_kout << "[set_marcro_time] Calibration offset set." << kendl;
    return ok;
}
