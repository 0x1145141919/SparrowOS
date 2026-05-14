#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/AddresSpace.h"
#include "util/OS_utils.h"
#include <cstring>

// ════════════════════════════════════════════════════════════════
// HCB_v3 — BCB-based Heap Control Block 实现
//
// 移植自 HCB_v3 独立项目: mixed_bitmap_v2 (heap-encoded 二叉树位图)
// + BCB core (cache-first-then-scan, conanico_free, split)
// ════════════════════════════════════════════════════════════════

// first_linekd_heap 的静态 BSS 数据
constexpr uint32_t DEFAULT_FIRST_HEAP_SIZE  = 1 << 22;  // 4MB
constexpr uint32_t DEFAULT_FIRST_BITMAP_SIZE = DEFAULT_FIRST_HEAP_SIZE / 128;
uint8_t first_heap[DEFAULT_FIRST_HEAP_SIZE];
uint8_t first_heap_bitmap[DEFAULT_FIRST_BITMAP_SIZE];

// ═══ mixed_bitmap_v2 ═══

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
    uint64_t fu64 = start >> 6, lu64 = (end - 1) >> 6;
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

// ═══ mixed_bitmap_v2 — 完整 online (含物理页分配+映射) ═══
KURD_t kpoolmemmgr_t::mixed_bitmap_v2::online(vaddr_t bitmap_va, uint8_t out_order_val)
{
    // bitmap_va 必须至少 4K 对齐
    if (bitmap_va & 0xFFF) {
        KURD_t k; k.result=result_code::FAIL; k.level=level_code::ERROR; return k;
    }
    out_order = out_order_val;
    const uint64_t total_bits = 1ULL << (out_order + 1);
    bitmap = reinterpret_cast<uint64_t*>(bitmap_va);
    bitmap_size_in_64bit_units = (total_bits + 63) / 64;
    bitmap_used_bit = 0;
    byte_bitmap_base = reinterpret_cast<uint8_t*>(bitmap);

#ifndef TEST_MODE
    // 分配物理页并映射
    uint64_t bitmap_bytes = bitmap_size_in_64bit_units * sizeof(uint64_t);
    uint64_t alloc_size = (bitmap_bytes + 0xFFF) & ~0xFFFULL;

    KURD_t kurd;
    bitmap_pbase = FreePagesAllocator::alloc(alloc_size, BUDDY_ALLOC_ALWAYS_TRY,
                                              page_state_t::kernel_pinned, kurd);
    if (!success_all_kurd(kurd)) return kurd;
    bitmap_allocated_size = alloc_size;

    vm_interval interval = {
        .vbase = bitmap_va,
        .pbase = bitmap_pbase,
        .size  = alloc_size,
        .access = KspacePageTable::PG_RW,
    };
    kurd = KspacePageTable::enable_VMentry(interval);
    if (!success_all_kurd(kurd)) {
        FreePagesAllocator::free(bitmap_pbase, alloc_size);
        bitmap_pbase = 0;
        bitmap_allocated_size = 0;
        return kurd;
    }
    __builtin_memset(bitmap, 0, bitmap_bytes);
#else
    __builtin_memset(bitmap, 0, bitmap_size_in_64bit_units * sizeof(uint64_t));
#endif

    // 清零 bitmap 并初始化根节点
    bit_set(1, true);
    bitmap_used_bit = 1;
    KURD_t k; k.result=result_code::SUCCESS; k.level=level_code::INFO; return k;
}

// ═══ mixed_bitmap_v2 — 用于 linktime_init 等已有现成物理页的场景 ═══
void kpoolmemmgr_t::mixed_bitmap_v2::init_existing(vaddr_t bitmap_va, uint8_t out_order_val)
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

// ═══ mixed_bitmap_v2 — offline (归还物理页) ═══
KURD_t kpoolmemmgr_t::mixed_bitmap_v2::offline()
{
#ifndef TEST_MODE
    KURD_t kurd;
    if (bitmap_pbase != 0 && bitmap_allocated_size != 0) {
        vm_interval interval = {
            .vbase = (vaddr_t)bitmap,
            .pbase = bitmap_pbase,
            .size  = bitmap_allocated_size,
            .access = KspacePageTable::PG_RW,
        };
        kurd=KspacePageTable::disable_VMentry(interval);
        if(error_kurd(kurd))return kurd;
        kurd=FreePagesAllocator::free(bitmap_pbase, bitmap_allocated_size);
        bitmap_pbase = 0;
        bitmap_allocated_size = 0;
    }
#endif
    out_order = 0; bitmap = nullptr; bitmap_size_in_64bit_units = 0;
    bitmap_used_bit = 0; byte_bitmap_base = nullptr;
    KURD_t k; k.result=result_code::SUCCESS; k.level=level_code::INFO; return k;
}

void kpoolmemmgr_t::mixed_bitmap_v2::bit_set0(uint64_t offset, uint8_t order)
{
    uint64_t idx = idx_from_order_offset(out_order, order, offset);
    if (bit_get(idx)) bitmap_used_bit--;
    bit_set(idx, false);
}

void kpoolmemmgr_t::mixed_bitmap_v2::bit_set1(uint64_t offset, uint8_t order)
{
    uint64_t idx = idx_from_order_offset(out_order, order, offset);
    if (!bit_get(idx)) bitmap_used_bit++;
    bit_set(idx, true);
}

bool kpoolmemmgr_t::mixed_bitmap_v2::bit_get(uint64_t offset, uint8_t order)
{
    uint64_t idx = idx_from_order_offset(out_order, order, offset);
    return bitmap_t::bit_get(idx);
}

uint64_t kpoolmemmgr_t::mixed_bitmap_v2::scan_free_block(uint8_t& order)
{
    uint64_t range_end = 1ULL << (1 + out_order - order);
    uint64_t found = u64_scan_interval(bitmap, 1, range_end);
    if (found == ~0ULL) return ~0ULL;
    uint64_t dummy_offset;
    order_offset_from_idx(out_order, found, order, dummy_offset);
    return dummy_offset;
}

// ═══ HCB_v3 ═══

kpoolmemmgr_t::buddy_meta* kpoolmemmgr_t::HCB_v3::meta_from_ptr(void* ptr) const
{
    return reinterpret_cast<buddy_meta*>((uint8_t*)ptr - sizeof(buddy_meta));
}

uint8_t kpoolmemmgr_t::HCB_v3::size_to_order(uint32_t size_with_meta) const
{
    if (size_with_meta == 0) return 0;
    uint64_t units = ((uint64_t)size_with_meta + BYTES_PER_ORDER0 - 1) / BYTES_PER_ORDER0;
    if (units == 1) return 0;
    return (uint8_t)(64 - __builtin_clzll(units - 1));
}

uint64_t kpoolmemmgr_t::HCB_v3::ptr_to_offset(void* ptr) const
{
    uint64_t block_va = (uint64_t)ptr - sizeof(buddy_meta);
    return (block_va - vbase_) / BYTES_PER_ORDER0;
}

// ═══ KURD 位置级模板 — <LOCATION_CODE_KPOOLMEMMGR_HCB_V3> ═══

KURD_t kpoolmemmgr_t::HCB_v3::kurd_default_success()
{
    KURD_t k;
    k.domain            = err_domain::CORE_MODULE;
    k.module_code       = module_code::MEMORY;
    k.in_module_location = MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB_V3;
    k.result            = result_code::SUCCESS;
    k.level             = level_code::INFO;
    return k;
}

KURD_t kpoolmemmgr_t::HCB_v3::kurd_default_error()
{
    KURD_t k;
    k.domain            = err_domain::CORE_MODULE;
    k.module_code       = module_code::MEMORY;
    k.in_module_location = MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB_V3;
    k.result            = result_code::FAIL;
    k.level             = level_code::ERROR;
    return k;
}

KURD_t kpoolmemmgr_t::HCB_v3::kurd_default_fatal()
{
    KURD_t k;
    k.domain            = err_domain::CORE_MODULE;
    k.module_code       = module_code::MEMORY;
    k.in_module_location = MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB_V3;
    k.result            = result_code::FATAL;
    k.level             = level_code::FATAL;
    return k;
}

// ═══ first_linekd_heap: BSS 已有现成物理页, 不分配 ═══
void kpoolmemmgr_t::HCB_v3::linktime_init()
{
    vbase_      = (uint64_t)first_heap;
    total_size_ = HCB_DEFAULT_SIZE;
    max_order_  = MAX_ORDER;
    data_pbase  = 0;              // BSS 数据, 无需分配
    bcb_bitmap.init_existing((vaddr_t)first_heap_bitmap, max_order_);
    for (auto& c : caches_) {
        for (auto& e : c.entries) e = ~0ULL;
        c.cursor = 0;
    }
    __builtin_memset(order_free_count, 0, sizeof(order_free_count));
    order_free_count[MAX_ORDER] = 1;
    stat_alloc = stat_free = stat_alloc_fail = stat_coalesce = stat_split = stat_cache_hit = stat_scan = 0;
    valid = true;
}

#ifdef TEST_MODE
// ═══ HCB_v3 — 用户态测试初始化 (使用预分配内存) ═══
void kpoolmemmgr_t::HCB_v3::test_init(vaddr_t data_va, vaddr_t bitmap_va, uint32_t size)
{
    vbase_      = data_va;
    total_size_ = size;
    max_order_  = MAX_ORDER;
    data_pbase  = 0;
    bcb_bitmap.init_existing(bitmap_va, max_order_);
    for (auto& c : caches_) {
        for (auto& e : c.entries) e = ~0ULL;
        c.cursor = 0;
    }
    __builtin_memset(order_free_count, 0, sizeof(order_free_count));
    order_free_count[MAX_ORDER] = 1;
    stat_alloc = stat_free = stat_alloc_fail = stat_coalesce = stat_split = stat_cache_hit = stat_scan = 0;
    valid = true;
}
#endif

// ═══ HCB_v3 — online (含 data 区域物理页分配+映射) ═══
KURD_t kpoolmemmgr_t::HCB_v3::online(uint32_t size, vaddr_t data_va, vaddr_t bitmap_va)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_ONLINE;
    error.event_code   = EVENT_CODE_ONLINE;

    if (valid) { return error; }

    // data_va 必须至少 2M 对齐
    if (data_va & ((1ULL << 21) - 1)) {
        return error;
    }

    vbase_      = data_va;
    total_size_ = size;
    max_order_  = MAX_ORDER;

#ifndef TEST_MODE
    // 分配 data 区域物理页 (2M 对齐) — 传递原则
    buddy_alloc_params params = BUDDY_ALLOC_ALWAYS_TRY;
    params.align_log2 = 21;
    {
        KURD_t kurd;
        data_pbase = FreePagesAllocator::alloc(size, params, page_state_t::kernel_pinned, kurd);
        if (!success_all_kurd(kurd)) return kurd;
    }

    // 映射 data 区域
    vm_interval interval = {
        .vbase = data_va,
        .pbase = data_pbase,
        .size  = size,
        .access = KspacePageTable::PG_RW,
    };
    {
        KURD_t kurd = KspacePageTable::enable_VMentry(interval);
        if (!success_all_kurd(kurd)) {
            FreePagesAllocator::free(data_pbase, size);
            data_pbase = 0;
            return kurd;
        }
    }

    // bitmap online (内部也做物理页分配+映射) — 传递原则
    KURD_t r = bcb_bitmap.online(bitmap_va, max_order_);
    if (!success_all_kurd(r)) {
        KspacePageTable::disable_VMentry(interval);
        FreePagesAllocator::free(data_pbase, size);
        data_pbase = 0;
        return r;
    }
#else
    bcb_bitmap.init_existing(bitmap_va, max_order_);
#endif

    for (auto& c : caches_) {
        for (auto& e : c.entries) e = ~0ULL;
        c.cursor = 0;
    }
    __builtin_memset(order_free_count, 0, sizeof(order_free_count));
    order_free_count[MAX_ORDER] = 1;
    stat_alloc = stat_free = stat_alloc_fail = stat_coalesce = stat_split = stat_cache_hit = stat_scan = 0;
    valid = true;
    return success;
}

// ═══ HCB_v3 — offline (归还 data + bitmap 物理页) ═══
KURD_t kpoolmemmgr_t::HCB_v3::offline()
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_OFFLINE;
    error.event_code   = EVENT_CODE_OFFLINE;

    if (!valid) { return error; }

#ifndef TEST_MODE
    // bitmap offline (内部归还 bitmap 物理页)
    KURD_t bitmap_kurd = bcb_bitmap.offline();
    if (error_kurd(bitmap_kurd)) return bitmap_kurd;

    // 归还 data 区域物理页
    if (data_pbase != 0) {
        vm_interval interval = {
            .vbase = vbase_,
            .pbase = data_pbase,
            .size  = total_size_,
            .access = KspacePageTable::PG_RW,
        };
        KURD_t kurd = KspacePageTable::disable_VMentry(interval);
        if (error_kurd(kurd)) return kurd;

        kurd = FreePagesAllocator::free(data_pbase, total_size_);
        if (error_kurd(kurd)) return kurd;

        data_pbase = 0;
    }
#endif

    valid = false; total_size_ = 0; vbase_ = 0;
    return success;
}

bool kpoolmemmgr_t::HCB_v3::is_addr_belong(void* addr) const
{
    uint64_t u = (uint64_t)addr;
    return u >= vbase_ && u < vbase_ + total_size_;
}

uint64_t kpoolmemmgr_t::HCB_v3::used_bytes() const { return 0; }
bool kpoolmemmgr_t::HCB_v3::is_full() const { return false; }

// ═══ 缓存 ═══

void kpoolmemmgr_t::HCB_v3::cache_insert(uint8_t order, uint64_t offset)
{
    if (order > max_order_) return;
    auto& cache = caches_[order];
    cache.entries[cache.cursor] = offset;
    cache.cursor = (cache.cursor + 1) % PER_ORDER_CACHE_COUNT;
}

bool kpoolmemmgr_t::HCB_v3::cache_pick(uint8_t order, uint64_t& out_offset)
{
    if (order > max_order_) return false;
    auto& cache = caches_[order];
    for (uint8_t i = 0; i < PER_ORDER_CACHE_COUNT; ++i) {
        uint64_t entry = cache.entries[i];
        if (entry == ~0ULL) continue;
        if (bcb_bitmap.bit_get(entry, order)) {
            cache.entries[i] = ~0ULL;
            out_offset = entry;
            return true;
        }
        cache.entries[i] = ~0ULL;
    }
    return false;
}

// ═══ free_page_without_merge — 三位一体: 设位 + 计数 + 入缓存 ═══

void kpoolmemmgr_t::HCB_v3::free_page_without_merge(uint64_t offset, uint8_t order)
{
    bcb_bitmap.bit_set1(offset, order);
    order_free_count[order]++;
    cache_insert(order, offset);
}

// ═══ internal_split — 分裂 (含 count 维护) ═══

KURD_t kpoolmemmgr_t::HCB_v3::internal_split(uint64_t offset, uint8_t from_order, uint8_t to_order)
{
    uint64_t cur_off = offset;
    for (uint8_t o = from_order; o > to_order; --o) {
        // 父块不再空闲
        bcb_bitmap.bit_set0(cur_off, o);
        order_free_count[o]--;

        // 右子块标记空闲 (三位一体)
        uint64_t right_off = (cur_off << 1) | 1;
        free_page_without_merge(right_off, o - 1);
        order_free_count[o - 1]++;  // 额外 +1 平衡 alloc 时的 --

        cur_off <<= 1;
        stat_split++;
    }
    KURD_t k; k.result=result_code::SUCCESS; return k;
}

// ═══ internal_alloc — 强一致分配 (cache-first-then-scan) ═══
// 返回的 KURD 遵循传递原则：由调用者(alloc)按原样向上透传

KURD_t kpoolmemmgr_t::HCB_v3::internal_alloc(uint64_t& out_offset, uint8_t order)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;

    KURD_t success = kurd_default_success();
    KURD_t fail    = kurd_default_error();
    success.event_code = EVENT_CODE_INTERNAL_ALLOC;
    fail.event_code    = EVENT_CODE_INTERNAL_ALLOC;

    // 先试缓存 (所有 order)
    for (uint8_t i = order; i <= max_order_; i++) {
        uint64_t cached_off = 0;
        if (cache_pick(i, cached_off)) {
            stat_cache_hit++;
            if (i > order) {
                KURD_t r = internal_split(cached_off, i, order);
                if (!success_all_kurd(r)) return r;
                cached_off <<= (i - order);
            }
            bcb_bitmap.bit_set0(cached_off, order);
            order_free_count[order]--;
            out_offset = cached_off;
            return success;
        }
    }

    // 全部 cache miss → 扫描位图
    stat_scan++;
    uint8_t found_order = order;
    uint64_t found_off = bcb_bitmap.scan_free_block(found_order);
    if (found_off != ~0ULL) {
        if (found_order > order) {
            KURD_t r = internal_split(found_off, found_order, order);
            if (!success_all_kurd(r)) return r;
            found_off <<= (found_order - order);
        }
        bcb_bitmap.bit_set0(found_off, order);
        order_free_count[order]--;
        out_offset = found_off;
        return success;
    }

    fail.reason = INTERNAL_ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_BUDDY;
    return fail;
}

// ═══ internal_free — 强一致释放 (conanico_free, 含 count 维护) ═══
// 返回的 KURD 遵循传递原则：由调用者(free)按原样向上透传

KURD_t kpoolmemmgr_t::HCB_v3::internal_free(uint64_t offset, uint8_t order)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;

    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    KURD_t fatal   = kurd_default_fatal();
    success.event_code = EVENT_CODE_INTERNAL_FREE;
    error.event_code   = EVENT_CODE_INTERNAL_FREE;
    fatal.event_code   = EVENT_CODE_INTERNAL_FREE;

    // double free check
    if (bcb_bitmap.bit_get(offset, order)) {
        error.reason = FREE_RESULTS::FATAL_REASONS::DOUBLE_FREE_DETECT;
        return error;
    }

    uint64_t cur_off = offset;
    uint8_t  cur_ord = order;

    // 向上折叠：中间步骤只维护位图和计数，不入缓存
    while (cur_ord < max_order_) {
        uint64_t buddy_off = cur_off ^ 1;
        if (!bcb_bitmap.bit_get(buddy_off, cur_ord)) break;

        bcb_bitmap.bit_set0(cur_off, cur_ord);
        bcb_bitmap.bit_set0(buddy_off, cur_ord);
        order_free_count[cur_ord] -= 2;

        cur_off >>= 1;
        cur_ord++;

        // 父块不应已存在
        if (bcb_bitmap.bit_get(cur_off, cur_ord)) {
            fatal.reason = FREE_RESULTS::FATAL_REASONS::MERGE_BUT_ALREADY_FREE;
            return fatal;
        }

        stat_coalesce++;
    }

    // 折叠结束：最终块三位一体（设位 + 计数 + 入缓存）
    free_page_without_merge(cur_off, cur_ord);

    return success;
}

// ═══ alloc / free / realloc / clear ═══

KURD_t kpoolmemmgr_t::HCB_v3::alloc(void*& addr, uint32_t size, alloc_flags_t flags)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;

    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_ALLOC;
    error.event_code   = EVENT_CODE_ALLOC;

    if (!valid) { stat_alloc_fail++; addr = nullptr; return error; }
    if (size == 0) {
        stat_alloc_fail++; addr = nullptr;
        error.reason = ALLOC_RESULTS::FAIL_REASONS::REASON_CODE_SIZE_IS_ZERO;
        return error;
    }

    uint32_t real_need = size + sizeof(buddy_meta);
    if (real_need > total_size_) {
        stat_alloc_fail++; addr = nullptr;
        error.reason = ALLOC_RESULTS::FAIL_REASONS::REASON_CODE_SIZE_TOO_LARGE;
        return error;
    }

    uint8_t order = size_to_order(real_need);
    uint64_t offset = 0;
    KURD_t r = internal_alloc(offset, order);
    if (!success_all_kurd(r)) { stat_alloc_fail++; addr = nullptr; return r; }  // 传递原则

    uint64_t order0_off = offset << order;
    vaddr_t block_va = vbase_ + order0_off * BYTES_PER_ORDER0;
    buddy_meta* meta = reinterpret_cast<buddy_meta*>(block_va);
    meta->data_size = size;
    meta->flags     = *reinterpret_cast<uint8_t*>(&flags);
    meta->magic     = MAGIC_ALLOCATED;
    addr = reinterpret_cast<void*>(block_va + sizeof(buddy_meta));
    stat_alloc++;
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v3::free(void* ptr)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;

    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    KURD_t fatal   = kurd_default_fatal();
    success.event_code = EVENT_CODE_FREE;
    error.event_code   = EVENT_CODE_FREE;
    fatal.event_code   = EVENT_CODE_FREE;

    if (!valid || !ptr || ((uint64_t)ptr & 15)) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }
    if (!is_addr_belong(ptr)) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_ADDR_NOT_THIS_HEAP;
        return error;
    }

    buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) {
        fatal.reason = COMMON_FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }

    uint8_t order = size_to_order(meta->data_size + sizeof(buddy_meta));
    uint64_t order0_off = ptr_to_offset(ptr);
    uint64_t order_off  = order0_off >> order;

    KURD_t r = internal_free(order_off, order);
    if (!success_all_kurd(r)) return r;  // 传递原则

    meta->magic = 0;
    stat_free++;
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v3::realloc(void*& ptr, uint32_t new_size, alloc_flags_t flags)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;

    KURD_t success = kurd_default_success();
    KURD_t fatal   = kurd_default_fatal();
    success.event_code = EVENT_CODE_REALLOC;
    fatal.event_code   = EVENT_CODE_REALLOC;

    if (!ptr) return alloc(ptr, new_size, flags);
    if (new_size == 0) { free(ptr); ptr = nullptr; return success; }

    buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) {
        fatal.reason = COMMON_FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }

    uint8_t old_order = size_to_order(meta->data_size + sizeof(buddy_meta));
    uint8_t new_order = size_to_order(new_size + sizeof(buddy_meta));

    if (old_order == new_order && !flags.is_when_realloc_force_new_addr) {
        meta->data_size = new_size;
        return success;
    }

    void* new_ptr = nullptr;
    KURD_t r = alloc(new_ptr, new_size, flags);
    if (!success_all_kurd(r)) return r;  // 传递原则
    __builtin_memcpy(new_ptr, ptr, (meta->data_size < new_size) ? meta->data_size : new_size);
    free(ptr);
    ptr = new_ptr;
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v3::clear(void* ptr)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;

    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    KURD_t fatal   = kurd_default_fatal();

    success.event_code = EVENT_CODE_CLEAR;
    error.event_code   = EVENT_CODE_CLEAR;
    fatal.event_code   = EVENT_CODE_CLEAR;
    if (!valid || !ptr) { return error; }

    buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) {
        fatal.reason = COMMON_FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }

    uint32_t block_size = BYTES_PER_ORDER0 << size_to_order(meta->data_size + sizeof(buddy_meta));
    __builtin_memset(ptr, 0, block_size - sizeof(buddy_meta));
    return success;
}

// ═══ flush_free_count — 扫描位图校验 order_free_count ═══

KURD_t kpoolmemmgr_t::HCB_v3::flush_free_count()
{
    for (uint8_t o = 0; o <= max_order_; o++) {
        uint64_t actual = 0;
        uint64_t count  = 1ULL << (max_order_ - o);
        for (uint64_t i = 0; i < count; i++) {
            if (bcb_bitmap.bit_get(i, o)) actual++;
        }
        if (actual != order_free_count[o]) {
            KURD_t k; k.result=result_code::FATAL; return k;
        }
    }
    KURD_t k; k.result=result_code::SUCCESS; return k;
}
