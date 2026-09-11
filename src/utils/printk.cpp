// ════════════════════════════════════════════════════════════════
// printk.cpp — klog 面层实现（宿主可编译 / 内核可编译）
//
// 纯逻辑：kvformat 格式化引擎 + render_prefix_plain + vprintk + 面层。
// 唯一的内部依赖是 now_ts_us()（extern "C"，谁接入谁提供）与 lock.h 的自旋锁。
// 目标：先在宿主铸好、测穿，再往内核搬（见 Docs 决策）。
// ════════════════════════════════════════════════════════════════
#include "util/printk.h"

namespace klog
{

// ─────────────────────────── 内部工具 ───────────────────────────

namespace
{

// 输出游标：始终记录「本该写出的长度」n；只在 n < cap-1 时真正落 buf。
// cap 含结尾 NUL 的空间。返回时 n 即「snprintf 式」的本该长度。
struct out_t
{
    char*    buf;
    uint64_t cap;   // 总容量（含 NUL 位）
    uint64_t n;     // 已经/本该写出的字符数（不含 NUL）

    void put(char c)
    {
        if (buf && cap > 0 && n < cap - 1) buf[n] = c;
        ++n;
    }
    void emit(const char* s, uint64_t len)
    {
        for (uint64_t i = 0; i < len; ++i) put(s[i]);
    }
    void emit_z(const char* s)
    {
        if (!s) s = "(null)";
        while (*s) put(*s++);
    }
};

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

// 数字转串（低位在前，返回长度）。base ∈ {2,8,10,16}。
uint32_t u_to_base(uint64_t v, unsigned base, bool upper, char* tmp)
{
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint32_t i = 0;
    if (v == 0) { tmp[i++] = '0'; return i; }   // caller 负责 precision-0 特殊处理
    while (v) { tmp[i++] = d[v % base]; v /= base; }
    return i;
}

// 十进制串（低位在前）便于前缀渲染复用
uint32_t u_to_dec(uint64_t v, char* tmp)
{
    uint32_t i = 0;
    if (v == 0) { tmp[i++] = '0'; return i; }
    while (v) { tmp[i++] = char('0' + v % 10); v /= 10; }
    return i;
}

// 整数格式化：统一处理 flags / width / precision / base
struct int_fmt_t
{
    bool     left      = false;  // '-'
    bool     plus      = false;  // '+'
    bool     space     = false;  // ' '
    bool     alt       = false;  // '#'
    bool     zero      = false;  // '0'
    int      width     = 0;
    int      prec      = -1;     // -1 = 未指定
    unsigned base      = 10;
    bool     upper     = false;
    bool     is_signed = false;  // 有符号转换（d/i）
};

void fmt_num(out_t& o, uint64_t uval, bool neg, const int_fmt_t& f)
{
    char digs[72];
    uint32_t dl = 0;
    // precision==0 且值为 0 → 空数字
    if (!(f.prec == 0 && uval == 0))
        dl = u_to_base(uval, f.base, f.upper, digs);

    uint32_t prec_zeros = (f.prec > (int)dl) ? (uint32_t)(f.prec - (int)dl) : 0;

    char pfx[4];
    uint32_t pl = 0;
    if (f.is_signed)
    {
        if (neg)        pfx[pl++] = '-';
        else if (f.plus) pfx[pl++] = '+';
        else if (f.space) pfx[pl++] = ' ';
    }
    if (f.alt)
    {
        if (f.base == 16 && dl != 0) { pfx[pl++] = '0'; pfx[pl++] = f.upper ? 'X' : 'x'; }
        else if (f.base == 2)        { pfx[pl++] = '0'; pfx[pl++] = 'b'; }
        else if (f.base == 8 && (dl == 0 || digs[dl - 1] != '0')) pfx[pl++] = '0';
    }

    uint32_t body  = prec_zeros + dl;
    uint32_t total = pl + body;
    uint32_t pad   = (f.width > (int)total) ? (uint32_t)(f.width - total) : 0;

    bool zero_pad = f.zero && !f.left && (f.prec < 0);   // 指定 precision 时 '0' 失效

    if (!f.left && !zero_pad) { for (uint32_t i = 0; i < pad; ++i) o.put(' '); }
    for (uint32_t i = 0; i < pl; ++i) o.put(pfx[i]);
    if (!f.left && zero_pad) { for (uint32_t i = 0; i < pad; ++i) o.put('0'); }
    for (uint32_t i = 0; i < prec_zeros; ++i) o.put('0');
    for (uint32_t i = dl; i > 0; --i) o.put(digs[i - 1]);
    if (f.left) { for (uint32_t i = 0; i < pad; ++i) o.put(' '); }
}

void fmt_str(out_t& o, const char* s, int width, int prec, bool left)
{
    if (!s) s = "(null)";
    uint64_t len = 0;
    while (s[len]) ++len;
    if (prec >= 0 && len > (uint64_t)prec) len = (uint64_t)prec;
    uint32_t pad = (width > (int)len) ? (uint32_t)(width - (int)len) : 0;
    if (!left) { for (uint32_t i = 0; i < pad; ++i) o.put(' '); }
    o.emit(s, len);
    if (left)  { for (uint32_t i = 0; i < pad; ++i) o.put(' '); }
}

}  // anonymous namespace

// ─────────────────────────── kvformat ───────────────────────────

int kvformat(char* out, uint64_t cap, const char* fmt, va_list ap)
{
    out_t o{out, cap, 0};
    if (!fmt) return 0;

    const char* p = fmt;
    while (*p)
    {
        if (*p != '%') { o.put(*p++); continue; }
        ++p;                                   // skip '%'

        // ---- flags ----
        int_fmt_t f;
        for (;;)
        {
            if (*p == '-')      f.left = true;
            else if (*p == '+') f.plus = true;
            else if (*p == ' ') f.space = true;
            else if (*p == '#') f.alt = true;
            else if (*p == '0') f.zero = true;
            else break;
            ++p;
        }

        // ---- width ----
        if (*p == '*') { f.width = va_arg(ap, int); ++p; if (f.width < 0) { f.left = true; f.width = -f.width; } }
        else while (*p >= '0' && *p <= '9') { f.width = f.width * 10 + (*p - '0'); ++p; }

        // ---- precision ----
        if (*p == '.')
        {
            ++p;
            f.prec = 0;
            if (*p == '*') { f.prec = va_arg(ap, int); ++p; if (f.prec < 0) f.prec = -1; }
            else while (*p >= '0' && *p <= '9') { f.prec = f.prec * 10 + (*p - '0'); ++p; }
        }

        // ---- length ----
        int len_mod = 0;   // 0=int/uint, 1=long/ll/size/intmax (LP64 下统一 64 位)
        if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        else if (*p == 'l') { ++p; len_mod = 1; if (*p == 'l') ++p; }
        else if (*p == 'z' || *p == 'j' || *p == 't') { ++p; len_mod = 1; }

        // ---- conversion ----
        char c = *p;
        if (!c) break;
        ++p;

        switch (c)
        {
            case '%': o.put('%'); break;

            case 'd':
            case 'i':
            {
                int64_t sv = (len_mod == 1) ? va_arg(ap, long long)
                                            : (int64_t)va_arg(ap, int);
                bool neg = sv < 0;
                uint64_t uv = neg ? (uint64_t)(-(sv + 1)) + 1 : (uint64_t)sv;  // 防 INT64_MIN
                f.is_signed = true; f.base = 10; f.upper = false;
                fmt_num(o, uv, neg, f);
                break;
            }
            case 'u':
            {
                uint64_t uv = (len_mod == 1) ? va_arg(ap, unsigned long long)
                                             : (uint64_t)va_arg(ap, unsigned int);
                f.base = 10;
                fmt_num(o, uv, false, f);
                break;
            }
            case 'x':
            case 'X':
            {
                uint64_t uv = (len_mod == 1) ? va_arg(ap, unsigned long long)
                                             : (uint64_t)va_arg(ap, unsigned int);
                f.base = 16; f.upper = (c == 'X');
                fmt_num(o, uv, false, f);
                break;
            }
            case 'o':
            {
                uint64_t uv = (len_mod == 1) ? va_arg(ap, unsigned long long)
                                             : (uint64_t)va_arg(ap, unsigned int);
                f.base = 8;
                fmt_num(o, uv, false, f);
                break;
            }
            case 'b':   // 扩展：二进制
            {
                uint64_t uv = (len_mod == 1) ? va_arg(ap, unsigned long long)
                                             : (uint64_t)va_arg(ap, unsigned int);
                f.base = 2;
                fmt_num(o, uv, false, f);
                break;
            }
            case 'p':   // 指针：0x + hex
            {
                uint64_t uv = (uint64_t)(uintptr_t)va_arg(ap, void*);
                f.base = 16; f.upper = false; f.alt = true; f.zero = f.zero && (f.prec < 0);
                fmt_num(o, uv, false, f);
                break;
            }
            case 'c':
            {
                char ch = (char)va_arg(ap, int);
                uint32_t pad = (f.width > 1) ? (uint32_t)(f.width - 1) : 0;
                if (!f.left) { for (uint32_t i = 0; i < pad; ++i) o.put(' '); }
                o.put(ch);
                if (f.left)  { for (uint32_t i = 0; i < pad; ++i) o.put(' '); }
                break;
            }
            case 's':
                fmt_str(o, va_arg(ap, const char*), f.width, f.prec, f.left);
                break;

            default:
                // 未知转换：原样吐出 "%<c>"，便于暴露 bug
                o.put('%');
                o.put(c);
                break;
        }
    }

    if (out && cap > 0)
    {
        uint64_t nul = (o.n < cap - 1) ? o.n : cap - 1;
        out[nul] = '\0';
    }
    return (int)o.n;   // snprintf 语义：本该长度；>= cap 即截断
}

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

    int wb = kvformat(buf + n, room, fmt, ap);
    uint64_t stored = 0;
    if (wb > 0 && room > 0)
    {
        uint64_t bl = (uint64_t)wb;
        stored = (bl < room) ? bl : (room - 1);
        if (bl >= room)   // 截断 → 尾部盖 "...[truncated]" 标记
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
