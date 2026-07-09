/**
 * @file i8042_kshell.cpp
 * @brief kshell i8042 键盘诊断命令实现
 * 
 * 所有命令只读，SAFE 等级，无副作用。
 * 遵循 Docs/kshell_i8042_keyboard_commands_design.md。
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "Scheduler/per_processor_scheduler.h"
#include <sys/io.h>
#include "util/lock.h"
#include "util/OS_utils.h"

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

// ── 修饰键位常量（与 i8042.cpp 同步） ────────────────────────────
static constexpr uint8_t MOD_SHIFT  = 1 << 0;
static constexpr uint8_t MOD_CTRL   = 1 << 1;
static constexpr uint8_t MOD_ALT    = 1 << 2;
static constexpr uint8_t MOD_CAPS   = 1 << 3;
static constexpr uint8_t MOD_NUM    = 1 << 4;
static constexpr uint8_t MOD_SCROLL = 1 << 5;

// ── 辅助 ────────────────────────────────────────────────────────

static KURD_t make_success() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}
static KURD_t make_fail() {
    return {result_code::FAIL, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::ERROR, err_domain::CORE_MODULE};
}

static bool token_eq(const token_t& tok, const char* s) {
    size_t n = strlen_in_kernel(s);
    if (tok.len != n) return false;
    return strcmp_in_kernel(tok.str, s, n) == 0;
}

static void print_modifier_summary(uint8_t m) {
    bsp_kout << "  Shift: "     << ((m & MOD_SHIFT)  ? "PRESSED"  : "released") << "  ";
    bsp_kout << "Ctrl: "        << ((m & MOD_CTRL)   ? "PRESSED"  : "released") << "  ";
    bsp_kout << "Alt: "         << ((m & MOD_ALT)    ? "PRESSED"  : "released") << kendl;
    bsp_kout << "  Caps Lock: " << ((m & MOD_CAPS)   ? "ON"       : "OFF")      << "  ";
    bsp_kout << "Num Lock: "    << ((m & MOD_NUM)    ? "ON"       : "OFF")      << "  ";
    bsp_kout << "Scroll Lock: " << ((m & MOD_SCROLL) ? "ON"       : "OFF")      << kendl;
}

static const char* action_str(uint8_t action) {
    if (action == static_cast<uint8_t>(ps2_key_action::make))   return "MAKE";
    if (action == static_cast<uint8_t>(ps2_key_action::break_)) return "BREAK";
    if (action == static_cast<uint8_t>(ps2_key_action::repeat)) return "REPEAT";
    return "ERROR";
}

static bool get_event(uint64_t seq, ps_2_keyboard_event* ev) {
    if (seq >= i8042_get_publish_seq()) return false;
    uint64_t diff = i8042_get_publish_seq() - seq;
    if (diff > i8042_buffer_max_size) return false;
    return i8042_read_event_by_seq(seq, ev);
}

// ── 修饰键历史记录（环形，在扫描事件时维护） ──────────────────
struct modifier_record {
    uint64_t timestamp_us;
    uint8_t old_mod;
    uint8_t new_mod;
    uint8_t cause_key;
    uint8_t is_break;
};
static constexpr uint8_t MOD_HISTORY_DEPTH = 10;
static modifier_record g_mod_history[MOD_HISTORY_DEPTH];
static uint8_t g_mod_history_idx = 0;
static uint8_t g_mod_history_count = 0;

static void maybe_record_mod_change(uint8_t key_code, bool is_break, uint8_t old_m, uint8_t new_m, uint64_t ts) {
    if (old_m == new_m) return;
    modifier_record* r = &g_mod_history[g_mod_history_idx];
    r->timestamp_us = ts;
    r->old_mod = old_m;
    r->new_mod = new_m;
    r->cause_key = key_code;
    r->is_break = is_break ? 1 : 0;
    g_mod_history_idx = (g_mod_history_idx + 1) % MOD_HISTORY_DEPTH;
    if (g_mod_history_count < MOD_HISTORY_DEPTH) g_mod_history_count++;
}

// ── 丢弃事件记录 ────────────────────────────────────────────────
struct drop_record {
    uint64_t timestamp_us;
    char ch;
    uint16_t reason; // 0 = ring overflow
};
static constexpr uint8_t DROP_RECORD_DEPTH = 5;
static drop_record g_drop_records[DROP_RECORD_DEPTH];
static uint8_t g_drop_idx = 0;
static uint8_t g_drop_count = 0;

static void record_drop(uint64_t ts, char ch, uint16_t reason) {
    drop_record* r = &g_drop_records[g_drop_idx];
    r->timestamp_us = ts;
    r->ch = ch;
    r->reason = reason;
    g_drop_idx = (g_drop_idx + 1) % DROP_RECORD_DEPTH;
    if (g_drop_count < DROP_RECORD_DEPTH) g_drop_count++;
}

// 扫描事件环以填充修饰键历史和丢弃记录（轻量，到命令调用时扫描）
static void scan_event_ring_for_history(uint8_t* out_mod) {
    uint64_t seq = i8042_get_publish_seq();
    if (seq == 0) {
        *out_mod = 0;
        return;
    }
    // 扫描范围：最近 up to i8042_buffer_max_size 个事件
    uint64_t start = (seq > i8042_buffer_max_size) ? seq - i8042_buffer_max_size : 0;
    uint8_t mod = 0;
    for (uint64_t s = start; s < seq; s++) {
        ps_2_keyboard_event ev;
        if (!get_event(s, &ev)) continue;
        uint8_t old_mod = mod;
        uint8_t act = ev.action;
        uint8_t mk   = static_cast<uint8_t>(ps2_key_action::make);
        uint8_t brk  = static_cast<uint8_t>(ps2_key_action::break_);
        bool is_ext  = (ev.flags & ps2_event_flag_extended_e0) != 0;
        bool is_syn  = (ev.flags & ps2_event_flag_synthetic) != 0;
        switch (ev.key_code) {
            case 0x2A: case 0x36:
                if (act == brk) mod &= ~MOD_SHIFT; else mod |= MOD_SHIFT;
                break;
            case 0x1D:
                if (act == brk) mod &= ~MOD_CTRL; else mod |= MOD_CTRL;
                break;
            case 0x38:
                if (act == brk) mod &= ~MOD_ALT; else mod |= MOD_ALT;
                break;
            case 0x3A:
                if (act == mk && !is_ext) mod ^= MOD_CAPS;
                break;
            case 0x45:
                if (act == mk && !is_syn) mod ^= MOD_NUM;
                break;
            case 0x46:
                if (act == mk && !is_ext) mod ^= MOD_SCROLL;
                break;
        }
        maybe_record_mod_change(ev.key_code, (act == brk), old_mod, mod, ev.timestamp_us);
    }
    *out_mod = mod;
}

// 扫描字符环以捕获丢弃记录
static void scan_char_ring_for_drops() {
    uint64_t seq = i8042_char_publish_seq.load(atomic_memory_order::acquire);
    if (seq == 0) return;
    uint64_t start = (seq > i8042_char_buffer_max_size) ? seq - i8042_char_buffer_max_size : 0;
    for (uint64_t s = start; s < seq; s++) {
        if (s >= i8042_char_get_publish_seq()) break;
        kbd_char_event ev;
        if (!i8042_char_read_event_by_seq(s, &ev)) continue;
        if (ev.flags & kbd_char_flag_drop_hint) {
            record_drop(0, ev.ch, 0);
        }
    }
}

// ── kbdstatus ─────────────────────────────────────────────────
KURD_t cmd_kbdstatus(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    // 解析模式参数
    enum { BRIEF, NORMAL, FULL } mode = NORMAL;
    if (line->token_count >= 2) {
        if (token_eq(line->tokens[1], "brief"))  mode = BRIEF;
        else if (token_eq(line->tokens[1], "full")) mode = FULL;
        // "normal" or unknown → keep default
    }

    uint64_t pub      = i8042_event_publish_seq.load(atomic_memory_order::acquire);
    uint64_t ovf      = i8042_ring_overflow_count.load(atomic_memory_order::acquire);
    uint64_t err_ct   = i8042_parser_error_count.load(atomic_memory_order::acquire);
    uint64_t gen_ct   = i8042_event_generated_count.load(atomic_memory_order::acquire);
    uint64_t char_pub = i8042_char_publish_seq.load(atomic_memory_order::acquire);
    uint64_t char_drp = i8042_char_drop_count.load(atomic_memory_order::acquire);

    bsp_kout << "=== Keyboard System Status ===" << kendl;

    // 事件环形缓冲区
    bsp_kout << "Event Ring:" << kendl;
    bsp_kout << "  Published:       " << pub << kendl;
    bsp_kout << "  Capacity:        " << i8042_buffer_max_size << kendl;
    bsp_kout << "  Overflow events: " << ovf << kendl;
    bsp_kout << "  Parser errors:   " << err_ct << kendl;

    if (mode != BRIEF) {
        uint64_t consumer_window = (pub > i8042_buffer_max_size) ? i8042_buffer_max_size : pub;
        uint64_t utilization = (consumer_window * 100) / i8042_buffer_max_size;
        bsp_kout << "  Utilization:     ~" << utilization << "%" << kendl;
    }

    if (mode == FULL || mode == NORMAL) {
        bsp_kout << "Char Buffer:" << kendl;
        bsp_kout << "  Published:       " << char_pub << kendl;
        bsp_kout << "  Capacity:        " << i8042_char_buffer_max_size << kendl;
        bsp_kout << "  Drops:           " << char_drp << kendl;
    }

    // 订阅者队列状态
    bsp_kout << "Subscriber Queues:" << kendl;
    bsp_kout << "  scancode:        " << (i8042_scancode_buffer_subscriber_queue ? "active" : "(null)") << kendl;
    bsp_kout << "  analyzed:        " << (i8042_analyzed_buffer_subscriber_queue ? "active" : "(null)") << kendl;
    bsp_kout << "  char:            " << (i8042_char_buffer_subscriber_queue ? "active" : "(null)") << kendl;

    // 健康度评估
    bsp_kout << "Health Assessment: ";
    if (ovf == 0 && err_ct < 10 && char_drp < 10) {
        bsp_kout << "Healthy" << kendl;
    } else if (ovf > 0 || err_ct >= 50 || char_drp >= 50) {
        bsp_kout << "CRITICAL" << kendl;
    } else {
        bsp_kout << "Warning" << kendl;
    }
    if (ovf  > 0) bsp_kout << "  [!] Ring overflow detected" << kendl;
    if (err_ct >= 50) bsp_kout << "  [!] High parser error count (" << err_ct << ")" << kendl;
    if (char_drp >= 50) bsp_kout << "  [!] High char drop count (" << char_drp << ")" << kendl;

    bsp_kout << "Total events generated: " << gen_ct << kendl;
    return ok;
}

// ── kbdevents ─────────────────────────────────────────────────
KURD_t cmd_kbdevents(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    enum { SUMMARY, RATE, ERRORS } mode = SUMMARY;
    if (line->token_count >= 2) {
        if (token_eq(line->tokens[1], "rate"))   mode = RATE;
        else if (token_eq(line->tokens[1], "errors")) mode = ERRORS;
    }

    uint64_t pub  = i8042_event_publish_seq.load(atomic_memory_order::acquire);
    uint64_t ovf  = i8042_ring_overflow_count.load(atomic_memory_order::acquire);
    uint64_t perr = i8042_parser_error_count.load(atomic_memory_order::acquire);
    uint64_t gen  = i8042_event_generated_count.load(atomic_memory_order::acquire);

    if (mode == SUMMARY) {
        bsp_kout << "=== Event Summary ===" << kendl;
        bsp_kout << "Total events:     " << pub << kendl;
        bsp_kout << "Generated:        " << gen << kendl;
        bsp_kout << "Overflows:        " << ovf << kendl;
        bsp_kout << "Parser errors:    " << perr << kendl;
        bsp_kout << "Valid events:     " << (pub - ovf - perr) << kendl;
    } else if (mode == RATE) {
        bsp_kout << "=== Event Rate ===" << kendl;
        if (pub == 0) {
            bsp_kout << "(no events)" << kendl;
            return ok;
        }
        // 获取最早和最新事件的 timestamp 计算时间跨度
        uint64_t first_ts = 0, last_ts = 0;
        ps_2_keyboard_event first_ev, last_ev;
        if (get_event(0, &first_ev)) first_ts = first_ev.timestamp_us;
        if (pub > 0 && get_event(pub - 1, &last_ev)) last_ts = last_ev.timestamp_us;

        uint64_t span_us = last_ts > first_ts ? last_ts - first_ts : 1;
        // 总事件率
        uint64_t total_rate_hz = (span_us > 0) ? (pub * 1000000ULL) / span_us : 0;
        bsp_kout << "Time span:        " << span_us / 1000 << " ms" << kendl;
        bsp_kout << "Total rate:       " << total_rate_hz << " events/s" << kendl;

        // 最近最多 1000 个事件的瞬时速率
        uint64_t recent_start = (pub > 1000) ? pub - 1000 : 0;
        uint64_t recent_count = pub - recent_start;
        uint64_t recent_first_ts = 0, recent_last_ts = 0;
        ps_2_keyboard_event rf, rl;
        if (get_event(recent_start, &rf)) recent_first_ts = rf.timestamp_us;
        if (pub > 0 && get_event(pub - 1, &rl)) recent_last_ts = rl.timestamp_us;
        uint64_t recent_span = recent_last_ts > recent_first_ts ? recent_last_ts - recent_first_ts : 1;
        bsp_kout << "Recent events:    " << recent_count << kendl;
        bsp_kout << "Inst. rate:       " << uint64_t((recent_count * 1000000ULL) / recent_span) << " events/s" << kendl;

        // 最大突发（events/10ms window）
        uint64_t max_burst = 0;
        const uint64_t window_us = 10000; // 10ms
        for (uint64_t s = recent_start; s < pub; ) {
            uint64_t window_end = s;
            uint64_t burst = 0;
            ps_2_keyboard_event anchor;
            if (!get_event(s, &anchor)) { s++; continue; }
            uint64_t win_limit = anchor.timestamp_us + window_us;
            while (window_end < pub) {
                ps_2_keyboard_event e;
                if (!get_event(window_end, &e)) { window_end++; continue; }
                if (e.timestamp_us > win_limit) break;
                burst++;
                window_end++;
            }
            if (burst > max_burst) max_burst = burst;
            s = window_end > s + 1 ? window_end : s + 1;
        }
        bsp_kout << "Max burst:        " << max_burst << " events/10ms" << kendl;

    } else if (mode == ERRORS) {
        bsp_kout << "=== Recent Errors ===" << kendl;
        if (pub == 0) { bsp_kout << "(no events)" << kendl; return ok; }
        uint64_t start = (pub > 5) ? pub - 5 : 0;
        // 反向扫描找最近的 error 事件
        uint64_t found = 0;
        for (uint64_t s = pub - 1; s >= start && s < pub; s--) {
            ps_2_keyboard_event ev;
            if (!get_event(s, &ev)) continue;
            if (ev.action == static_cast<uint8_t>(ps2_key_action::error)) {
                bsp_kout << "  #" << found << "  Seq=" << s;
                bsp_kout << "  ts=" << ev.timestamp_us / 1000 << "ms";
                bsp_kout << "  flags=" << HEX << ev.flags << DEC;
                bsp_kout << "  key=0x" << HEX << ev.key_code << DEC;
                bsp_kout << kendl;
                found++;
                if (found >= 5) break;
            }
        }
        if (found == 0) bsp_kout << "(no errors)" << kendl;
    }
    return ok;
}

// ── kbdchars ─────────────────────────────────────────────────
KURD_t cmd_kbdchars(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    enum { SUMMARY, DISTRIBUTION, DROPS } mode = SUMMARY;
    if (line->token_count >= 2) {
        if (token_eq(line->tokens[1], "distribution")) mode = DISTRIBUTION;
        else if (token_eq(line->tokens[1], "drops"))    mode = DROPS;
    }

    uint64_t pub    = i8042_char_publish_seq.load(atomic_memory_order::acquire);
    uint64_t drop   = i8042_char_drop_count.load(atomic_memory_order::acquire);

    if (mode == SUMMARY) {
        bsp_kout << "=== Char Buffer Summary ===" << kendl;
        bsp_kout << "Published chars:  " << pub << kendl;
        bsp_kout << "Capacity:         " << i8042_char_buffer_max_size << kendl;
        bsp_kout << "Drop count:       " << drop << kendl;
        // 有效字符计数（忽略重复/回退等）
        uint64_t valid = 0;
        uint64_t start = (pub > i8042_char_buffer_max_size) ? pub - i8042_char_buffer_max_size : 0;
        for (uint64_t s = start; s < pub; s++) {
            if (s >= i8042_char_get_publish_seq()) break;
            kbd_char_event ev;
            if (!i8042_char_read_event_by_seq(s, &ev)) continue;
            if (ev.ch >= 0x20 || ev.ch == '\n' || ev.ch == '\t' || ev.ch == '\b') valid++;
        }
        bsp_kout << "Valid chars:      " << valid << kendl;

    } else if (mode == DISTRIBUTION) {
        bsp_kout << "=== Char Distribution (Top 20) ===" << kendl;
        // 扫描 char 环，统计字符频率
        int freq[128] = {0};
        uint64_t start = (pub > i8042_char_buffer_max_size) ? pub - i8042_char_buffer_max_size : 0;
        for (uint64_t s = start; s < pub; s++) {
            if (s >= i8042_char_get_publish_seq()) break;
            kbd_char_event ev;
            if (!i8042_char_read_event_by_seq(s, &ev)) continue;
            uint8_t c = static_cast<uint8_t>(ev.ch);
            if (c < 128) freq[c]++;
        }
        // 找出 Top 20
        struct cf { char ch; int count; };
        cf top[20] = {};
        for (int c = 0; c < 128; c++) {
            if (freq[c] == 0) continue;
            int cnt = freq[c];
            int insert_at = -1;
            for (int j = 0; j < 20; j++) {
                if (cnt > top[j].count) { insert_at = j; break; }
            }
            if (insert_at >= 0) {
                for (int j = 19; j > insert_at; j--) top[j] = top[j-1];
                top[insert_at].ch = static_cast<char>(c);
                top[insert_at].count = cnt;
            }
        }
        for (int i = 0; i < 20 && top[i].count > 0; i++) {
            char ch = top[i].ch;
            bsp_kout << "  ";
            if (ch >= 0x20) bsp_kout << ch;
            else if (ch == '\n') bsp_kout << "\\n";
            else if (ch == '\t') bsp_kout << "\\t";
            else if (ch == '\b') bsp_kout << "\\b";
            else { bsp_kout << "0x" << HEX << (uint8_t)ch << DEC; }
            bsp_kout << "  " << top[i].count << "  ";
            // 简易 ASCII 条状图（每格 = ~max/60）
            int bar_max = 60;
            int bar_len = (top[0].count > 0) ? (top[i].count * bar_max) / top[0].count : 0;
            if (bar_len < 1 && top[i].count > 0) bar_len = 1;
            for (int b = 0; b < bar_len; b++) bsp_kout << "#";
            bsp_kout << kendl;
        }

    } else if (mode == DROPS) {
        bsp_kout << "=== Recent Char Drops ===" << kendl;
        scan_char_ring_for_drops();
        uint8_t cnt = g_drop_count > DROP_RECORD_DEPTH ? DROP_RECORD_DEPTH : g_drop_count;
        if (cnt == 0) {
            bsp_kout << "(no drops recorded)" << kendl;
        } else {
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t idx = (g_drop_idx + DROP_RECORD_DEPTH - cnt + i) % DROP_RECORD_DEPTH;
                const auto& r = g_drop_records[idx];
                bsp_kout << "  #" << i << "  ts=" << r.timestamp_us;
                bsp_kout << "  char=";
                if (r.ch >= 0x20) bsp_kout << r.ch;
                else bsp_kout << "0x" << HEX << (uint8_t)r.ch << DEC;
                bsp_kout << "  reason=" << (r.reason == 0 ? "ring_overflow" : "unknown");
                bsp_kout << kendl;
            }
        }
    }
    return ok;
}

// ── kbdmodifiers ─────────────────────────────────────────────
KURD_t cmd_kbdmodifiers(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    enum { CURRENT, HISTORY, MAP } mode = CURRENT;
    if (line->token_count >= 2) {
        if (token_eq(line->tokens[1], "history")) mode = HISTORY;
        else if (token_eq(line->tokens[1], "map"))  mode = MAP;
    }

    // 获取最新事件以读取当前修饰键
    uint8_t mod = 0;
    uint64_t last_seq = i8042_get_publish_seq();
    if (last_seq > 0) {
        ps_2_keyboard_event ev;
        if (get_event(last_seq - 1, &ev)) mod = ev.modifiers;
    }

    if (mode == CURRENT) {
        bsp_kout << "=== Current Modifier State ===" << kendl;
        print_modifier_summary(mod);
    } else if (mode == HISTORY) {
        bsp_kout << "=== Modifier History (last " << MOD_HISTORY_DEPTH << " changes) ===" << kendl;
        // 扫描事件环重建历史（触发 mbaybe_record_mod_change）
        uint8_t dummy;
        scan_event_ring_for_history(&dummy);

        uint8_t cnt = g_mod_history_count > MOD_HISTORY_DEPTH ? MOD_HISTORY_DEPTH : g_mod_history_count;
        uint8_t start_idx = (g_mod_history_count >= MOD_HISTORY_DEPTH) ?
            g_mod_history_idx : 0;
        if (cnt == 0) {
            bsp_kout << "(no modifier changes recorded)" << kendl;
        } else {
            for (uint8_t i = 0; i < cnt; i++) {
                uint8_t idx = (start_idx + i) % MOD_HISTORY_DEPTH;
                const auto& r = g_mod_history[idx];
                bsp_kout << "  #" << i << "  key=0x" << HEX << r.cause_key << DEC;
                bsp_kout << "  " << (r.is_break ? "BREAK" : "MAKE");
                bsp_kout << "  " << r.timestamp_us / 1000 << "ms";
                bsp_kout << "  old=0x" << HEX << r.old_mod << DEC;
                bsp_kout << "  new=0x" << HEX << r.new_mod << DEC;
                bsp_kout << kendl;
            }
        }
        bsp_kout << kendl;
        bsp_kout << "Current: ";
        print_modifier_summary(mod);
    } else if (mode == MAP) {
        bsp_kout << "=== Modifier Bit Map ===" << kendl;
        bsp_kout << "  Bit 0: Shift        (0x01)" << kendl;
        bsp_kout << "  Bit 1: Ctrl         (0x02)" << kendl;
        bsp_kout << "  Bit 2: Alt          (0x04)" << kendl;
        bsp_kout << "  Bit 3: Caps Lock    (0x08)" << kendl;
        bsp_kout << "  Bit 4: Num Lock     (0x10)" << kendl;
        bsp_kout << "  Bit 5: Scroll Lock  (0x20)" << kendl;
        bsp_kout << "  Bits 6-7: reserved" << kendl;
        bsp_kout << kendl;
        bsp_kout << "Raw value: 0x" << HEX << mod << DEC << kendl;
    }
    return ok;
}

// ── KDB ───────────────────────────────────────────────────────
KURD_t cmd_kdb(const line_t* line) {
    (void)line;
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    uint8_t mod = 0;
    uint64_t last_seq = i8042_get_publish_seq();
    if (last_seq > 0) {
        ps_2_keyboard_event ev;
        if (get_event(last_seq - 1, &ev)) mod = ev.modifiers;
    }

    bsp_kout << kendl;
    bsp_kout << "Caps Lock:   " << ((mod & MOD_CAPS)   ? "ON" : "OFF") << kendl;
    bsp_kout << "Num Lock:    " << ((mod & MOD_NUM)    ? "ON" : "OFF") << kendl;
    bsp_kout << "Scroll Lock: " << ((mod & MOD_SCROLL) ? "ON" : "OFF") << kendl;
    bsp_kout << kendl;
    bsp_kout << "Note: LED indicators are NOT maintained by the system" << kendl;
    return ok;
}

// ── kbdsubscribers ───────────────────────────────────────────
KURD_t cmd_kbdsubscribers(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    enum { ALL, SCANCODE, ANALYZED, CHAR } which = ALL;
    if (line->token_count >= 2) {
        if (token_eq(line->tokens[1], "scancode")) which = SCANCODE;
        else if (token_eq(line->tokens[1], "analyzed")) which = ANALYZED;
        else if (token_eq(line->tokens[1], "char"))    which = CHAR;
    }

    auto dump_queue = [](const char* name, block_queue* q) {
        bsp_kout << "--- " << name << " ---" << kendl;
        if (!q) {
            bsp_kout << "  (null)" << kendl;
            return;
        }
        spinlock_interrupt_about_guard g(q->qlock);
        bsp_kout << "  Active: " << (q->is_queue_ready() ? "no" : "yes") << kendl;
    };

    bsp_kout << "=== Subscriber Queue Status ===" << kendl;
    if (which == ALL || which == SCANCODE)
        dump_queue("scancode", i8042_scancode_buffer_subscriber_queue);
    if (which == ALL || which == ANALYZED)
        dump_queue("analyzed", i8042_analyzed_buffer_subscriber_queue);
    if (which == ALL || which == CHAR)
        dump_queue("char", i8042_char_buffer_subscriber_queue);

    return ok;
}

// ── kbdmonitor ───────────────────────────────────────────────
KURD_t cmd_kbdmonitor(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    enum { MON_EVENTS, MON_CHARS, MON_BOTH } mon = MON_EVENTS;
    uint64_t max_count = 10;
    bool infinite = false;

    for (uint16_t i = 1; i < line->token_count; i++) {
        if (token_eq(line->tokens[i], "events")) mon = MON_EVENTS;
        else if (token_eq(line->tokens[i], "chars"))  mon = MON_CHARS;
        else if (token_eq(line->tokens[i], "both"))   mon = MON_BOTH;
        else {
            // try to parse as number (count) — 使用框架 token_to_uint64
            uint64_t val;
            if (token_to_uint64(line->tokens[i], &val)) {
                max_count = val;
                if (max_count == 0) infinite = true;
            }
        }
    }

    uint64_t events_shown = 0;
    uint64_t ev_seq = i8042_get_publish_seq();
    uint64_t ch_seq = i8042_char_get_publish_seq();

    bsp_kout << "=== Monitor ===";
    if (mon == MON_EVENTS) bsp_kout << " (events mode)";
    else if (mon == MON_CHARS) bsp_kout << " (chars mode)";
    else bsp_kout << " (both mode)";
    if (infinite) bsp_kout << " [Press ESC to stop]";
    else bsp_kout << " [count=" << max_count << "]";
    bsp_kout << kendl;
    bsp_kout << "Seq\tKey/Action/Modifiers/Timestamp" << kendl;

    while (true) {
        bool advanced = false;
        // 轮询事件
        if (mon == MON_EVENTS || mon == MON_BOTH) {
            uint64_t cur = i8042_get_publish_seq();
            while (ev_seq < cur) {
                ps_2_keyboard_event ev;
                if (get_event(ev_seq, &ev)) {
                    bsp_kout << ev_seq << "\t"
                             << "key=0x" << HEX << ev.key_code << DEC
                             << " " << action_str(ev.action)
                             << " mod=0x" << HEX << ev.modifiers << DEC
                             << " ts=" << ev.timestamp_us/1000 << "ms"
                             << kendl;
                    // 检查 ESC (key_code 0x01, make)
                    if (ev.key_code == 0x01 &&
                        ev.action == static_cast<uint8_t>(ps2_key_action::make)) {
                        bsp_kout << "[Monitor] ESC pressed, stopping." << kendl;
                        return ok;
                    }
                    events_shown++;
                    advanced = true;
                }
                ev_seq++;
                if (!infinite && events_shown >= max_count) {
                    bsp_kout << "[Monitor] " << events_shown << " events shown." << kendl;
                    return ok;
                }
            }
        }
        // 轮询字符
        if (mon == MON_CHARS || mon == MON_BOTH) {
            uint64_t cur = i8042_char_get_publish_seq();
            while (ch_seq < cur) {
                kbd_char_event cev;
                if (i8042_char_read_event_by_seq(ch_seq, &cev)) {
                    bsp_kout << "ch[" << ch_seq << "]\t"
                             << "char=";
                    if (cev.ch >= 0x20) bsp_kout << cev.ch;
                    else if (cev.ch == '\n') bsp_kout << "\\n";
                    else if (cev.ch == '\t') bsp_kout << "\\t";
                    else if (cev.ch == '\b') bsp_kout << "\\b";
                    else bsp_kout << "0x" << HEX << (uint8_t)cev.ch << DEC;
                    bsp_kout << " flags=0x" << HEX << cev.flags << DEC;
                    bsp_kout << " src=0x" << HEX << cev.source_key << DEC;
                    bsp_kout << kendl;
                    events_shown++;
                    advanced = true;
                }
                ch_seq++;
                if (!infinite && events_shown >= max_count) {
                    bsp_kout << "[Monitor] " << events_shown << " events shown." << kendl;
                    return ok;
                }
            }
        }
        if (!advanced) {
            // 小睡 1ms 避免忙等
            kthread_sleep(1000);
            // 工作中断退出：轮询中检查新事件的 ESC 已涵盖
        }
    }
}

// ── kbdtest ───────────────────────────────────────────────────
KURD_t cmd_kbdtest(const line_t* line) {
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    uint64_t duration_sec = 10;
    bool infinite = false;
    if (line->token_count >= 2) {
        uint64_t val;
        if (token_to_uint64(line->tokens[1], &val)) {
            duration_sec = val;
            if (duration_sec == 0) infinite = true;
        }
    }

    bsp_kout << "=== Interactive Test ===" << kendl;
    if (infinite) bsp_kout << "[Press ESC to stop]" << kendl;
    else bsp_kout << "[Duration: " << duration_sec << "s, or press ESC]" << kendl;
    bsp_kout << "Key\tAction\tModifiers\tChar" << kendl;

    uint64_t ev_seq = i8042_get_publish_seq();
    uint64_t start_us = ktime::get_microsecond_stamp();
    uint64_t end_us = infinite ? ~0ULL : start_us + duration_sec * 1000000ULL;
    uint64_t total_keys = 0, total_chars = 0, total_errors = 0;

    while (ktime::get_microsecond_stamp() < end_us) {
        uint64_t cur = i8042_get_publish_seq();
        bool advanced = false;
        while (ev_seq < cur) {
            ps_2_keyboard_event ev;
            if (get_event(ev_seq, &ev)) {
                total_keys++;
                bsp_kout << "0x" << HEX << ev.key_code << DEC
                         << "\t" << action_str(ev.action)
                         << "\t0x" << HEX << ev.modifiers << DEC;

                // 查找对应的字符输出
                kbd_char_event c_ev;
                bool has_char = false;
                uint64_t ch_cur = i8042_char_get_publish_seq();
                for (uint64_t cs = 0; cs < ch_cur; cs++) {
                    if (i8042_char_read_event_by_seq(cs, &c_ev) &&
                        c_ev.decisive_event_seq == ev_seq) {
                        has_char = true;
                        break;
                    }
                }
                if (has_char) {
                    bsp_kout << "\t";
                    if (c_ev.ch >= 0x20 && c_ev.ch < 0x7F) {
                        bsp_kout << c_ev.ch;
                    } else if (c_ev.ch == '\n') bsp_kout << "\\n";
                    else if (c_ev.ch == '\t') bsp_kout << "\\t";
                    else if (c_ev.ch == '\b') bsp_kout << "\\b";
                    else bsp_kout << "0x" << HEX << (uint8_t)c_ev.ch << DEC;
                    total_chars++;
                } else {
                    bsp_kout << "\t-";
                }

                if (ev.action == static_cast<uint8_t>(ps2_key_action::error)) {
                    total_errors++;
                    bsp_kout << " [ERROR]";
                }
                bsp_kout << kendl;
                advanced = true;

                // 检查 ESC
                if (ev.key_code == 0x01 &&
                    ev.action == static_cast<uint8_t>(ps2_key_action::make)) {
                    bsp_kout << "[Test] ESC pressed, stopping." << kendl;
                    goto test_summary;
                }
            }
            ev_seq++;
        }
        if (!advanced) kthread_sleep(1000);
    }

test_summary:
    bsp_kout << kendl;
    bsp_kout << "=== Test Summary ===" << kendl;
    bsp_kout << "Total keys:   " << total_keys << kendl;
    bsp_kout << "Total chars:  " << total_chars << kendl;
    bsp_kout << "Total errors: " << total_errors << kendl;
    uint64_t elapsed = ktime::get_microsecond_stamp() - start_us;
    if (elapsed > 0) {
        bsp_kout << "Elapsed:     " << elapsed / 1000 << " ms" << kendl;
        bsp_kout << "Avg rate:    " <<uint64_t( (total_keys * 1000000ULL) / elapsed) << " keys/s" << kendl;
    }
    return ok;
}

// ── i8042regs ─────────────────────────────────────────────────
KURD_t cmd_i8042regs(const line_t* line) {
    (void)line;
    KURD_t ok = make_success();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    // 读状态寄存器 (0x64)
    uint8_t status = inb(0x64);
    bsp_kout << "=== i8042 Status Register (port 0x64) ===" << kendl;
    bsp_kout << "  Raw:  0x" << HEX << status << DEC << kendl;
    bsp_kout << "  Bit 0 (OBF):        " << ((status & 0x01) ? "1 - Output buffer full" : "0 - Empty") << kendl;
    bsp_kout << "  Bit 1 (IBF):        " << ((status & 0x02) ? "1 - Input buffer full"  : "0 - Empty") << kendl;
    bsp_kout << "  Bit 2 (SYS):        " << ((status & 0x04) ? "1 - System flag set"    : "0 - Clear") << kendl;
    bsp_kout << "  Bit 3 (A2):         " << ((status & 0x08) ? "1 - Data=command"       : "0 - Data=data") << kendl;
    bsp_kout << "  Bit 4 (INH):        " << ((status & 0x10) ? "1 - Keyboard inhibited" : "0 - Enabled") << kendl;
    bsp_kout << "  Bit 5 (TOUT):       " << ((status & 0x20) ? "1 - Transmit timeout"   : "0 - OK") << kendl;
    bsp_kout << "  Bit 6 (ERR):        " << ((status & 0x40) ? "1 - Receive timeout"    : "0 - OK") << kendl;
    bsp_kout << "  Bit 7 (PERR):       " << ((status & 0x80) ? "1 - Parity error"       : "0 - OK") << kendl;
    bsp_kout << kendl;

    // 读取控制寄存器（通过命令 0x20）
    while (inb(0x64) & 0x02);  // 等待 IBF 清空
    outb(0x64, 0x20);
    while (!(inb(0x64) & 0x01)); // 等待 OBF 就绪
    uint8_t control = inb(0x60);

    bsp_kout << "=== i8042 Control Register (command 0x20) ===" << kendl;
    bsp_kout << "  Raw:  0x" << HEX << control << DEC << kendl;
    bsp_kout << "  Bit 0 (KBIE):       " << ((control & 0x01) ? "1 - Keyboard IRQ enabled"   : "0 - Disabled") << kendl;
    bsp_kout << "  Bit 1 (MIE):        " << ((control & 0x02) ? "1 - Mouse IRQ enabled"      : "0 - Disabled") << kendl;
    bsp_kout << "  Bit 2 (SYS):        " << ((control & 0x04) ? "1 - System flag set"        : "0 - Clear") << kendl;
    bsp_kout << "  Bit 3 (ZERO):       " << ((control & 0x08) ? "1 (reserved, should be 0)"  : "0 (OK)") << kendl;
    bsp_kout << "  Bit 4 (KBD):        " << ((control & 0x10) ? "1 - Keyboard disabled"      : "0 - Enabled") << kendl;
    bsp_kout << "  Bit 5 (MOUSE):      " << ((control & 0x20) ? "1 - Mouse disabled"         : "0 - Enabled") << kendl;
    bsp_kout << "  Bit 6 (XLAT):       " << ((control & 0x40) ? "1 - Set2->Set1 translation ON" : "0 - OFF") << kendl;
    bsp_kout << "  Bit 7 (RES):        " << ((control & 0x80) ? "1 (reserved)"               : "0") << kendl;

    return ok;
}
