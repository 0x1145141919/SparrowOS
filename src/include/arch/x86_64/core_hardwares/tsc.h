#pragma once
#include <stdint.h>
extern    uint32_t tsc_fs_per_cycle;
extern   bool is_tsc_ddline_avaliabe;
extern   bool is_tsc_reliable;
struct time_complex{
    uint32_t lapic_fs_per_cycle;
};
void tsc_regist();//创建本CPU的time_complex后立马tsc能力测量，由于架构特性刻意设计complex创建和tsc能力测量是串行绑定的
