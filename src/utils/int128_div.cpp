#include "stdint.h"
// 实现128位整数除法函数，用于支持__uint128_t运算
//
// 策略：
//   除零不预检 — 让硬件 DIV 触发 #DE，内核态直接爆雷（bug 就该大声）
//   商超 64-bit 预检 — 若 n_hi >= d_lo，回退到软件 bit-loop
//   常见路径（128÷64，商 ≤ 2^64−1）→ 单条硬件 DIV
extern "C" {

unsigned __int128 __udivti3(unsigned __int128 n, unsigned __int128 d) {
    uint64_t d_lo = (uint64_t)d;
    uint64_t d_hi = (uint64_t)(d >> 64);
    uint64_t n_lo = (uint64_t)n;
    uint64_t n_hi = (uint64_t)(n >> 64);

    // 除数 > 64-bit → 走完整 bit-loop（罕见）
    if (d_hi != 0)
        goto fallback;

    // n 本身 ≤ 64-bit → 编译器优化为 64/64 DIV
    if (n_hi == 0)
        return n_lo / d_lo;

    // 硬件 DIV 路径（安全条件：商 ≤ 2^64−1，或 d_lo==0 → #DE）
    //   不检除零 — d_lo==0 时 DIV 触发 #DE，内核 fatal
    if (d_lo == 0 || n_hi < d_lo) {
        uint64_t q_lo;
        asm volatile("divq %3" : "=a"(q_lo), "=d"(n_hi) : "a"(n_lo), "r"(d_lo), "d"(n_hi));
        return (unsigned __int128)q_lo;
    }
    // n_hi >= d_lo && d_lo != 0 → 商 > 2^64−1，走 bit-loop
    goto fallback;

fallback:
    // 完整 128-bit 除法算法（逐位移位减法）
    unsigned __int128 q = 0;
    unsigned __int128 r = 0;
    for (int i = 127; i >= 0; i--) {
        r <<= 1;
        if ((n >> i) & 1)
            r |= 1;
        if (r >= d) {
            r -= d;
            q |= ((unsigned __int128)1 << i);
        }
    }
    return q;
}

unsigned __int128 __umodti3(unsigned __int128 n, unsigned __int128 d) {
    return n - (d * __udivti3(n, d));
}

}
