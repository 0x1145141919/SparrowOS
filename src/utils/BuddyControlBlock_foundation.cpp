#include "util/BuddyControlBlock_foundation.h"
#include "abi/src_loc.h"

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

// ================================================================
// 收养：叶子已在全布局位图区写实，内部节点区清零
// 不重扫清零，只算 free_count[0]，状态置 JUVENILE
// ================================================================

void BuddyControlBlock_foundation::init_from_leaves(
    vaddr_t bitmap_va, uint8_t max_order_val)
{
    max_order = max_order_val;
    bitmap    = reinterpret_cast<uint64_t*>(bitmap_va);

    for (uint8_t i = 0; i < ORDER_COUNT; i++)
        free_count[i] = 0;

    const uint64_t leaf_cnt = 1ull << max_order;
    uint64_t free_leaf = 0;
    for (uint64_t o = 0; o < leaf_cnt; o++)
        if (leaf_read((1ull << max_order) + o))
            free_leaf++;
    free_count[0] = free_leaf;

    state = STATE_JUVENILE;
}

// ================================================================
// 成年仪式：JUVENILE → ADULT
// 自底向上填内部节点（4 状态），被合并的空闲叶子清位，全 free_count 重算
// 完成后满足 btree_validation 全部不变约束
// ================================================================

tmp_error_locator BuddyControlBlock_foundation::fold_up_from_leaves()
{
    if (state != STATE_JUVENILE)
        return SRC_LOC();

    for (uint8_t i = 0; i < ORDER_COUNT; i++)
        free_count[i] = 0;

    // 自底向上：order 1 .. max_order
    for (uint8_t k = 1; k <= max_order; k++) {
        const uint64_t level_base  = 1ull << (max_order - k);  // order-k 节点 heap idx 起点
        const uint64_t level_count = 1ull << (max_order - k);  // order-k 节点个数
        for (uint64_t j = 0; j < level_count; j++) {
            uint64_t idx = level_base + j;
            uint64_t lc  = idx << 1;
            uint64_t rc  = (idx << 1) | 1;

            if (k == 1) {
                bool lf = leaf_read(lc);
                bool rf = leaf_read(rc);
                if (lf && rf) {
                    node_write(idx, NODE_FREE);
                    free_count[1]++;
                    leaf_write(lc, false);
                    leaf_write(rc, false);
                } else if (!lf && !rf) {
                    node_write(idx, NODE_OCCUPIED);
                } else {
                    node_write(idx, NODE_NONLEAF);
                }
            } else {
                uint8_t ls = node_read(lc);
                uint8_t rs = node_read(rc);
                if (ls == NODE_FREE && rs == NODE_FREE) {
                    node_write(idx, NODE_FREE);
                    free_count[k]++;
                    free_count[k - 1] -= 2;   // 双子不再空闲，合并到 order-k
                    node_write(lc, NODE_NONEXIST);
                    node_write(rc, NODE_NONEXIST);
                } else if (ls == NODE_OCCUPIED && rs == NODE_OCCUPIED) {
                    node_write(idx, NODE_OCCUPIED);
                } else {
                    node_write(idx, NODE_NONLEAF);
                }
            }
        }
    }

    // 重算 free_count[0]：合并后剩余的空闲叶子
    const uint64_t leaf_cnt = 1ull << max_order;
    uint64_t free_leaf = 0;
    for (uint64_t o = 0; o < leaf_cnt; o++)
        if (leaf_read((1ull << max_order) + o))
            free_leaf++;
    free_count[0] = free_leaf;

    state = STATE_ADULT;
    return 0;
}

// ================================================================
// 幼年态 order-0 分配：找 acquire_count 个连续空闲叶子 → 占位 → 返回页偏移
// 纯位图语义，无内存/对齐概念；只维护 free_count[0]
// ================================================================

uint64_t BuddyControlBlock_foundation::juvenile_alloc_order0(
    tmp_error_locator& kurd, uint64_t acquire_count)
{
    kurd = 0;
    if (state != STATE_JUVENILE) {
        kurd = SRC_LOC();
        return INVALID_OFFSET;
    }
    if (acquire_count == 0 || acquire_count > (1ull << max_order)) {
        kurd = SRC_LOC();
        return INVALID_OFFSET;
    }
    if (free_count[0] < acquire_count) {
        kurd = SRC_LOC();
        return INVALID_OFFSET;
    }

    const uint64_t leaf_cnt = 1ull << max_order;
    uint64_t run = 0, start = 0;
    for (uint64_t o = 0; o < leaf_cnt; o++) {
        if (leaf_read((1ull << max_order) + o)) {
            if (run == 0) start = o;
            if (++run >= acquire_count) break;
        } else {
            run = 0;
        }
    }
    if (run < acquire_count) {
        kurd = SRC_LOC();
        return INVALID_OFFSET;
    }

    for (uint64_t o = start; o < start + acquire_count; o++)
        leaf_write((1ull << max_order) + o, false);
    free_count[0] -= acquire_count;
    return start;
}

// ================================================================
// 幼年态 order-0 归还：清 return_count 个叶子 → 维护 free_count[0]
// ================================================================

tmp_error_locator BuddyControlBlock_foundation::juvenile_free_order0(
    uint64_t offset, uint64_t return_count)
{
    if (state != STATE_JUVENILE)
        return SRC_LOC();
    if (return_count == 0)
        return SRC_LOC();
    if (offset + return_count > (1ull << max_order))
        return SRC_LOC();

    // 防御：目标范围内叶子须全部占用（重复释放检测）
    for (uint64_t o = offset; o < offset + return_count; o++) {
        if (leaf_read((1ull << max_order) + o))
            return SRC_LOC();
    }
    for (uint64_t o = offset; o < offset + return_count; o++)
        leaf_write((1ull << max_order) + o, true);
    free_count[0] += return_count;
    return 0;
}
