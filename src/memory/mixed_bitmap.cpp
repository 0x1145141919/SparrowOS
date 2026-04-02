#include "memory/FreePagesAllocator.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#ifdef USER_MODE
#include "new"
#include <unistd.h>
#include <stdlib.h>
#endif
constexpr uint8_t max_firstBCBorder=20;
uint64_t first_BCB_bitmap[(1ull<<(max_firstBCBorder+1))/64];
static void* fpa_wrapped_pgs_valloc(KURD_t* kurd, uint64_t pages, page_state_t state, uint8_t align_log2)
{
#ifdef KERNEL_MODE
    return __wrapped_pgs_valloc(kurd, pages, state, align_log2);
#endif
#ifdef USER_MODE
    (void)state;
    (void)align_log2;
    if (kurd) {
        *kurd = KURD_t();
    }
    size_t bytes = static_cast<size_t>(pages) * 4096;
    void* ptr = malloc(bytes ? bytes : 1);
    if (!ptr && kurd) {
        kurd->result = result_code::FAIL;
        kurd->level = level_code::ERROR;
    }
    return ptr;
#endif
}
FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::mixed_bitmap_t(uint64_t entry_count,vaddr_t base_addr)
{
    this->entry_count=entry_count;
    this->bitmap=(uint64_t*)base_addr;
}
KURD_t FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::default_kurd()
{
     return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_BITMAP,0,0,err_domain::CORE_MODULE);
}
void FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::mixedbitmap_base_specify(vaddr_t bitmap_base_addr)
{
    if (bitmap_base_addr != 0) {
        bitmap = reinterpret_cast<uint64_t*>(bitmap_base_addr);
    } else if (bitmap == nullptr) {
        bitmap = first_BCB_bitmap;
    }
    bsp_kout<<(void*)bitmap<<kendl;
    byte_bitmap_base=(uint8_t*)bitmap;
    bitmap_size_in_64bit_units=(entry_count+63)/64;
    bitmap_used_bit=0;
}
KURD_t FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=result_code::SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}
KURD_t FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::default_error()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}
FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::~mixed_bitmap_t()
{
    uint64_t pages_count=align_up(entry_count/8,4096)/4096;
    #ifdef KERNEL_MODE
    KURD_t kurd=__wrapped_pgs_vfree((void*)bitmap,pages_count);
    if(error_kurd(kurd)){
        //panic
        bsp_kout<<"FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::~mixed_bitmap_t()"<<kendl;
    }
    #endif
    #ifdef USER_MODE
    delete[] bitmap;
    #endif
    
}
KURD_t FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}
//返回0xFFFFFFFFFFFFFFFF表示没有找到
//标记为1的才认为是空闲，返回的引索是基于参数start_idx的偏移
//如果区间超过bitmap范围，则只在能搜索到的范围内搜索，不提前报错
uint64_t
FreePagesAllocator::BuddyControlBlock::mixed_bitmap_t::find_free_in_interval(uint64_t start_idx,
                                      uint64_t interval_length)
{
    constexpr uint64_t NOT_FOUND = 0xFFFFFFFFFFFFFFFFULL;

    const uint64_t bit_cap = entry_count; // entry_count 是位图的 bit 数
    if (start_idx >= bit_cap || interval_length == 0) {
        return NOT_FOUND;
    }

    // 截断搜索长度，避免 start_idx + interval_length 溢出
    const uint64_t max_length = bit_cap - start_idx;
    const uint64_t search_length = interval_length > max_length ? max_length : interval_length;
    const uint64_t end_idx = start_idx + search_length; // [start_idx, end_idx)

    uint64_t start_u64 = start_idx / 64;
    uint64_t end_u64   = (end_idx - 1) / 64;

    /* ---------- 情况 1：完全在同一个 u64 ---------- */
    if (start_u64 == end_u64) {
        uint64_t mask =
            (~0ULL << (start_idx & 63)) &
            (~0ULL >> (63 - ((end_idx - 1) & 63)));

        uint64_t bits = bitmap[start_u64] & mask;
        if (bits) {
            return start_u64 * 64
                 + __builtin_ctzll(bits)
                 - start_idx;
        }
        return NOT_FOUND;
    }

    /* ---------- 情况 2.1：起始残段 ---------- */
    {
        uint64_t mask = ~0ULL << (start_idx & 63);
        uint64_t bits = bitmap[start_u64] & mask;
        if (bits) {
            return start_u64 * 64
                 + __builtin_ctzll(bits)
                 - start_idx;
        }
    }

    /* ---------- 情况 2.2：完整 u64 ---------- */
    for (uint64_t u = start_u64 + 1; u < end_u64; ++u) {
        uint64_t bits = bitmap[u];
        if (bits) {
            return u * 64
                 + __builtin_ctzll(bits)
                 - start_idx;
        }
    }

    /* ---------- 情况 2.3：结尾残段 ---------- */
    {
        uint64_t mask = ~0ULL >> (63 - ((end_idx - 1) & 63));
        uint64_t bits = bitmap[end_u64] & mask;
        if (bits) {
            return end_u64 * 64
                 + __builtin_ctzll(bits)
                 - start_idx;
        }
    }

    return NOT_FOUND;
}
