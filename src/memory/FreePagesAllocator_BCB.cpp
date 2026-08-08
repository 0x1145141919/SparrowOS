#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "panic.h"
#include "abi/src_loc.h"

// ════════════════════════════════════════════════════════════════
// BCB_v4 移植: BuddyControlBlock_foundation 适配
//   - 移除 mixed_bitmap_v2, 使用 v4 底座 (3×2^N bits, 4-state nodes)
//   - BCB 层缓存: 单 hint/order（last_free_hint），用 is_free 低成本验证
//   - 底座负责树操作: find_candidate + split + order_occupy_try + order_return
// ════════════════════════════════════════════════════════════════

// ================================================================
// 地址归属
// ================================================================

bool FreePagesAllocator::BuddyControlBlock::is_addr_belong_to_this_BCB_no_lock(phyaddr_t addr)
{
    return (addr >= this->base) && (addr < (this->base + (1ull << (max_supprt_order + 12))));
}

// ================================================================
// KURD 模板（空 KURD 占位）
// ================================================================

KURD_t FreePagesAllocator::BuddyControlBlock::default_success()
{
    return KURD_t();
}

KURD_t FreePagesAllocator::BuddyControlBlock::default_error()
{
    return KURD_t();
}

KURD_t FreePagesAllocator::BuddyControlBlock::default_fatal()
{
    return KURD_t();
}

KURD_t FreePagesAllocator::BuddyControlBlock::default_kurd()
{
    return KURD_t();
}

// ================================================================
// size_to_order
// ================================================================

uint8_t FreePagesAllocator::BuddyControlBlock::size_to_order(uint64_t size)
{
    if (size == 0) return 0;
    uint64_t numof_4kbpgs = (size + _4KB_PAGESIZE - 1) / _4KB_PAGESIZE;
    auto next_pow2 = [](uint64_t n) -> uint64_t {
        n--; n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16; n |= n >> 32; n++;
        return n;
    };
    return 63 - __builtin_clzll(next_pow2(numof_4kbpgs));
}

// ================================================================
// 缓存: 单 hint/order, 用 is_free 低成本验证
// ================================================================

void FreePagesAllocator::BuddyControlBlock::cache_insert(uint8_t order, uint64_t idx)
{
    if (order >= DESINGED_MAX_SUPPORT_ORDER) return;
    // 单 hint: 直接覆盖旧条目（最近释放原则）
    for (uint8_t i = 0; i < PER_ORDER_CACHE_SUGGEST_COUNT; ++i) {
        if (suggest_order_free_page_index[order][i] == INVALID_INBCB_INDEX) {
            suggest_order_free_page_index[order][i] = idx;
            return;
        }
    }
    uint8_t& cursor = suggest_order_cache_cursor[order];
    suggest_order_free_page_index[order][cursor] = idx;
    cursor = (cursor + 1) % PER_ORDER_CACHE_SUGGEST_COUNT;
}

bool FreePagesAllocator::BuddyControlBlock::cache_pick(uint8_t order, uint64_t& out_idx)
{
    if (order >= DESINGED_MAX_SUPPORT_ORDER) return false;
    for (uint8_t i = 0; i < PER_ORDER_CACHE_SUGGEST_COUNT; ++i) {
        uint64_t idx = suggest_order_free_page_index[order][i];
        if (idx == INVALID_INBCB_INDEX) continue;
        // 快速验证：检查 (order,idx) 在底座中是否仍为 FREE
        if (!fnd.is_free(order, idx)) {
            // 过期（因折叠合并），移除
            suggest_order_free_page_index[order][i] = INVALID_INBCB_INDEX;
            continue;
        }
        out_idx = idx;
        suggest_order_free_page_index[order][i] = INVALID_INBCB_INDEX;
        statistics.suggest_hit[order]++;
        return true;
    }
    statistics.suggest_miss[order]++;
    return false;
}

// ================================================================
// can_alloc
// ================================================================

bool FreePagesAllocator::BuddyControlBlock::can_alloc(uint8_t order)
{
    // 状态自适应：幼年态只有 free_count[0] 有意义（所需页数 = 1<<order）；
    // 成年态按 per-order 检查（允许从高阶分裂）
    if (fnd.is_juvenile()) {
        return fnd.get_free_count(0) >= (1ull << order);
    }
    for (uint8_t o = order; o <= max_supprt_order; o++) {
        if (fnd.order_exist_check(o)) return true;
    }
    return false;
}

// ================================================================
// 构造
// ================================================================

FreePagesAllocator::BuddyControlBlock::BuddyControlBlock(phyaddr_t base, uint8_t max_support_order)
{
    this->max_supprt_order = max_support_order;
    this->base = base;
    for (uint8_t i = 0; i < DESINGED_MAX_SUPPORT_ORDER; i++) {
        for (uint8_t j = 0; j < PER_ORDER_CACHE_SUGGEST_COUNT; j++)
            suggest_order_free_page_index[i][j] = INVALID_INBCB_INDEX;
        suggest_order_cache_cursor[i] = 0;
    }
    ksetmem_8(&statistics, 0, sizeof(statistics));
    // fnd 尚未初始化——由 corebcb_mixedbitmap_base_acclaim 延迟初始化
}

FreePagesAllocator::BuddyControlBlock::BuddyControlBlock() {}

// ================================================================
// corebcb_mixedbitmap_base_acclaim — 延迟初始化底座
// ================================================================

void FreePagesAllocator::BuddyControlBlock::corebcb_mixedbitmap_base_acclaim(vaddr_t bitmap_base_addr)
{
    fnd.init(bitmap_base_addr, max_supprt_order);
    // 旧版 (v2) 的 free_page_without_merge(0, max_supprt_order) 等价于
    // 底座 init 已自动将根设为 NODE_FREE, free_count[max_order]=1
}

// ================================================================
// corebcb_init_from_leaves — 收养路径（init.elf 穿越位图，叶子已写实）
// 置幼年态；由上层在适当时候调用 fold_up_from_leaves 成年
// ================================================================

void FreePagesAllocator::BuddyControlBlock::corebcb_init_from_leaves(vaddr_t bitmap_base_addr)
{
    fnd.init_from_leaves(bitmap_base_addr, max_supprt_order);
}

// ================================================================
// corebcb_fold_adult — 成年仪式（JUVENILE → ADULT）
// 自底向上填内部节点 + 全 free_count 重算（→ fnd.fold_up_from_leaves）
// ================================================================

KURD_t FreePagesAllocator::BuddyControlBlock::corebcb_fold_adult()
{
    tmp_error_locator e = fnd.fold_up_from_leaves();
    if (e != 0) return default_fatal();
    return default_success();
}

// ================================================================
// juvenile_alloc — 幼年态专用分配（仅 JUVENILE 态可调）
// 扫 order0 叶子位图找 acquire_count 个连续空闲页，成功返回本 BCB 内
// 页偏移对应的物理地址；失败返回 0 并填 kurd。状态不符即 error。
// ================================================================

phyaddr_t FreePagesAllocator::BuddyControlBlock::juvenile_alloc(
    tmp_error_locator& kurd, uint64_t acquire_count)
{
    kurd = 0;
    if (!fnd.is_juvenile()) {
        kurd = SRC_LOC();
        statistics.alloc_times_fail++;
        return 0;
    }
    uint64_t off = fnd.juvenile_alloc_order0(kurd, acquire_count);
    if (kurd != 0 || off == INVALID_INBCB_INDEX) {
        statistics.alloc_times_fail++;
        return 0;
    }
    statistics.alloc_times_success++;
    return base + (off << 12);
}

// ================================================================
// juvenile_free — 幼年态专用释放（仅 JUVENILE 态可调）
// 清 return_count 个叶子 → 维护 free_count[0]。状态不符即 error。
// ================================================================

tmp_error_locator FreePagesAllocator::BuddyControlBlock::juvenile_free(
    uint64_t offset, uint64_t return_count)
{
    if (!fnd.is_juvenile()) {
        return SRC_LOC();
    }
    tmp_error_locator e = fnd.juvenile_free_order0(offset, return_count);
    if (e == 0) statistics.free_times_success++;
    return e;
}

// ================================================================
// allocate_buddy_way — 成年态专用分配（仅 ADULT 态可调）
// 先试缓存，miss 则 fallback 底座。幼年态请走 juvenile_alloc。
// ================================================================

phyaddr_t FreePagesAllocator::BuddyControlBlock::allocate_buddy_way(
    uint64_t size, KURD_t& result, uint8_t align_log2)
{
    KURD_t error = default_error();

    // ── 状态校验：仅成年态允许（幼年态请走 juvenile_alloc） ──
    if (!fnd.is_adult()) {
        result = error;
        statistics.alloc_times_fail++;
        return 0;
    }

    uint8_t order = size_to_order(size);
    uint8_t align_order = align_log2 > 12 ? align_log2 - 12 : 0;
    order = order > align_order ? order : align_order;

    if (order > max_supprt_order || align_order > max_supprt_order) {
        result = error;
        return 0;
    }

    // ── Phase 1: 试缓存 (从低到高扫 orders, is_free 低成本验证) ──
    for (uint8_t i = order; i <= max_supprt_order; i++) {
        uint64_t cached_offset = INVALID_INBCB_INDEX;
        if (!cache_pick(i, cached_offset))
            continue;

        // 缓存命中（cache_pick 已通过 is_free 验证有效）
        uint64_t final_offset = cached_offset;
        if (i > order) {
            result = fnd.split(i, cached_offset, order);
            if (!success_all_kurd(result))
                return 0;
            final_offset = cached_offset << (i - order);
        }

        result = fnd.order_occupy_try(order, final_offset);
        if (!success_all_kurd(result))
            return 0;
        statistics.alloc_times_success++;
        phyaddr_t res_addr = base + (final_offset << (order + 12));
        result = default_success();
        return res_addr;
    }

    // ── Phase 2: 缓存全 miss → 底座 find_candidate ──
    statistics.suggest_miss[order]++;
    statistics.scan_count++;

    uint8_t base_order = order;
    uint64_t offset = fnd.find_candidate(base_order, result);
    if (!success_all_kurd(result)) {
        statistics.alloc_times_fail++;
        return 0;
    }

    // ── Phase 3: 分裂 → 缓存右兄弟链 ──
    if (base_order > order) {
        result = fnd.split(base_order, offset, order);
        if (!success_all_kurd(result))
            return 0;

        // 缓存各级右兄弟
        uint64_t so = offset;
        for (uint8_t o = base_order; o > order + 1; o--) {
            cache_insert(o - 1, so * 2 + 1);
            so = so * 2;
        }
        cache_insert(order, so * 2 + 1);

        offset <<= (base_order - order);
    }

    // ── Phase 4: 占用 ──
    result = fnd.order_occupy_try(order, offset);
    if (!success_all_kurd(result))
        return 0;

    statistics.alloc_times_success++;
    statistics.split_count += (base_order - order);

    phyaddr_t res_addr = base + (offset << (order + 12));
    result = default_success();
    return res_addr;
}

// ================================================================
// free_buddy_way
// ================================================================

KURD_t FreePagesAllocator::BuddyControlBlock::free_buddy_way(phyaddr_t addr, uint64_t size)
{
    KURD_t error = default_error();

    if (!is_addr_belong_to_this_BCB_no_lock(addr) ||
        !is_addr_belong_to_this_BCB_no_lock(addr + size - 1)) {
        return error;
    }

    // ── 状态校验：仅成年态允许（幼年态请走 juvenile_free） ──
    if (!fnd.is_adult()) {
        return error;
    }

    uint8_t order = size_to_order(size);
    uint64_t offset = (addr - this->base) >> (order + 12);

    KURD_t kurd;
    uint8_t final_order = fnd.order_return(order, offset, kurd);
    if (final_order >= 0x40)
        return kurd;  // 错误已由 order_return 填入 kurd

    // 缓存折叠后的最终块（原始 offset 可能已折叠到更高 order）
    uint64_t coalesced_offset = offset >> (final_order - order);
    cache_insert(final_order, coalesced_offset);

    statistics.free_times_success++;
    return kurd;
}

// ================================================================
// free_pages_flush → 底座 btree_validation
// ================================================================

KURD_t FreePagesAllocator::BuddyControlBlock::free_pages_flush()
{
    return fnd.btree_validation();
}

// ================================================================
// is_addr_belong_to_this_BCB
// ================================================================

bool FreePagesAllocator::BuddyControlBlock::is_addr_belong_to_this_BCB(phyaddr_t addr)
{
    return is_addr_belong_to_this_BCB_no_lock(addr);
}

// ================================================================
// 辅助查询
// ================================================================

phyaddr_t FreePagesAllocator::BuddyControlBlock::get_base() { return this->base; }
uint8_t FreePagesAllocator::BuddyControlBlock::get_order() { return this->max_supprt_order; }
uint8_t FreePagesAllocator::BuddyControlBlock::get_max_order() { return this->max_supprt_order; }
bool FreePagesAllocator::BuddyControlBlock::is_bcb_avaliable() { return true; }

// ================================================================
// print 函数
// ================================================================

void FreePagesAllocator::BuddyControlBlock::print_basic_info_no_lock()
{
    bsp_kout << "BCB base=0x" << (void*)(uint64_t)base
             << " max_order=" << (uint32_t)max_supprt_order << kendl;
}

void FreePagesAllocator::BuddyControlBlock::print_bitmap_info_no_lock() {}
void FreePagesAllocator::BuddyControlBlock::print_basic_info()  { print_basic_info_no_lock(); }
void FreePagesAllocator::BuddyControlBlock::print_bitmap_info() {}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_info_compress_no_lock(uint8_t) {}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_interval_compress_no_lock(uint8_t, uint64_t, uint64_t) {}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_info_compress(uint8_t o) { print_bitmap_order_info_compress_no_lock(o); }
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_interval_compress(uint8_t o, uint64_t b, uint64_t l) { print_bitmap_order_interval_compress_no_lock(o, b, l); }
void FreePagesAllocator::BuddyControlBlock::print_all_statistics()
{
    bsp_kout << "  alloc_ok=" << statistics.alloc_times_success
             << " alloc_fail=" << statistics.alloc_times_fail
             << " free_ok=" << statistics.free_times_success
             << " scan=" << statistics.scan_count
             << " split=" << statistics.split_count << kendl;
}
