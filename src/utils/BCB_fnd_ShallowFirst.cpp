#include "util/BCB_fnd_ShallowFirst.h"
#include "util/OS_utils.h"

#define BCB_HIGHER_GET(heap_idx, order) \
    higher_node_get((heap_idx) - (1ull << (max_order - (order))), (order))
#define BCB_HIGHER_SET(heap_idx, order, val) \
    higher_node_set((heap_idx) - (1ull << (max_order - (order))), (order), (val))
#define BCB_ORDER0_TEST(heap_idx) \
    order0_bit_test((heap_idx) - (1ull << max_order))
#define BCB_ORDER0_SET(heap_idx, val) \
    order0_bit_set((heap_idx) - (1ull << max_order), (val))

// ════════════════════════════════════════════════════════════════
// BCB_fnd_ShallowFirst 实现
//
// 位图访问、索引辅助、btree_validation、pure_init 由基类提供
// 本文件仅实现 4 个纯虚分配接口
// 无防御性校验，无 KURD 三阶段错误链
// ════════════════════════════════════════════════════════════════

// ================================================================
// DFS 只读查找 — order 预检 + 双 child 一次性读取版
// ================================================================

uint64_t BCB_fnd_ShallowFirst::dfs_find_free(
    uint64_t idx, uint8_t target_order) const
{
    uint8_t cur_order = heap_idx_order(idx);
    if (cur_order < target_order)
        return INVALID_OFFSET;

    // ── 检查 idx 自身是否为 FREE ──
    // 只有入口 root 或首次引用 FREE 节点时会走此分支
    {
        uint8_t self_state;
        if (idx < (1ull << max_order))
            self_state = BCB_HIGHER_GET(idx, cur_order);
        else
            self_state = BCB_ORDER0_TEST(idx) ? NODE_FREE : NODE_OCCUPIED;
        if (self_state == NODE_FREE)
            return idx;
        if (self_state != NODE_NONLEAF)
            return INVALID_OFFSET;
    }

    // 至此 self = NONLEAF，读 children

    // order=1: children are order-0 leaves (1-bit each)
    if (cur_order == 1) {
        uint8_t subnodes = BCB_ORDER0_TEST(idx << 1)
                         | (BCB_ORDER0_TEST(1 | (idx << 1)) << 1);
        switch (subnodes) {
            case 0b01: return idx << 1;          // left free
            case 0b10: return (idx << 1) | 1;    // right free
            default:   return INVALID_OFFSET;    // 0b00/0b11 -> invariant violation
        }
    }

    // order >= 2: children are 2-bit nodes
    uint8_t subnodes = BCB_HIGHER_GET(idx << 1, cur_order - 1)
                     | (BCB_HIGHER_GET(1 | (idx << 1), cur_order - 1) << 2);
    switch (subnodes) {

    // ── 两边都 NONLEAF → 左优先 DFS ──
    case NODE_NONLEAF | (NODE_NONLEAF << 2): {
        uint64_t left = dfs_find_free(idx << 1, target_order);
        if (left != INVALID_OFFSET)
            return left;
        return dfs_find_free((idx << 1) | 1, target_order);
    }

    // ── 左边 FREE → 直接取左 ──
    case NODE_FREE | (NODE_OCCUPIED << 2):
    case NODE_FREE | (NODE_NONLEAF  << 2):
        return idx << 1;

    // ── 右边 FREE → 直接取右 ──
    case NODE_OCCUPIED | (NODE_FREE << 2):
    case NODE_NONLEAF  | (NODE_FREE << 2):
        return (idx << 1) | 1;

    // ── NONLEAF + OCCUPIED / OCCUPIED + NONLEAF → 进 NONLEAF 边 ──
    case NODE_NONLEAF  | (NODE_OCCUPIED << 2):
        return dfs_find_free(idx << 1, target_order);
    case NODE_OCCUPIED | (NODE_NONLEAF  << 2):
        return dfs_find_free((idx << 1) | 1, target_order);

    default:
        // FREE|FREE → parent 应 FREE 而非 NONLEAF
        // 其他混合 → invariant violation
        return INVALID_OFFSET;
    }
}

// ================================================================
// find_candidate
// ================================================================

uint64_t BCB_fnd_ShallowFirst::find_candidate(
    uint8_t& base_order, KURD_t& kurd)
{
    uint8_t scan_order = base_order;
    while (scan_order <= max_order && free_count[scan_order] == 0)
        scan_order++;

    if (scan_order > max_order) {
        KURD_t k;
        k.result = result_code::FAIL;
        base_order = ERROR_MARK;
        kurd = k;
        return INVALID_OFFSET;
    }

    uint64_t found_idx = dfs_find_free(1, scan_order);

    if (found_idx == INVALID_OFFSET) {
        KURD_t k;
        k.result = result_code::FAIL;
        base_order = ERROR_MARK + 1;
        kurd = k;
        return INVALID_OFFSET;
    }

    uint8_t  found_order;
    uint64_t found_offset;
    idx_to_order_offset(found_idx, found_order, found_offset);

    KURD_t k;
    k.result = result_code::SUCCESS;
    base_order = found_order;
    kurd = k;
    return found_offset;
}

// ================================================================
// split
// ================================================================

void BCB_fnd_ShallowFirst::split_internal(
    uint64_t idx, uint8_t order, uint8_t target_order)
{
    while (order > target_order) {
        uint8_t child_order = order - 1;
        uint64_t left_idx   = idx << 1;
        uint64_t right_idx  = (idx << 1) | 1;

        BCB_HIGHER_SET(idx, order, NODE_NONLEAF);
        free_count[order]--;

        if (child_order > 0) {
            BCB_HIGHER_SET(left_idx, child_order, NODE_FREE);
            BCB_HIGHER_SET(right_idx, child_order, NODE_FREE);
        } else {
            BCB_ORDER0_SET(left_idx, true);
            BCB_ORDER0_SET(right_idx, true);
        }
        free_count[child_order] += 2;

        idx   = left_idx;
        order = child_order;
    }
}

KURD_t BCB_fnd_ShallowFirst::split(
    uint8_t order, uint64_t offset, uint8_t target_order)
{
    if (order > max_order || target_order > order) {
        KURD_t k;
        k.result = result_code::FAIL;
        return k;
    }

    if (order == target_order) {
        KURD_t k;
        k.result = result_code::SUCCESS;
        return k;
    }

    uint64_t idx = order_offset_to_idx(order, offset);

    if (BCB_HIGHER_GET(idx, order) != NODE_FREE) {
        KURD_t k;
        k.result = result_code::FAIL;
        return k;
    }

    split_internal(idx, order, target_order);

    KURD_t k;
    k.result = result_code::SUCCESS;
    return k;
}

// ================================================================
// order_occupy_try
// ================================================================

void BCB_fnd_ShallowFirst::occupy_internal(
    uint64_t idx, uint8_t order)
{
    if (order == 0)
        BCB_ORDER0_SET(idx, false);
    else
        BCB_HIGHER_SET(idx, order, NODE_OCCUPIED);

    free_count[order]--;

    // ── 占用坍缩 ──
    {
        uint8_t  co = order;
        uint64_t ci = idx;
        while (co < max_order) {
            uint64_t pi = ci >> 1;
            uint64_t bi = ci ^ 1;
            if (BCB_HIGHER_GET(pi, co + 1) != NODE_NONLEAF)
                break;
            bool bo = (co == 0) ? !BCB_ORDER0_TEST(bi)
                                : (BCB_HIGHER_GET(bi, co) == NODE_OCCUPIED);
            if (!bo) break;
            BCB_HIGHER_SET(pi, co + 1, NODE_OCCUPIED);
            ci = pi;
            co = co + 1;
        }
    }
}

KURD_t BCB_fnd_ShallowFirst::order_occupy_try(
    uint8_t order, uint64_t offset)
{
    uint64_t idx = order_offset_to_idx(order, offset);

    uint8_t cur_state;
    if (order == 0)
        cur_state = order0_bit_test(offset) ? NODE_FREE : NODE_OCCUPIED;
    else
        cur_state = BCB_HIGHER_GET(idx, order);

    if (cur_state != NODE_FREE) {
        KURD_t k;
        k.result = result_code::FAIL;
        return k;
    }

    occupy_internal(idx, order);

    KURD_t k;
    k.result = result_code::SUCCESS;
    return k;
}

// ================================================================
// order_return
// ================================================================

uint8_t BCB_fnd_ShallowFirst::coalesce_internal(
    uint64_t idx, uint8_t order)
{
    uint8_t  cur_order = order;
    uint64_t cur_idx   = idx;

    while (cur_order < max_order) {
        uint64_t buddy_idx  = cur_idx ^ 1;
        uint64_t parent_idx = cur_idx >> 1;

        bool buddy_free;
        if (cur_order == 0)
            buddy_free = BCB_ORDER0_TEST(buddy_idx);
        else
            buddy_free = (BCB_HIGHER_GET(buddy_idx, cur_order) == NODE_FREE);

        if (!buddy_free)
            break;

        if (cur_order == 0) {
            BCB_ORDER0_SET(cur_idx, false);
            BCB_ORDER0_SET(buddy_idx, false);
        } else {
            BCB_HIGHER_SET(cur_idx, cur_order, NODE_NONEXIST);
            BCB_HIGHER_SET(buddy_idx, cur_order, NODE_NONEXIST);
        }
        free_count[cur_order] -= 2;

        BCB_HIGHER_SET(parent_idx, cur_order + 1, NODE_FREE);
        free_count[cur_order + 1]++;

        cur_idx   = parent_idx;
        cur_order = cur_order + 1;
    }

    // ── 坍缩展开 ──
    {
        uint8_t  wo = cur_order;
        uint64_t wi = cur_idx;
        while (wo < max_order) {
            uint64_t pi = wi >> 1;
            if (pi < 1) break;
            uint8_t ps = BCB_HIGHER_GET(pi, wo + 1);
            if (ps != NODE_OCCUPIED) break;
            uint8_t cc = (wo == 0)
                ? (BCB_ORDER0_TEST(pi << 1) ? NODE_FREE : NODE_OCCUPIED)
                : BCB_HIGHER_GET(pi << 1, wo);
            if (cc == NODE_NONEXIST) break;
            BCB_HIGHER_SET(pi, wo + 1, NODE_NONLEAF);
            wi = pi;
            wo = wo + 1;
        }
    }

    return cur_order;
}

uint8_t BCB_fnd_ShallowFirst::order_return(
    uint8_t order, uint64_t offset, KURD_t& kurd)
{
    uint64_t idx = order_offset_to_idx(order, offset);

    uint8_t cur_state;
    if (order == 0)
        cur_state = order0_bit_test(offset) ? NODE_FREE : NODE_OCCUPIED;
    else
        cur_state = BCB_HIGHER_GET(idx, order);

    if (cur_state != NODE_OCCUPIED) {
        KURD_t k;
        k.result = result_code::FAIL;
        kurd = k;
        return ERROR_MARK;
    }

    if (order == 0)
        BCB_ORDER0_SET(idx, true);
    else
        BCB_HIGHER_SET(idx, order, NODE_FREE);

    free_count[order]++;

    uint8_t final_order = coalesce_internal(idx, order);

    KURD_t k;
    k.result = result_code::SUCCESS;
    kurd = k;
    return final_order;
}

#undef BCB_HIGHER_GET
#undef BCB_HIGHER_SET
#undef BCB_ORDER0_TEST
#undef BCB_ORDER0_SET
