#pragma once
#include <stdint.h>
#if !defined(__clang__) && __GNUC__ < 15
int  __builtin_ctzll(long long unsigned x);
#endif
