// ════════════════════════════════════════════════════════════════
// v3_simulator — mixed_bitmap_v2 (1-bit heap-encoded) 模拟器
//
// 接口与 BuddyControlBlock_foundation 对齐，供三路 benchmark
// 使用 BFS scan_free_block: O(2^N) worst-case 位图访问
// ════════════════════════════════════════════════════════════════

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include "abi/os_error_definitions.h"

class v3_simulator {
public:
    static constexpr uint8_t  ORDER_COUNT = 65;
    static constexpr uint64_t INVALID_OFFSET = ~0ULL;
    static constexpr uint8_t  ERROR_MARK = 0x40;

private:
    uint64_t* bitmap;
    uint8_t   max_order;
    uint64_t  total_bits;
    uint64_t  free_count[ORDER_COUNT];

    // heap-index ↔ (order, offset)
    static uint64_t idx_from_order_offset(uint8_t N, uint8_t order, uint64_t offset) {
        return (1ULL << (N - order)) + offset;
    }

    static void idx_to_order_offset(uint8_t N, uint64_t idx,
                                     uint8_t& order, uint64_t& offset) {
        uint8_t level = 63 - __builtin_clzll(idx);
        order  = N - level;
        offset = idx - (1ULL << level);
    }

    // 位操作
    bool bit_get(uint64_t idx) const {
        return (bitmap[idx >> 6] >> (idx & 63)) & 1;
    }

    void bit_set(uint64_t idx, bool val) {
        uint64_t& w = bitmap[idx >> 6];
        uint8_t   sh = idx & 63;
        if (val) w |=  (1ULL << sh);
        else     w &= ~(1ULL << sh);
    }

    // BFS 区间扫描: 在 [start, end) 找第一个 1-bit
    uint64_t u64_scan_interval(uint64_t start, uint64_t end) const {
        if (start >= end) return INVALID_OFFSET;
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
        return INVALID_OFFSET;
    }

public:
    v3_simulator() : bitmap(nullptr), max_order(0), total_bits(0) {
        std::memset(free_count, 0, sizeof(free_count));
    }

    void init(uint64_t bitmap_va, uint8_t mo) {
        max_order = mo;
        total_bits = 1ULL << (mo + 1);  // 2 * 2^N bits
        uint64_t u64cnt = (total_bits + 63) >> 6;
        bitmap = reinterpret_cast<uint64_t*>(bitmap_va);
        std::memset(bitmap, 0, u64cnt * sizeof(uint64_t));

        // 根节点 = FREE
        bit_set(1, true);
        std::memset(free_count, 0, sizeof(free_count));
        free_count[max_order] = 1;
    }

    // ─── BFS 扫描: 从 base_order 往上，在每个 order 的节点范围扫第一个 1-bit ───
    uint64_t find_candidate(uint8_t& base_order, KURD_t& kurd) {
        KURD_t ok, fail;
        ok.result = result_code::SUCCESS;
        fail.result = result_code::FAIL;

        // 从 base_order 到 max_order 逐阶 BFS 扫描
        for (uint8_t order = base_order; order <= max_order; order++) {
            uint64_t range_start = 1;
            uint64_t range_end   = 1ULL << (1 + max_order - order);
            uint64_t found = u64_scan_interval(range_start, range_end);
            if (found != INVALID_OFFSET) {
                uint8_t fo;
                uint64_t off;
                idx_to_order_offset(max_order, found, fo, off);
                base_order = fo;
                kurd = ok;
                return off;
            }
        }

        base_order = ERROR_MARK;
        kurd = fail;
        return INVALID_OFFSET;
    }

    // ─── split: 递归清除目标位然后设置子节点位 ───
    KURD_t split(uint8_t order, uint64_t offset, uint8_t target_order) {
        KURD_t ok;
        ok.result = result_code::SUCCESS;
        while (order > target_order) {
            uint64_t idx = idx_from_order_offset(max_order, order, offset);

            // 清当前位
            if (bit_get(idx)) {
                bit_set(idx, false);
                free_count[order]--;
            }

            // 设左右子位
            uint64_t left_idx  = idx << 1;
            uint64_t right_idx = (idx << 1) | 1;

            // 子节点可能是叶子(1-bit)或内部节点
            bit_set(left_idx, true);
            bit_set(right_idx, true);
            if (order - 1 <= max_order) free_count[order - 1] += 2;

            order--;
            offset <<= 1;
            offset = offset;  // left child offset = 2*parent_offset
        }

        // 最终需要被 occupy 的节点保持 1(set)
        // occupy 会清除它
        return ok;
    }

    KURD_t order_occupy_try(uint8_t order, uint64_t offset) {
        KURD_t ok;
        ok.result = result_code::SUCCESS;
        uint64_t idx = idx_from_order_offset(max_order, order, offset);
        if (bit_get(idx)) {
            bit_set(idx, false);
            free_count[order]--;
        }
        return ok;
    }

    uint8_t order_return(uint8_t order, uint64_t offset, KURD_t& kurd) {
        KURD_t ok;
        ok.result = result_code::SUCCESS;

        uint64_t idx = idx_from_order_offset(max_order, order, offset);
        bit_set(idx, true);
        free_count[order]++;

        // 合并: 向上检查 buddy
        uint8_t cur_order = order;
        uint64_t cur_idx  = idx;

        while (cur_order < max_order) {
            uint64_t buddy_idx = cur_idx ^ 1;
            if (!bit_get(buddy_idx)) break;

            // 清除双子
            bit_set(cur_idx, false);
            bit_set(buddy_idx, false);
            free_count[cur_order] -= 2;

            // 设父
            uint64_t parent_idx = cur_idx >> 1;
            bit_set(parent_idx, true);
            free_count[cur_order + 1]++;

            cur_idx   = parent_idx;
            cur_order = cur_order + 1;
        }

        kurd = ok;
        return cur_order;
    }

    bool order_exist_check(uint8_t order) const {
        return free_count[order] > 0;
    }

    bool is_free(uint8_t order, uint64_t offset) const {
        uint64_t idx = idx_from_order_offset(max_order, order, offset);
        return bit_get(idx);
    }

    uint64_t get_free_count(uint8_t order) const {
        return (order < ORDER_COUNT) ? free_count[order] : 0;
    }

    uint8_t get_max_order() const { return max_order; }
};
