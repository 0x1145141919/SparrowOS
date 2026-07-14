/**
 * @file uefi_kshell_commands.cpp
 * @brief kshell system commands (legacy reset, OS time)
 *
 * Renamed mission: UEFI runtime service commands removed.
 * Kept: legacy_reboot via 0xcf9, OS RTC-anchored calendar time.
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "util/lock.h"
#include "util/OS_utils.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "ktime.h"
#include <arch/x86_64/core_hardwares/lapic.h>
using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

static constexpr int16_t TZ_UNSPECIFIED = 0x07FF;

// ── 辅助 ────────────────────────────────────────────────────────

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

static bool tok_eq(const token_t& t, const char* s) {
    size_t n = strlen_in_kernel(s);
    return (t.len == n) && (strcmp_in_kernel(t.str, s, n) == 0);
}

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
    if (tz != TZ_UNSPECIFIED && (tz < -1440 || tz > 1440)) return false;
    return true;
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

// ── legacy_reboot ─────────────────────────────────────────────
extern "C" void broadcast_shutdown();
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
    bsp_kout << "[legacy_reboot] Issuing " << (cold ? "cold" : "warm")
             << " reset via port 0xcf9 (val=0x" << HEX << (uint32_t)val << DEC << ")..." << kendl;

    bsp_kout << "[legacy_reboot] Halting APs..." << kendl;
    broadcast_shutdown();
    __asm__ volatile("wbinvd");
    outb(0xcf9, val);
    __asm__ volatile("1: hlt; jmp 1b");
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

    if (!validate_datetime(y, mo, d, h, mi, s, 0, TZ_UNSPECIFIED)) {
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
