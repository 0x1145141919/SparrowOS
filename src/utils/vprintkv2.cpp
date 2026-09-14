// ════════════════════════════════════════════════════════════════
// vprintkv2.cpp — 无状态纯内存格式化核心（freestanding，零外部依赖）
//
// 这一份 TU 是双方（kernel.elf / init.elf）与宿主【共用】的唯一 formatter：
//   - 不 include lock.h / abi / 任何系统头；-ffreestanding 编译后无未定义符号；
//   - 支持 %c %s %d %i %u %o %x %X %b(扩展) %p %% + flags(- + 空格 # 0)/width/precision/length；
//   - 【不支持】float → 标记且不 va_arg（绝不碰 XMM/FPU）。
// ════════════════════════════════════════════════════════════════
#include "util/vprintkv2.h"

namespace klog
{

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
};

// 数字转串（低位在前，返回长度）。base ∈ {2,8,10,16}。
uint32_t u_to_base(uint64_t v, unsigned base, bool upper, char* tmp)
{
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint32_t i = 0;
    if (v == 0) { tmp[i++] = '0'; return i; }   // caller 负责 precision-0 特殊处理
    while (v) { tmp[i++] = d[v % base]; v /= base; }
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

struct format_result_t { uint64_t written; uint64_t total; bool overflow; };

format_result_t format_core(char* out, uint64_t cap, const char* fmt, va_list ap)
{
    out_t o{out, cap, 0};
    if (!fmt) { if (out && cap > 0) out[0] = '\0'; return {0, 0, false}; }

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
        const char* lmod = "";                       // 原样保留，用于诊断标记
        int len_mod = 0;   // 0=int/uint, 1=long/ll/size/intmax (LP64 下统一 64 位)
        if (*p == 'h') { lmod = "h"; ++p; if (*p == 'h') { lmod = "hh"; ++p; } }
        else if (*p == 'l') { lmod = "l"; ++p; if (*p == 'l') { lmod = "ll"; ++p; } len_mod = 1; }
        else if (*p == 'z') { lmod = "z"; ++p; len_mod = 1; }
        else if (*p == 'j') { lmod = "j"; ++p; len_mod = 1; }
        else if (*p == 't') { lmod = "t"; ++p; len_mod = 1; }
        else if (*p == 'L') { lmod = "L"; ++p; }

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

            // 浮点：[不实现]（对齐 Linux printk）。显式标记，且【不】va_arg
            //   —— 本 formatter 绝不碰 XMM / FPU，保证 IRQ/early/panic 上下文安全。
            case 'f': case 'F': case 'e': case 'E':
            case 'g': case 'G': case 'a': case 'A':
            {
                o.put('<'); o.put('%');
                for (const char* q = lmod; *q; ++q) o.put(*q);
                o.put(c);
                for (const char* q = "? unsupported>"; *q; ++q) o.put(*q);
                break;
            }

            default:
                // 未知转换：显式标记，别静默吐字面量（易被误读为“打出来了”）
            {
                o.put('<'); o.put('%');
                for (const char* q = lmod; *q; ++q) o.put(*q);
                o.put(c);
                for (const char* q = "? unknown>"; *q; ++q) o.put(*q);
                break;
            }
        }
    }

    uint64_t written = 0;
    bool overflow = (o.n > 0);
    if (out && cap > 0)
    {
        written = (o.n < cap - 1) ? o.n : cap - 1;
        out[written] = '\0';
        overflow = (o.n >= cap);
    }
    return {written, o.n, overflow};
}

}  // anonymous namespace

uint64_t vprintkv2(const char* fmt, va_list ap, void* out_buff, uint32_t max_limit)
{
    format_result_t r = format_core((char*)out_buff, (uint64_t)max_limit, fmt, ap);
    uint64_t packed = (r.written & 0xFFFFFFFFull);
    if (r.overflow) packed |= (1ull << 63);
    return packed;
}

}  // namespace klog
