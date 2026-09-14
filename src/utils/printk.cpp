// ════════════════════════════════════════════════════════════════
// printk.cpp — kernel.elf 的日志面层（sink + 前缀 + 面层）
//
// 职责边界（刻意收窄）：
//   · 纯格式化核心 vprintkv2 已独立成 TU：src/utils/vprintkv2.cpp（freestanding，双向共用）；
//   · 本文件只管 kernel.elf 特有的事：level/ts 元数据、单 sink 落地方言、前缀渲染、面层宏。
//
// 唯一外部依赖：now_ts_us()（extern "C"，谁接入谁提供）与 lock.h 的自旋锁。
// init.elf 走自己的面层（src/init/util/printk.cpp），与本文无耦合。
// ════════════════════════════════════════════════════════════════
#include "util/printk.h"

namespace klog
{

// ─────────────────────────── 内部工具 ───────────────────────────

namespace
{

const char* level_name(level_t l)
{
    switch (l)
    {
        case level_code::INFO:    return "INFO";
        case level_code::NOTICE:  return "NOTICE";
        case level_code::WARNING: return "WARN";
        case level_code::ERROR:   return "ERROR";
        case level_code::FATAL:   return "FATAL";
        default:                  return "?";
    }
}

// 十进制串（低位在前）便于前缀渲染复用
uint32_t u_to_dec(uint64_t v, char* tmp)
{
    uint32_t i = 0;
    if (v == 0) { tmp[i++] = '0'; return i; }
    while (v) { tmp[i++] = char('0' + v % 10); v /= 10; }
    return i;
}

}  // anonymous namespace

// ─────────────────────── render_prefix_plain ───────────────────────

uint32_t render_prefix_plain(void* self, char* buf, uint64_t cap,
                             level_t level, uint64_t ts_us)
{
    (void)self;
    char tmp[64];
    uint64_t n = 0;
    auto put  = [&](char c) { if (n < sizeof(tmp)) tmp[n] = c; ++n; };

    if (ts_us != 0)
    {
        put('[');
        uint32_t sec = (uint32_t)(ts_us / 1000000ull);
        uint32_t us  = (uint32_t)(ts_us % 1000000ull);

        char d[12]; uint32_t dl = u_to_dec(sec, d);           // 低位在前
        for (uint32_t i = dl; i < 6; ++i) put(' ');           // 秒右对齐宽 6
        for (uint32_t i = dl; i > 0; --i) put(d[i - 1]);
        put('.');
        char u[6];
        for (int i = 5; i >= 0; --i) { u[i] = char('0' + us % 10); us /= 10; }
        for (int i = 0; i < 6; ++i) put(u[i]);
        put(']'); put(' ');
    }

    const char* nm = level_name(level);
    uint32_t nl = 0;
    while (nm[nl]) { put(nm[nl]); ++nl; }
    for (uint32_t i = nl; i < 5; ++i) put(' ');               // 等级左对齐宽 5
    put(' ');

    uint64_t m = (n < cap) ? n : cap;
    for (uint64_t i = 0; i < m; ++i) buf[i] = tmp[i];
    return (uint32_t)m;
}

// ─────────────────────────── vprintk ───────────────────────────

void vprintk(level_t level, const log_sink* sink, const char* fmt, va_list ap)
{
    if (!sink || !sink->render_prefix || !sink->emit) return;

    char buf[LOG_LINE_MAX];
    uint64_t ts = now_ts_us();

    spinlock_interrupt_about_guard guard(*sink->sink_lock);

    uint64_t n = sink->render_prefix(sink->self, buf, LOG_LINE_MAX, level, ts);
    if (n > LOG_LINE_MAX) n = LOG_LINE_MAX;
    uint64_t room = LOG_LINE_MAX - n;

    uint64_t res = vprintkv2(fmt, ap, buf + n, (uint32_t)room);   // 核心：纯格式化
    uint32_t wb  = (uint32_t)(res & 0xFFFFFFFFu);                 // 实际写入字节数
    bool     ovf = (res >> 63) & 1u;                              // 截断标志

    uint64_t stored = 0;
    if (wb > 0 && room > 0)
    {
        stored = wb;
        if (ovf)   // 截断 → 尾部盖 "...[truncated]" 标记
        {
            static const char MK[] = "...[truncated]";
            const uint64_t mk = sizeof(MK) - 1;
            if (room > mk + 1)
            {
                stored = room - 1 - mk;
                for (uint64_t i = 0; i < mk; ++i) buf[n + stored + i] = MK[i];
                stored += mk;
            }
        }
    }

    sink->emit(sink->self, buf, n + stored);
}

// ─────────────────────────── 面层 ───────────────────────────

static const log_sink* g_default_sink = nullptr;

void set_default_sink(const log_sink* sink) { g_default_sink = sink; }
const log_sink* get_default_sink()          { return g_default_sink; }

void printk(level_t level, const char* fmt, ...)
{
    const log_sink* s = g_default_sink;
    if (!s) return;
    va_list ap;
    va_start(ap, fmt);
    vprintk(level, s, fmt, ap);
    va_end(ap);
}

void early_printk(const char* fmt, ...)
{
    const log_sink* s = g_default_sink;
    if (!s) return;
    va_list ap;
    va_start(ap, fmt);
    vprintk(level_code::INFO, s, fmt, ap);   // early 全部 INFO（薄包装）
    va_end(ap);
}

}  // namespace klog
