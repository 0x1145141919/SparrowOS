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
    uint32_t refcount;

    union {
        uint32_t raw;
        struct {
            uint32_t is_skipped:1;
        }bitfield;
    }page_flags;

    union{
        struct{
            uint64_t type:6;
            uint64_t ptr:52;
            uint64_t order:6;
        }head;

        struct{
            uint64_t type:6;
            uint64_t head_page:52;
            uint64_t huge_order:6;
        }tranp;
    };
};
struct page_v2{
    uint32_t refcount;
    uint32_t map_count:24;
    uint32_t is_skipped:1;
    uint32_t page_flags_reserved:7;
    uint64_t compressed_kptr:52;
    uint64_t page_type:6;
    uint64_t page_flags_reserved2:6;
    union 
    {
        uint64_t raw;
    }extra;  
};
void*ptr_dump(page*p);
static_assert(sizeof(page)==16,"struct page size must be 16 bytes");
static_assert(sizeof(page_v2)==24,"struct page_v2 size must be 16 bytes");