#pragma once
#include <stdint.h>
struct tsc_complex{
    uint32_t tsc_fs_per_cycle;
    uint64_t is_valid:1;
    uint64_t is_tsc_ddline_avaliabe:1;
    uint64_t is_tsc_reliable:1;
};
struct time_complex{
    uint32_t lapic_fs_per_cycle;
    tsc_complex complex;
};
void tsc_regist();//创建本CPU的time_complex后立马tsc能力测量，由于架构特性刻意设计complex创建和tsc能力测量是串行绑定的
