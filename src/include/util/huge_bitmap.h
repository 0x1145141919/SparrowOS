#pragma once
#include "bitmap.h"
class huge_bitmap:public bitmap_t
{ 
    public:
    using bitmap_t::bit_set;
    using bitmap_t::bit_get;
    using bitmap_t::bits_set;
    using bitmap_t::bytes_set;
    using bitmap_t::u64s_set;
    huge_bitmap(uint64_t bits_count);
    KURD_t second_stage_init();//涉及到页框分配，不是原子性的
    ~huge_bitmap();
};
