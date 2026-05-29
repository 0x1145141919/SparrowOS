#include "util/BuddyControlBlock_foundation.h"

// ════════════════════════════════════════════════════════════════
// BuddyControlBlock_foundation 基类实现
//
// 所有派生类共享的位图访问、索引辅助、btree_validation
// ════════════════════════════════════════════════════════════════

// ================================================================
// 底层位图访问
// ================================================================

uint8_t BuddyControlBlock_foundation::node_read(uint64_t heap_idx) const
{
    uint64_t boff = heap_idx << 1;
    return (bitmap[boff >> 6] >> (boff & 63)) & 0b11;
}

void BuddyControlBlock_foundation::node_write(uint64_t heap_idx, uint8_t val)
{
    uint64_t boff = heap_idx << 1;
    uint64_t& w = bitmap[boff >> 6];
    uint8_t   sh = boff & 63;
    w = (w & ~(0b11ull << sh)) | ((uint64_t)(val & 0b11) << sh);
}

bool BuddyControlBlock_foundation::leaf_read(uint64_t leaf_idx) const
{
    uint64_t boff = (1ull << max_order) + leaf_idx;
    return (bitmap[boff >> 6] >> (boff & 63)) & 1;
}

void BuddyControlBlock_foundation::leaf_write(uint64_t leaf_idx, bool free)
{
    uint64_t boff = (1ull << max_order) + leaf_idx;
    uint64_t& w = bitmap[boff >> 6];
    uint8_t   sh = boff & 63;
    if (free)
        w |=  (1ull << sh);
    else
        w &= ~(1ull << sh);
}

// ================================================================
// heap 索引辅助
// ================================================================

uint8_t BuddyControlBlock_foundation::heap_idx_order(uint64_t idx) const
{
    uint8_t level = 63 - __builtin_clzll(idx);
    return max_order - level;
}

uint64_t BuddyControlBlock_foundation::order_offset_to_idx(
    uint8_t order, uint64_t offset) const
{
    return (1ull << (max_order - order)) + offset;
}

void BuddyControlBlock_foundation::idx_to_order_offset(
    uint64_t idx, uint8_t& order, uint64_t& offset) const
{
    uint8_t level = 63 - __builtin_clzll(idx);
    order  = max_order - level;
    offset = idx - (1ull << level);
}

// ================================================================
// is_free
// ================================================================

bool BuddyControlBlock_foundation::is_free(uint8_t order, uint64_t offset) const
{
    uint64_t idx = order_offset_to_idx(order, offset);
    if (order == 0)
        return leaf_read(idx);
    return node_read(idx) == NODE_FREE;
}

// ================================================================
// btree_validation + validate_subtree
//
// 规则：
//   (1) NODE_FREE / NODE_NONEXIST → 子树必须全空
//       （所有非 order0 后代必须 NONEXIST，order0 后代 leaf=0）
//   (2) NODE_OCCUPIED → 两种子情况：
//       b1. children 全 NONEXIST → 真正整块占用
//       b2. children 全 OCCUPIED → 占用坍缩产物，递归验证
//   (3) NODE_NONLEAF → 孩子不能有 NONEXIST
//       且不能同为 FREE（应合并）或同为 OCCUPIED（应坍缩）
//   (4) （新规则）NONEXIST 与非 NONEXIST 做兄弟 → 非法
//       FREE/NONEXIST/ORDER1 三种节点状态中均检查
// ================================================================

bool BuddyControlBlock_foundation::validate_subtree(
    uint64_t idx, uint8_t order, uint64_t count[]) const
{
    if (order == 0) {
        if (leaf_read(idx))
            count[0]++;
        return true;
    }

    uint8_t state = node_read(idx);

    switch (state) {

    // ── 规则1 ──
    // FREE 或 NONEXIST 节点：子树必须完全空
    // 规则4：两个孩子在 order≥2 时必须都是 NONEXIST
    // 不允许 (NONEXIST, FREE) 或 (NONEXIST, OCCUPIED) 等混合
    case NODE_NONEXIST:
    case NODE_FREE: {
        if (state == NODE_FREE)
            count[order]++;
        if (order == 1)
            return !leaf_read(idx << 1) && !leaf_read((idx << 1) | 1);
        uint8_t ls = node_read(idx << 1);
        uint8_t rs = node_read((idx << 1) | 1);
        // 规则4: NONEXIST 不能与非 NONEXIST 做兄弟
        if (ls != NODE_NONEXIST || rs != NODE_NONEXIST)
            return false;
        return true;
    }

    // ── 规则2 ──
    // OCCUPIED 节点：子必须全 NONEXIST（真正占用）或全 OCCUPIED（坍缩）
    // 规则4: (NONEXIST, OCCUPIED) 混合 → false
    case NODE_OCCUPIED: {
        if (order == 1) {
            return !leaf_read(idx << 1) && !leaf_read((idx << 1) | 1);
        }
        uint8_t ls = node_read(idx << 1);
        uint8_t rs = node_read((idx << 1) | 1);
        if (ls == NODE_NONEXIST && rs == NODE_NONEXIST)
            return true;
        if (ls == NODE_OCCUPIED && rs == NODE_OCCUPIED) {
            bool lv = validate_subtree(idx << 1, order - 1, count);
            bool rv = validate_subtree((idx << 1) | 1, order - 1, count);
            return lv && rv;
        }
        // (NONEXIST, OCCUPIED) / (OCCUPIED, NONEXIST) → 规则4 违规
        // (NONEXIST/FREE) 混合 → 违规
        return false;
    }

    // ── 规则3 ──
    // NONLEAF 的孩子不能有 NONEXIST，不能同为 FREE 或同为 OCCUPIED
    // 规则4: 因为 NONEXIST 被明确禁止作为 NONLEAF 的子节点，
    //        实际上不会有 (NONEXIST, FREE) 等组合出现
    case NODE_NONLEAF: {
        if (order == 1) {
            bool lf = leaf_read(idx << 1);
            bool rf = leaf_read((idx << 1) | 1);
            if (lf == rf)
                return false;
            bool left_ok  = validate_subtree(idx << 1, 0, count);
            bool right_ok = validate_subtree((idx << 1) | 1, 0, count);
            return left_ok && right_ok;
        }

        uint8_t left_state  = node_read(idx << 1);
        uint8_t right_state = node_read((idx << 1) | 1);

        // 规则3: NONLEAF 孩子不能有 NONEXIST
        if (left_state == NODE_NONEXIST || right_state == NODE_NONEXIST)
            return false;

        // 不能同为 FREE 或同为 OCCUPIED
        if ((left_state == NODE_FREE && right_state == NODE_FREE) ||
            (left_state == NODE_OCCUPIED && right_state == NODE_OCCUPIED))
            return false;

        bool left_ok  = validate_subtree(idx << 1,       order - 1, count);
        bool right_ok = validate_subtree((idx << 1) | 1, order - 1, count);
        return left_ok && right_ok;
    }

    default:
        return false;
    }
}

KURD_t BuddyControlBlock_foundation::btree_validation()
{
    KURD_t kurd;
    kurd.result = result_code::FAIL;
    kurd.level  = 7;  // FATAL

    uint64_t count[ORDER_COUNT] = {0};

    if (!validate_subtree(1, max_order, count)) {
        kurd.result = result_code::FAIL;
        return kurd;
    }

    for (uint8_t o = 0; o <= max_order; o++) {
        if (count[o] != free_count[o]) {
            kurd.result = result_code::FAIL;
            return kurd;
        }
    }

    kurd.result = result_code::SUCCESS;
    return kurd;
}
