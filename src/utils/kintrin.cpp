#include "kintrin.h"
#ifdef KERNEL_MODE
#if defined(__clang__)
// Clang provides __builtin_ctzll natively; no user definition needed.
#elif __GNUC__ >= 15
// GCC 15+ provides __builtin_ctzll natively and forbids user definitions.
#else
// Older GCC needs this manual implementation.
 int  __builtin_ctzll(long long unsigned x)
{
    uint64_t result;
    asm volatile(
        "tzcnt %1, %0"
        : "=r"(result)
        : "r"(x)
    );
    return result;
}
#endif
#endif
