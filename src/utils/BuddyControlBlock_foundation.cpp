#include "util/BuddyControlBlock_foundation.h"
#include "util/OS_utils.h"

// ════════════════════════════════════════════════════════════════
// v4 伙伴系统底座实现
//
// 位图内存格式 (N = max_order)：
//   [0, 2^(N+1))      非 order0 节点区 — 2-bit/node, idx ∈ [0, 2^N)
//   [2^(N+1), 3·2^N)   order0 节点区   — 1-bit/node, idx ∈ [2^N, 2^(N+1))
//
// heap 索引布局（非 order0 区）：
//   idx 1           → order=N        (root)
//   idx 2~3         → order=N−1
//   idx 4~7         → order=N−2 
//   ...
//   idx [2^N−1]     → order=1
//   idx [2^N..2^(N+1)) → order=0 (leaf 区，用 leaf_read/write)
//
// KURD 错误树：module_code=INFRA, in_module_location=BCB_FOUNDATION
// 详见 BuddyControlBlock_foundation.h 中 infrastructure_location_code::BCB_foundation 命名空间
// ════════════════════════════════════════════════════════════════

using namespace infrastructure_location_code::BCB_foundation;

// ================================================================
// KURD 三阶段构造模板
// ================================================================

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
// 底层位图访问
// ================================================================

uint8_t BuddyControlBlock_foundation::node_read(uint64_t heap_idx) const
{
    // idx ∈ [0, 2^N)，2-bit 从 bitmap[ 2i/64 ] 的 bit[ 2i%64 ] 起
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

bool BuddyControlBlock_foundation::is_free(uint8_t order, uint64_t offset) const
{
    uint64_t idx = order_offset_to_idx(order, offset);
    if (order == 0)
        return leaf_read(idx);
    return node_read(idx) == NODE_FREE;
}

// ================================================================
// 初始化 — event_code = INIT (0)
// ================================================================

void BuddyControlBlock_foundation::init(vaddr_t bitmap_va, uint8_t max_order_val)
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

    // init 成功，不返回 KURD — 注：初始化逻辑简单，无失败路径
}

// ================================================================
// DFS 只读查找 — 内部递归
// ================================================================

uint64_t BuddyControlBlock_foundation::dfs_find_free(
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
// find_candidate — event_code = FIND_CANDIDATE (1)
// ================================================================

uint64_t BuddyControlBlock_foundation::find_candidate(
    uint8_t& base_order, KURD_t& kurd)
{
    // 阶段一、二：取模板 + 注入 event_code
    KURD_t error = default_error();
    error.event_code = FIND_CANDIDATE;

    // Step 1: free_count 预跳
    uint8_t start_order = base_order;
    while (start_order <= max_order && free_count[start_order] == 0)
        start_order++;

    if (start_order > max_order) {
        // 阶段三：填 reason
        using namespace FIND_CANDIDATE_RESULTS::fail_reasons;
        base_order = ERROR_MARK;
        error.reason = no_more_candidate;
        kurd = error;
        return INVALID_OFFSET;
    }

    // Step 2: DFS
    uint64_t found_idx = dfs_find_free(1, start_order);

    if (found_idx == INVALID_OFFSET) {
        // 防御性校验：free_count 有数但 DFS 找不到 → 树结构异常
        KURD_t fatal = default_fatal();
        fatal.event_code = FIND_CANDIDATE;
        fatal.reason = common_fatal_reasons::FREE_COUNT_VIOLAITON;
        base_order = ERROR_MARK + 1;
        kurd = fatal;
        return INVALID_OFFSET;
    }

    // Step 3: 解码返回
    uint8_t  found_order;
    uint64_t found_offset;
    idx_to_order_offset(found_idx, found_order, found_offset);

    // 成功路径
    KURD_t success = default_success();
    success.event_code = FIND_CANDIDATE;
    base_order = found_order;
    kurd = success;
    return found_offset;
}

// ================================================================
// split — event_code = SPLIT (2)
// ================================================================

KURD_t BuddyControlBlock_foundation::split_internal(
    uint64_t idx, uint8_t order, uint8_t target_order)
{
    // 阶段一：取模板（event_code 由外部 fill 或内部推断）
    // 注意：此函数只被 split() 调用，不对外暴露
    // 内部的 fatal/error 应由调用方 split() 的 KURD 模板承载
    // 这里用匿名 KURD 只做内部 return，真正 carry 出去的交由调用方包装

    while (order > target_order) {
        uint8_t child_order = order - 1;
        uint64_t left_idx   = idx << 1;
        uint64_t right_idx  = (idx << 1) | 1;

        // 校验1: 当前节点 NODE_FREE
        if (node_read(idx) != NODE_FREE) {
            KURD_t fatal = default_fatal();
            fatal.event_code = SPLIT;
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            return fatal;
        }

        // 校验2: 子节点必须 NODE_NONEXIST（从未分裂）
        if (child_order > 0) {
            if (node_read(left_idx)  != NODE_NONEXIST ||
                node_read(right_idx) != NODE_NONEXIST) {
                KURD_t fatal = default_fatal();
                fatal.event_code = SPLIT;
                fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                return fatal;
            }
        } else {
            // order0 叶子：应未使用 (leaf=0)
            if (leaf_read(left_idx) || leaf_read(right_idx)) {
                KURD_t fatal = default_fatal();
                fatal.event_code = SPLIT;
                fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
                return fatal;
            }
        }

        // 分裂操作
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

KURD_t BuddyControlBlock_foundation::split(
    uint8_t order, uint64_t offset, uint8_t target_order)
{
    using namespace SPLIT_INTERVAL_RESULTS::fail_reasons;

    // 阶段一、二
    KURD_t success = default_success();
    KURD_t error   = default_error();
    KURD_t fatal   = default_fatal();
    success.event_code = SPLIT;
    error.event_code   = SPLIT;
    fatal.event_code   = SPLIT;

    // 参数校验
    if (order > max_order || target_order > order) {
        error.reason = common_fail_reasons::ORDER_OUT_OF_RANGE;
        return error;
    }

    if (order == target_order) {
        // 无需分裂，直接成功
        success.event_code = SPLIT;
        return success;
    }

    uint64_t idx = order_offset_to_idx(order, offset);

    // 校验：当前节点必须为 NODE_FREE
    uint8_t cur_state = node_read(idx);
    if (cur_state != NODE_FREE) {
        error.reason = TARGET_NOT_FREE;
        return error;
    }

    // 委托 internal
    KURD_t internal = split_internal(idx, order, target_order);
    if (!success_all_kurd(internal)) {
        return internal;  // 原始 KURD 向上透传（含 fatal）
    }

    return success;
}

// ================================================================
// order_occupy_try — event_code = OCCUPY_TRY (3)
// ================================================================

KURD_t BuddyControlBlock_foundation::occupy_internal(
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
    // 如果父 NONLEAF 的两个子都是 OCCUPIED → 父坍缩为 OCCUPIED
    // DFS 看到 OCCUPIED 直接剪枝，不会扫进死子树
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

KURD_t BuddyControlBlock_foundation::order_occupy_try(
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
// order_return — event_code = ORDER_RETURN (4)
// ================================================================

uint8_t BuddyControlBlock_foundation::coalesce_internal(
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

        // 校验1: 当前节点应 NODE_FREE
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

        // 校验2: 父节点必须 NODE_NONLEAF（被坍缩的 OCCUPIED 也允许）
        uint8_t parent_state = node_read(parent_idx);
        if (parent_state != NODE_NONLEAF && parent_state != NODE_OCCUPIED) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 4;
        }

        // 兄弟检查
        bool buddy_free;
        if (cur_order == 0)
            buddy_free = leaf_read(buddy_idx);
        else
            buddy_free = (node_read(buddy_idx) == NODE_FREE);

        if (!buddy_free)
            break;

        // 合并操作
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

        // 校验3: 写入确认
        if (node_read(parent_idx) != NODE_FREE) {
            fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
            kurd = fatal;
            return ERROR_MARK + 5;
        }
    }

    // ── Phase 2: 坍缩展开 ──
    // 合并中止后，如果父/祖先被坍缩为 OCCUPIED（子全占但实际已释放一个），
    // 需要展开为 NONLEAF 并向上传播。
    {
        uint8_t  wo = cur_order;
        uint64_t wi = cur_idx;
        while (wo < max_order) {
            uint64_t pi = wi >> 1;
            if (pi < 1) break;
            uint8_t ps = node_read(pi);
            if (ps != NODE_OCCUPIED) break;
            // 防御校验：确是坍塌（children 非 NONEXIST）
            uint8_t cc = (wo == 0)
                ? (leaf_read(pi << 1) ? NODE_FREE : NODE_OCCUPIED)
                : node_read(pi << 1);
            if (cc == NODE_NONEXIST) break;  // 真正整块占用
            node_write(pi, NODE_NONLEAF);
            wi = pi;
            wo = wo + 1;
        }
        // 尾部校验
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

uint8_t BuddyControlBlock_foundation::order_return(
    uint8_t order, uint64_t offset, KURD_t& kurd)
{
    KURD_t error = default_error();
    KURD_t fatal = default_fatal();
    error.event_code = ORDER_RETURN;
    fatal.event_code = ORDER_RETURN;

    uint64_t idx = order_offset_to_idx(order, offset);

    // 校验1: 当前节点应为 OCCUPIED
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

    // 标记为 free
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

    // 校验2: 父节点一致性（应有 NODE_NONLEAF）
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

    // 向上折叠
    return coalesce_internal(idx, order, kurd);
}

// ================================================================
// btree_validation — event_code = TREE_VALIDATION (5)
// 从根遍历校验二叉树 + free_count 一致性
// ================================================================

bool BuddyControlBlock_foundation::validate_subtree(
    uint64_t idx, uint8_t order, uint64_t count[]) const
{
    if (order == 0) {
        // 叶子层：计数空闲页
        if (leaf_read(idx))
            count[0]++;
        return true;
    }

    uint8_t state = node_read(idx);

    switch (state) {

    // ── 规则1 ──
    // (a) NODE_NONEXIST / NODE_FREE：子树必须全空
    // (b) NODE_OCCUPIED：可接受两种子情况：
    //     b1. children NONEXIST → 真正整块占用
    //     b2. children OCCUPIED → 占用坍缩产物，递归验证
    case NODE_NONEXIST:
    case NODE_FREE: {
        if (state == NODE_FREE)
            count[order]++;
        if (order == 1)
            return !leaf_read(idx << 1) && !leaf_read((idx << 1) | 1);
        if (node_read(idx << 1) != NODE_NONEXIST ||
            node_read((idx << 1) | 1) != NODE_NONEXIST)
            return false;
        return true;
    }

    case NODE_OCCUPIED: {
        // 真正整块占用 vs 坍缩 OCCUPIED 的区别：children 状态
        if (order == 1) {
            // 叶子级：必须全 0（不论哪种情况）
            return !leaf_read(idx << 1) && !leaf_read((idx << 1) | 1);
        }
        uint8_t ls = node_read(idx << 1);
        uint8_t rs = node_read((idx << 1) | 1);
        if (ls == NODE_NONEXIST && rs == NODE_NONEXIST)
            return true;  // b1. 真正整块占用
        if (ls == NODE_OCCUPIED && rs == NODE_OCCUPIED) {
            // b2. 坍缩 → 递归验证子节点
            bool lv = validate_subtree(idx << 1, order - 1, count);
            bool rv = validate_subtree((idx << 1) | 1, order - 1, count);
            return lv && rv;
        }
        return false;  // 混合状态 → 非法
    }

    // ── 规则2 ──
    // NODE_NONLEAF 的孩子不能有 NODE_NONEXIST
    // 且不能同为 NODE_FREE（应合并）或同为 NODE_OCCUPIED（应坍缩）
    case NODE_NONLEAF: {
        if (order == 1) {
            // 孩子是 order0 叶子：两者不能相同值
            //   (1,1) → 父应 FREE, (0,0) → 父应 OCCUPIED
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
    KURD_t fatal = default_fatal();
    fatal.event_code = TREE_VALIDATION;

    // 阶段一：清零临时计数
    uint64_t count[ORDER_COUNT] = {0};

    // 阶段二：完整树遍历校验 + 计数
    if (!validate_subtree(1, max_order, count)) {
        fatal.reason = common_fatal_reasons::BTREE_VIOLATION;
        return fatal;
    }

    // 阶段三：free_count 一致性校验
    for (uint8_t o = 0; o <= max_order; o++) {
        if (count[o] != free_count[o]) {
            fatal.reason = common_fatal_reasons::FREE_COUNT_VIOLAITON;
            return fatal;
        }
    }

    KURD_t success = default_success();
    success.event_code = TREE_VALIDATION;
    return success;
}
