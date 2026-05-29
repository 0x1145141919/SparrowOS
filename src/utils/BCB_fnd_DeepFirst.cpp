#include "util/BCB_fnd_DeepFirst.h"
#include "util/OS_utils.h"

// ════════════════════════════════════════════════════════════════
// BCB_fnd_DeepFirst 实现
//
// 位图访问、索引辅助、btree_validation 由基类提供
// 本文件仅实现 5 个纯虚分配接口 + 内部辅助方法
// ════════════════════════════════════════════════════════════════

// ================================================================
// KURD 三阶段构造模板（Debug 专用）
// ================================================================

namespace infrastructure_location_code{
    constexpr uint8_t LOCATION_CODE_BCB_FOUNDATION = 1;
    namespace BCB_foundation{
        namespace common_fatal_reasons{
            static constexpr uint8_t BTREE_VIOLATION = 0x1;
            static constexpr uint8_t FREE_COUNT_VIOLAITON = 0x2;
        };
        namespace common_fail_reasons{
            static constexpr uint8_t ORDER_OUT_OF_RANGE = 0x1;
            static constexpr uint8_t TARGET_NOT_FREE = 0x2;
        }
        constexpr uint8_t FIND_CANDIDATE = 1;
        namespace FIND_CANDIDATE_RESULTS::fail_reasons{ 
            static constexpr uint8_t no_more_candidate = 0x20;
        }
        constexpr uint8_t SPLIT = 2;
        namespace SPLIT_INTERVAL_RESULTS::fail_reasons{ 
            static constexpr uint8_t TARGET_NOT_FREE = 0x20;
        }
        constexpr uint8_t OCCUPY_TRY = 3;
        namespace OCCUPY_TRY_RESULTS::fail_reasons{ 
            static constexpr uint8_t TARGET_NOT_FREE = 0x20;
        }
        constexpr uint8_t ORDER_RETURN = 4;
    }
}

using namespace infrastructure_location_code::BCB_foundation;

static KURD_t default_kurd()
{
    return KURD_t(0, 0,
        module_code::INFRA,
        infrastructure_location_code::LOCATION_CODE_BCB_FOUNDATION,
        0, 0,
        err_domain::CORE_MODULE);
}

static KURD_t default_success()
{
    KURD_t k = default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    return k;
}

static KURD_t default_error()
{
    KURD_t k = default_kurd();
    k = set_result_fail_and_error_level(k);
    return k;
}

static KURD_t default_fatal()
{
    KURD_t k = default_kurd();
    k = set_fatal_result_level(k);
    return k;
}

// ================================================================
// 初始化
// ================================================================

void BCB_fnd_DeepFirst::init(
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
// DFS 只读查找
// ================================================================

uint64_t BCB_fnd_DeepFirst::dfs_find_free(
    uint64_t idx, uint8_t target_order) const
{
    uint8_t cur_order = heap_idx_order(idx);
    if (cur_order < target_order)
        return INVALID_OFFSET;

    uint8_t state;
    if (idx < (1ull << max_order))
        state = node_read(idx);
    else
        state = leaf_read(idx) ? NODE_FREE : NODE_OCCUPIED;

    switch (state) {
    case NODE_NONEXIST:
    case NODE_OCCUPIED:
        return INVALID_OFFSET;

    case NODE_FREE:
        return idx;

    case NODE_NONLEAF:
    {
        uint64_t left = dfs_find_free(idx << 1, target_order);
        if (left != INVALID_OFFSET)
            return left;
        return dfs_find_free((idx << 1) | 1, target_order);
    }

    default:
        return INVALID_OFFSET;
    }
}

// ================================================================
// find_candidate
// ================================================================

uint64_t BCB_fnd_DeepFirst::find_candidate(
    uint8_t& base_order, KURD_t& kurd)
{
    KURD_t error = default_error();
    error.event_code = FIND_CANDIDATE;

    uint8_t start_order = base_order;
    while (start_order <= max_order && free_count[start_order] == 0)
        start_order++;

    if (start_order > max_order) {
        using namespace FIND_CANDIDATE_RESULTS::fail_reasons;
        base_order = ERROR_MARK;
        error.reason = no_more_candidate;
        kurd = error;
        return INVALID_OFFSET;
    }

    uint64_t found_idx = dfs_find_free(1, start_order);

    if (found_idx == INVALID_OFFSET) {
        KURD_t fatal = default_fatal();
        fatal.event_code = FIND_CANDIDATE;
        fatal.reason = common_fatal_reasons::FREE_COUNT_VIOLAITON;
        base_order = ERROR_MARK + 1;
        kurd = fatal;
        return INVALID_OFFSET;
    }

    uint8_t  found_order;
    uint64_t found_offset;
    idx_to_order_offset(found_idx, found_order, found_offset);

    KURD_t success = default_success();
    success.event_code = FIND_CANDIDATE;
    base_order = found_order;
    kurd = success;
    return found_offset;
}

// ================================================================
// split
// ================================================================

KURD_t BCB_fnd_DeepFirst::split_internal(
    uint64_t idx, uint8_t order, uint8_t target_order)
{
    while (order > target_order) {
        uint8_t child_order = order - 1;
        uint64_t left_idx   = idx << 1;
        uint64_t right_idx  = (idx << 1) | 1;

        if (node_read(idx) != NODE_FREE) {
            KURD_t fatal = default_fatal();
            fatal.event_code = SPLIT;
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            return fatal;
        }

        if (child_order > 0) {
            if (node_read(left_idx)  != NODE_NONEXIST ||
                node_read(right_idx) != NODE_NONEXIST) {
                KURD_t fatal = default_fatal();
                fatal.event_code = SPLIT;
                fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                return fatal;
            }
        } else {
            if (leaf_read(left_idx) || leaf_read(right_idx)) {
                KURD_t fatal = default_fatal();
                fatal.event_code = SPLIT;
                fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                return fatal;
            }
        }

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

    KURD_t success = default_success();
    success.event_code = SPLIT;
    return success;
}

KURD_t BCB_fnd_DeepFirst::split(
    uint8_t order, uint64_t offset, uint8_t target_order)
{
    using namespace SPLIT_INTERVAL_RESULTS::fail_reasons;

    KURD_t success = default_success();
    KURD_t error   = default_error();
    KURD_t fatal   = default_fatal();
    success.event_code = SPLIT;
    error.event_code   = SPLIT;
    fatal.event_code   = SPLIT;

    if (order > max_order || target_order > order) {
        error.reason = common_fail_reasons::ORDER_OUT_OF_RANGE;
        return error;
    }

    if (order == target_order) {
        return success;
    }

    uint64_t idx = order_offset_to_idx(order, offset);

    uint8_t cur_state = node_read(idx);
    if (cur_state != NODE_FREE) {
        error.reason = TARGET_NOT_FREE;
        return error;
    }

    KURD_t internal = split_internal(idx, order, target_order);
    if (!success_all_kurd(internal)) {
        return internal;
    }

    return success;
}

// ================================================================
// order_occupy_try
// ================================================================

KURD_t BCB_fnd_DeepFirst::occupy_internal(
    uint64_t idx, uint8_t order)
{
    if (order == 0) {
        leaf_write(idx, false);
        if (leaf_read(idx)) {
            KURD_t fatal = default_fatal();
            fatal.event_code = OCCUPY_TRY;
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            return fatal;
        }
    } else {
        node_write(idx, NODE_OCCUPIED);
        if (node_read(idx) != NODE_OCCUPIED) {
            KURD_t fatal = default_fatal();
            fatal.event_code = OCCUPY_TRY;
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            return fatal;
        }
    }
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

    KURD_t success = default_success();
    success.event_code = OCCUPY_TRY;
    return success;
}

KURD_t BCB_fnd_DeepFirst::order_occupy_try(
    uint8_t order, uint64_t offset)
{
    using namespace OCCUPY_TRY_RESULTS::fail_reasons;

    KURD_t success = default_success();
    KURD_t error   = default_error();
    success.event_code = OCCUPY_TRY;
    error.event_code   = OCCUPY_TRY;

    uint64_t idx = order_offset_to_idx(order, offset);

    uint8_t cur_state;
    if (order == 0)
        cur_state = leaf_read(idx) ? NODE_FREE : NODE_OCCUPIED;
    else
        cur_state = node_read(idx);

    if (cur_state != NODE_FREE) {
        error.reason = TARGET_NOT_FREE;
        return error;
    }

    return occupy_internal(idx, order);
}

// ================================================================
// order_return
// ================================================================

uint8_t BCB_fnd_DeepFirst::coalesce_internal(
    uint64_t idx, uint8_t order, KURD_t& kurd)
{
    KURD_t success = default_success();
    KURD_t fatal   = default_fatal();
    success.event_code = ORDER_RETURN;
    fatal.event_code   = ORDER_RETURN;

    uint8_t  cur_order = order;
    uint64_t cur_idx   = idx;

    while (cur_order < max_order) {
        uint64_t buddy_idx  = cur_idx ^ 1;
        uint64_t parent_idx = cur_idx >> 1;

        if (cur_order == 0) {
            if (!leaf_read(cur_idx)) {
                fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                kurd = fatal;
                return ERROR_MARK + 3;
            }
        } else {
            if (node_read(cur_idx) != NODE_FREE) {
                fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                kurd = fatal;
                return ERROR_MARK + 3;
            }
        }

        uint8_t parent_state = node_read(parent_idx);
        if (parent_state != NODE_NONLEAF && parent_state != NODE_OCCUPIED) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 4;
        }

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

        if (node_read(parent_idx) != NODE_FREE) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 5;
        }
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
        if (wo < max_order) {
            uint64_t pi = wi >> 1;
            if (pi >= 1) {
                uint8_t vp = node_read(pi);
                if (vp != NODE_NONLEAF && vp != NODE_FREE &&
                    vp != NODE_OCCUPIED) {
                    fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                    kurd = fatal;
                    return ERROR_MARK + 6;
                }
            }
        }
    }

    kurd = success;
    return cur_order;
}

uint8_t BCB_fnd_DeepFirst::order_return(
    uint8_t order, uint64_t offset, KURD_t& kurd)
{
    KURD_t error = default_error();
    KURD_t fatal = default_fatal();
    error.event_code = ORDER_RETURN;
    fatal.event_code = ORDER_RETURN;

    uint64_t idx = order_offset_to_idx(order, offset);

    uint8_t cur_state;
    if (order == 0)
        cur_state = leaf_read(idx) ? NODE_FREE : NODE_OCCUPIED;
    else
        cur_state = node_read(idx);

    if (cur_state != NODE_OCCUPIED) {
        kurd = error;
        kurd.reason = common_fail_reasons::TARGET_NOT_FREE;
        return ERROR_MARK;
    }

    if (order == 0) {
        leaf_write(idx, true);
        if (!leaf_read(idx)) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 7;
        }
    } else {
        node_write(idx, NODE_FREE);
        if (node_read(idx) != NODE_FREE) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 7;
        }
    }
    free_count[order]++;

    if (order < max_order) {
        uint64_t parent_idx = idx >> 1;
        uint8_t  parent_state = node_read(parent_idx);
        if (parent_state == NODE_NONEXIST ||
            parent_state == NODE_FREE) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 8;
        }
    }

    return coalesce_internal(idx, order, kurd);
}
