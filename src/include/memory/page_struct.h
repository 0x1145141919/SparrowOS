#pragma once
#include <stdint.h>
#include "util/lock.h"
enum class page_state_t : uint8_t { 
    free = 0, 
    kernel_persisit = 1, 
    kernel_anonymous = 2, 
    user_file = 3, 
    user_anonymous = 4, 
    dma = 5,
    kernel_pinned = 10,
    reserved = 63
};

/**
 * @brief page struct
 *此数据结构必须进入mem_map才有语义，
 *透明大页支持：若某个页框的page.is_skipped为真则属于透明页，其实际语义被mem_map[ptr]所代表
 */
struct page{
    page_state_t state;
};
void*ptr_dump(page*p);
static_assert(sizeof(page)==1,"struct page size must be 16 bytes");
