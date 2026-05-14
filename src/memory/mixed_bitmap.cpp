#include "memory/FreePagesAllocator.h"
#include <cstring>

// HCB_v3 移植: mixed_bitmap_v2 — heap-encoded 二叉树位图
// 替代原 mixed_bitmap_t (order_bases 分段布局)

constexpr uint8_t max_firstBCBorder=20;
uint64_t first_BCB_bitmap[(1ull<<(max_firstBCBorder+1))/64];

static inline uint64_t idx_from_order_offset(uint8_t out_order, uint8_t order, uint64_t offset)
{
    return (1ULL << (out_order - order)) + offset;
}

static inline void order_offset_from_idx(uint8_t out_order, uint64_t idx,
                                           uint8_t& out_order_val, uint64_t& out_offset)
{
    uint64_t level = 63 - __builtin_clzll(idx);
    out_order_val = static_cast<uint8_t>(out_order - level);
    out_offset = idx - (1ULL << level);
}

static uint64_t u64_scan_interval(uint64_t* bitmap, uint64_t start, uint64_t end)
{
    if (start >= end) return ~0ULL;
    uint64_t fu64 = start >> 6;
    uint64_t lu64 = (end - 1) >> 6;
    uint64_t skip = start & 63;
    if (skip) {
        uint64_t bits = bitmap[fu64] & (~0ULL << skip);
        if (bits) {
            uint64_t pos = __builtin_ctzll(bits);
            uint64_t idx = (fu64 << 6) + pos;
            if (idx < end) return idx;
        }
        fu64++;
    }
    for (uint64_t w = fu64; w <= lu64; w++) {
        if (bitmap[w] == 0) continue;
        uint64_t pos = __builtin_ctzll(bitmap[w]);
        uint64_t idx = (w << 6) + pos;
        if (idx < end) return idx;
    }
    return ~0ULL;
}

void FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2::online(vaddr_t bitmap_va, uint8_t out_order_val)
{
    out_order = out_order_val;
    const uint64_t total_bits = 1ULL << (out_order + 1);
    bitmap = reinterpret_cast<uint64_t*>(bitmap_va);
    bitmap_size_in_64bit_units = (total_bits + 63) / 64;
    bitmap_used_bit = 0;
    byte_bitmap_base = reinterpret_cast<uint8_t*>(bitmap);
    __builtin_memset(bitmap, 0, bitmap_size_in_64bit_units * sizeof(uint64_t));
    bit_set(1, true);
    bitmap_used_bit = 1;
}

void FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2::offline()
{
    out_order = 0; bitmap = nullptr; bitmap_size_in_64bit_units = 0;
    bitmap_used_bit = 0; byte_bitmap_base = nullptr;
}

void FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2::bit_set0(uint64_t offset, uint8_t order)
{
    uint64_t idx = idx_from_order_offset(out_order, order, offset);
    if (bit_get(idx)) bitmap_used_bit--;
    bit_set(idx, false);
}

void FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2::bit_set1(uint64_t offset, uint8_t order)
{
    uint64_t idx = idx_from_order_offset(out_order, order, offset);
    if (!bit_get(idx)) bitmap_used_bit++;
    bit_set(idx, true);
}

bool FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2::bit_get(uint64_t offset, uint8_t order)
{
    uint64_t idx = idx_from_order_offset(out_order, order, offset);
    return bit_get(idx);
}

uint64_t FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2::scan_free_block(uint8_t& order)
{
    uint64_t range_end = 1ULL << (1 + out_order - order);
    uint64_t found = u64_scan_interval(bitmap, 1, range_end);
    if (found == ~0ULL) return ~0ULL;
    uint64_t offset;
    order_offset_from_idx(out_order, found, order, offset);
    return offset;
}
