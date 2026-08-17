#include "util/BuddyControlBlock_foundation.h"
#include "abi/src_loc.h"
#include "util/OS_utils.h"   // ksetmem_8（pure_init 共享实现）

// ════════════════════════════════════════════════════════════════
// BuddyControlBlock_foundation 基类实现
//
// 所有派生类共享的位图访问、索引辅助、btree_validation
// ════════════════════════════════════════════════════════════════

// ================================================================
// 纯洁初始化（派生类共享：原 ShallowFirst/DeepFirst 实现完全一致，收敛到基类）
// ================================================================

void BuddyControlBlock_foundation::pure_init(
    vaddr_t bitmap_va, uint8_t max_order_val)
{
    max_order = max_order_val;

    const uint64_t total_bits = (3ull << max_order);
    const uint64_t u64_count  = (total_bits + 63) >> 6;

    bitmap = reinterpret_cast<uint64_t*>(bitmap_va);
    ksetmem_8(bitmap, 0, u64_count * sizeof(uint64_t));
    state=STATE_ADULT;
    node_write(1, NODE_FREE);

    for (uint8_t i = 0; i < ORDER_COUNT; i++)
        free_count[i] = 0;
    free_count[max_order] = 1;
}

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

bool BuddyControlBlock_foundation::order0_bit_test(uint64_t idx) const
{
    const uint64_t boff = (2ull << max_order) + idx;
    return (bitmap[boff >> 6] >> (boff & 63)) & 1;
}

void BuddyControlBlock_foundation::order0_bit_set(uint64_t idx, bool val)
{
    const uint64_t boff = (2ull << max_order) + idx;
    uint64_t& w = bitmap[boff >> 6];
    const uint8_t sh = boff & 63;
    if (val)
        w |= (1ull << sh);
    else
        w &= ~(1ull << sh);
}

BuddyControlBlock_foundation::node_state_t
BuddyControlBlock_foundation::higher_node_get(uint64_t idx, uint8_t order) const
{
    const uint64_t heap_idx = order_offset_to_idx(order, idx);
    return static_cast<node_state_t>(node_read(heap_idx));
}

void BuddyControlBlock_foundation::higher_node_set(
    uint64_t idx, uint8_t order, node_state_t val)
{
    const uint64_t heap_idx = order_offset_to_idx(order, idx);
    node_write(heap_idx, val);
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
    if (order == 0)
        return order0_bit_test(offset);
    uint64_t idx = order_offset_to_idx(order, offset);
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
        if (order0_bit_test(o))
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

    auto clear_higher_descendants = [this](uint8_t root_order, uint64_t root_off) {
        for (uint8_t o = 1; o < root_order; o++) {
            const uint64_t first = root_off << (root_order - o);
            const uint64_t count = 1ull << (root_order - o);
            for (uint64_t i = 0; i < count; i++)
                higher_node_set(first + i, o, NODE_NONEXIST);
        }
    };

    // 自底向上：order 1 .. max_order
    const uint8_t seed_order = (max_order >= 6) ? 6 : 1;

    if (seed_order == 6) {
        const uint64_t order0_bit_base = 2ull << max_order;
        const uint64_t order6_count = 1ull << (max_order - 6);
        for (uint64_t g = 0; g < order6_count; g++) {
            const uint64_t word = bitmap[(order0_bit_base >> 6) + g];

            if (word == ~0ull) {
                clear_higher_descendants(6, g);
                higher_node_set(g, 6, NODE_FREE);
                free_count[6]++;
                bitmap[(order0_bit_base >> 6) + g] = 0;
                continue;
            }

            if (word == 0) {
                clear_higher_descendants(6, g);
                higher_node_set(g, 6, NODE_OCCUPIED);
                continue;
            }

            for (uint8_t k = 1; k <= 6; k++) {
                const uint64_t level_count = 1ull << (6 - k);
                const uint64_t level_base = g << (6 - k);
                for (uint64_t j = 0; j < level_count; j++) {
                    const uint64_t off = level_base + j;
                    if (k == 1) {
                        bool lf = order0_bit_test(off << 1);
                        bool rf = order0_bit_test((off << 1) | 1);
                        if (lf && rf) {
                            higher_node_set(off, k, NODE_FREE);
                            free_count[1]++;
                            order0_bit_set(off << 1, false);
                            order0_bit_set((off << 1) | 1, false);
                        } else if (!lf && !rf) {
                            higher_node_set(off, k, NODE_OCCUPIED);
                        } else {
                            higher_node_set(off, k, NODE_NONLEAF);
                        }
                    } else {
                        const uint8_t child_order = k - 1;
                        const uint64_t left_off = off << 1;
                        const uint64_t right_off = (off << 1) | 1;
                        uint8_t ls = higher_node_get(left_off, child_order);
                        uint8_t rs = higher_node_get(right_off, child_order);
                        if (ls == NODE_FREE && rs == NODE_FREE) {
                            higher_node_set(off, k, NODE_FREE);
                            free_count[k]++;
                            free_count[k - 1] -= 2;
                            higher_node_set(left_off, child_order, NODE_NONEXIST);
                            higher_node_set(right_off, child_order, NODE_NONEXIST);
                        } else if (ls == NODE_OCCUPIED && rs == NODE_OCCUPIED) {
                            higher_node_set(off, k, NODE_OCCUPIED);
                        } else {
                            higher_node_set(off, k, NODE_NONLEAF);
                        }
                    }
                }
            }
        }
    }

    for (uint8_t k = seed_order; k <= max_order; k++) {
        if (seed_order == 6 && k == 6)
            continue;
        const uint64_t level_count = 1ull << (max_order - k);  // order-k 节点个数
        for (uint64_t j = 0; j < level_count; j++) {
            if (k == 1) {
                bool lf = order0_bit_test(j << 1);
                bool rf = order0_bit_test((j << 1) | 1);
                if (lf && rf) {
                    higher_node_set(j, k, NODE_FREE);
                    free_count[1]++;
                    order0_bit_set(j << 1, false);
                    order0_bit_set((j << 1) | 1, false);
                } else if (!lf && !rf) {
                    higher_node_set(j, k, NODE_OCCUPIED);
                } else {
                    higher_node_set(j, k, NODE_NONLEAF);
                }
            } else {
                const uint8_t child_order = k - 1;
                const uint64_t left_off = j << 1;
                const uint64_t right_off = (j << 1) | 1;
                uint8_t ls = higher_node_get(left_off, child_order);
                uint8_t rs = higher_node_get(right_off, child_order);
                if (ls == NODE_FREE && rs == NODE_FREE) {
                    higher_node_set(j, k, NODE_FREE);
                    free_count[k]++;
                    free_count[k - 1] -= 2;   // 双子不再空闲，合并到 order-k
                    higher_node_set(left_off, child_order, NODE_NONEXIST);
                    higher_node_set(right_off, child_order, NODE_NONEXIST);
                } else if (ls == NODE_OCCUPIED && rs == NODE_OCCUPIED) {
                    higher_node_set(j, k, NODE_OCCUPIED);
                } else {
                    higher_node_set(j, k, NODE_NONLEAF);
                }
            }
        }
    }

    // 重算 free_count[0]：合并后剩余的空闲叶子
    const uint64_t leaf_cnt = 1ull << max_order;
    uint64_t free_leaf = 0;
    if (max_order >= 6) {
        const uint64_t order0_word_base = (2ull << max_order) >> 6;
        const uint64_t order0_word_count = leaf_cnt >> 6;
        for (uint64_t w = 0; w < order0_word_count; w++)
            free_leaf += __builtin_popcountll(bitmap[order0_word_base + w]);
    } else {
        for (uint64_t o = 0; o < leaf_cnt; o++)
            if (order0_bit_test(o))
                free_leaf++;
    }
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
        if (order0_bit_test(o)) {
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
        order0_bit_set(o, false);
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
        if (order0_bit_test(o))
            return SRC_LOC();
    }
    for (uint64_t o = offset; o < offset + return_count; o++)
        order0_bit_set(o, true);
    free_count[0] += return_count;
    return 0;
}
// ================================================================
// 内联辅助：统计 [bit_off, bit_off+bit_cnt) 区间内的置位位数
// 以 u64 字为单位逐字 popcount，头部/尾部部分字做掩码裁剪
// ================================================================

static inline uint64_t count_set_bits_range(
    const uint64_t* words, uint64_t bit_off, uint64_t bit_cnt)
{
    if (bit_cnt == 0) return 0;

    const uint64_t w0 = bit_off >> 6;
    const uint64_t w1 = (bit_off + bit_cnt - 1) >> 6;

    uint64_t total = 0;
    for (uint64_t w = w0; w <= w1; w++) {
        uint64_t word = words[w];
        if (w == w0 && (bit_off & 63))               // 头部部分字：屏蔽低端
            word &= ~0ULL << (bit_off & 63);
        if (w == w1 && ((bit_off + bit_cnt) & 63))   // 尾部部分字：屏蔽高端
            word &= (1ULL << ((bit_off + bit_cnt) & 63)) - 1;
        total += __builtin_popcountll(word);
    }
    return total;
}

// ================================================================
// inherit_init — 继承初始化：整块位图原样接管，置幼年态（纯 order-0 模式）
//
// 与 init_from_leaves 的差异：
//   - init_from_leaves：收养路径——外部叶子已写实、内部节点区清零，
//     只重算 free_count[0]，不触碰位图内容。
//   - inherit_init    ：整块位图（含内部节点区）原样继承，内部节点不预清零；
//                       状态置 JUVENILE，外部必须遵守纯 order-0 模式
//                       （只有 order-0 叶子位图区参与分配/归还）。
//                       free_count[0] 由叶子位图区以 u64 字单位 popcount 统计。
//
// order-0 叶子位图区 = 位偏移 [2<<max_order, 3<<max_order)
// ================================================================

void BuddyControlBlock_foundation::inherit_init(vaddr_t bitmap_va, uint8_t max_order_val)
{
    max_order = max_order_val;
    bitmap    = reinterpret_cast<uint64_t*>(bitmap_va);

    for (uint8_t i = 0; i < ORDER_COUNT; i++)
        free_count[i] = 0;

    free_count[0] = count_set_bits_range(bitmap, 2ull << max_order, 1ull << max_order);

    state = STATE_JUVENILE;
}
