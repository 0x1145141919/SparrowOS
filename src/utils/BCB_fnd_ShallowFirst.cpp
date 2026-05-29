#include "util/BCB_fnd_ShallowFirst.h"
#include "util/OS_utils.h"

// ════════════════════════════════════════════════════════════════
// BCB_fnd_ShallowFirst 实现
//
// 位图访问、索引辅助、btree_validation 由基类提供
// 本文件仅实现 5 个纯虚分配接口
// 无防御性校验，无 KURD 三阶段错误链
// ════════════════════════════════════════════════════════════════

// ================================================================
// 初始化
// ================================================================

void BCB_fnd_ShallowFirst::init(
    vaddr_t bitmap_va, uint8_t max_order_val)
{
    max_order = max_order_val;

    const uint64_t total_bits = (3ull << max_order);
    const uint64_t u64_count  = (total_bits + 63) >> 6;

    bitmap = reinterpret_cast<uint64_t*>(bitmap_va);
    ksetmem_8(bitmap, 0, u64_count * sizeof(uint64_t));

    node_write(1, NODE_FREE);

    for (uint8_t i = 0; i < ORDER_COUNT; i++)
        free_count[i] = 0;
    free_count[max_order] = 1;
}

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
            self_state = node_read(idx);
        else
            self_state = leaf_read(idx) ? NODE_FREE : NODE_OCCUPIED;
        if (self_state == NODE_FREE)
            return idx;
        if (self_state != NODE_NONLEAF)
            return INVALID_OFFSET;
    }

    // 至此 self = NONLEAF，读 children

    // order=1: children are order-0 leaves (1-bit each)
    if (cur_order == 1) {
        // leaf_read(left) | leaf_read(right) << 1
        uint8_t subnodes = leaf_read(idx << 1)
                         | (leaf_read(1 | (idx << 1)) << 1);
        switch (subnodes) {
            case 0b01: return idx << 1;          // left free
            case 0b10: return (idx << 1) | 1;    // right free
            default:   return INVALID_OFFSET;    // 0b00/0b11 -> invariant violation
        }
    }

    // order >= 2: children are 2-bit nodes
    // node_read(left) | node_read(right) << 2
    uint8_t subnodes = node_read(idx << 1)
                     | (node_read(1 | (idx << 1)) << 2);
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

        node_write(idx, NODE_NONLEAF);
        free_count[order]--;

        if (child_order > 0) {
            node_write(left_idx, NODE_FREE);
            node_write(right_idx, NODE_FREE);
        } else {
            leaf_write(left_idx, true);
            leaf_write(right_idx, true);
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

    if (node_read(idx) != NODE_FREE) {
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
        leaf_write(idx, false);
    else
        node_write(idx, NODE_OCCUPIED);

    free_count[order]--;

    // ── 占用坍缩 ──
    {
        uint8_t  co = order;
        uint64_t ci = idx;
        while (co < max_order) {
            uint64_t pi = ci >> 1;
            uint64_t bi = ci ^ 1;
            if (node_read(pi) != NODE_NONLEAF)
                break;
            bool bo = (co == 0) ? !leaf_read(bi)
                                : (node_read(bi) == NODE_OCCUPIED);
            if (!bo) break;
            node_write(pi, NODE_OCCUPIED);
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
        cur_state = leaf_read(idx) ? NODE_FREE : NODE_OCCUPIED;
    else
        cur_state = node_read(idx);

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
            buddy_free = leaf_read(buddy_idx);
        else
            buddy_free = (node_read(buddy_idx) == NODE_FREE);

        if (!buddy_free)
            break;

        if (cur_order == 0) {
            leaf_write(cur_idx, false);
            leaf_write(buddy_idx, false);
        } else {
            node_write(cur_idx, NODE_NONEXIST);
            node_write(buddy_idx, NODE_NONEXIST);
        }
        free_count[cur_order] -= 2;

        node_write(parent_idx, NODE_FREE);
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
            uint8_t ps = node_read(pi);
            if (ps != NODE_OCCUPIED) break;
            uint8_t cc = (wo == 0)
                ? (leaf_read(pi << 1) ? NODE_FREE : NODE_OCCUPIED)
                : node_read(pi << 1);
            if (cc == NODE_NONEXIST) break;
            node_write(pi, NODE_NONLEAF);
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
        cur_state = leaf_read(idx) ? NODE_FREE : NODE_OCCUPIED;
    else
        cur_state = node_read(idx);

    if (cur_state != NODE_OCCUPIED) {
        KURD_t k;
        k.result = result_code::FAIL;
        kurd = k;
        return ERROR_MARK;
    }

    if (order == 0)
        leaf_write(idx, true);
    else
        node_write(idx, NODE_FREE);

    free_count[order]++;

    uint8_t final_order = coalesce_internal(idx, order);

    KURD_t k;
    k.result = result_code::SUCCESS;
    kurd = k;
    return final_order;
}
