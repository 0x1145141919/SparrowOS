// ════════════════════════════════════════════════════════════════
// stub_kout.cpp — 用户态测试用的 kout 垫片
//
// 提供 bsp_kout 符号和必要的 operator<< 实现
// 所有输出重定向到 stderr
// ════════════════════════════════════════════════════════════════

#include "util/kout.h"
#include <cstdio>
#include <cstdarg>
#include <unistd.h>

// 简单缓存
static char stub_buf[4096];
static size_t stub_pos = 0;

// kout 本身的方法实现
kio::kout::kout() : calls_str(0), calls_char(0), calls_ptr(0),
    calls_uint64(0), calls_int64(0), calls_uint32(0), calls_int32(0),
    calls_uint16(0), calls_int16(0), calls_uint8(0), calls_int8(0),
    calls_nowtime(0), calls_endl(0), calls_radix(0),
    numer_system(DEC), is_silent_this_line(false), is_nonewline(false) {}

kio::kout::~kout() { if (!is_nonewline) { write(2, "\n", 1); } }

void kio::kout::Init() {}
void kio::kout::shift_dec() { numer_system = DEC; }

void kio::kout::flush_to_backend(const char* s, size_t len) {
    write(2, s, len);
}

kio::kout& kio::kout::operator<<(const char* str) {
    if (str) write(2, str, __builtin_strlen(str));
    return *this;
}

kio::kout& kio::kout::operator<<(char c) {
    write(2, &c, 1);
    return *this;
}

kio::kout& kio::kout::operator<<(const void* ptr) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ptr);
    if (n > 0) write(2, buf, n);
    return *this;
}

kio::kout& kio::kout::operator<<(uint64_t num) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lu", (unsigned long)num);
    if (n > 0) write(2, buf, n);
    return *this;
}

kio::kout& kio::kout::operator<<(int64_t num) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld", (long)num);
    if (n > 0) write(2, buf, n);
    return *this;
}

kio::kout& kio::kout::operator<<(uint32_t num) {
    return operator<<((uint64_t)num);
}

kio::kout& kio::kout::operator<<(int32_t num) {
    return operator<<((int64_t)num);
}

kio::kout& kio::kout::operator<<(uint16_t num) {
    return operator<<((uint64_t)num);
}

kio::kout& kio::kout::operator<<(int16_t num) {
    return operator<<((int64_t)num);
}

kio::kout& kio::kout::operator<<(uint8_t num) {
    return operator<<((uint64_t)num);
}

kio::kout& kio::kout::operator<<(int8_t num) {
    return operator<<((int64_t)num);
}

kio::kout& kio::kout::operator<<(kio::endl) {
    write(2, "\n", 1);
    return *this;
}

kio::kout& kio::kout::operator<<(kio::numer_system_select radix) {
    numer_system = radix;
    return *this;
}

kio::kout& kio::kout::operator<<(kio::now_time) { return *this; }

kio::kout& kio::kout::operator,(const KURD_t&) { return *this; }

kio::kout bsp_kout;

// 其他 kout 相关符号
constexpr kio::numer_system_select DEC{10};
constexpr kio::numer_system_select HEX{16};
constexpr kio::now_time now{};
constexpr kio::endl kendl{};
