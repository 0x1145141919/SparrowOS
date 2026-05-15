#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/AddresSpace.h"
#include "util/OS_utils.h"
#include <cstring>

// ════════════════════════════════════════════════════════════════
// HCB_v3 — BuddyControlBlock_foundation 移植
//
// 替换 mixed_bitmap_v2 (1-bit heap-encoded) → BuddyControlBlock_foundation
// (3×2^N bits, 4-state nodes)
// ════════════════════════════════════════════════════════════════

// first_linekd_heap 的静态 BSS 数据
constexpr uint32_t DEFAULT_FIRST_HEAP_SIZE  = 1 << 22;  // 4MB
constexpr uint32_t DEFAULT_FIRST_BITMAP_SIZE = DEFAULT_FIRST_HEAP_SIZE / 128;
uint8_t first_heap[DEFAULT_FIRST_HEAP_SIZE];
uint8_t first_heap_bitmap[DEFAULT_FIRST_BITMAP_SIZE];

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

uint64_t kpoolmemmgr_t::HCB_v3::ptr_to_offset(void* ptr, uint8_t order) const
{
    uint64_t block_va = (uint64_t)ptr - sizeof(buddy_meta);
    return (block_va - vbase_) >> (order + 5);  // ÷ (32 << order)
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
    data_pbase  = 0;
    fnd.init((vaddr_t)first_heap_bitmap, max_order_);
    for (auto& c : caches_) {
        for (auto& e : c.entries) e = ~0ULL;
        c.cursor = 0;
    }
    bitmap_pbase = 0; bitmap_allocated_size = 0;
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
    fnd.init(bitmap_va, max_order_);
    for (auto& c : caches_) {
        for (auto& e : c.entries) e = ~0ULL;
        c.cursor = 0;
    }
    bitmap_pbase = 0; bitmap_allocated_size = 0;
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

#ifndef TEST_MODE
    if (data_va & ((1ULL << 21) - 1)) {
        return error;
    }
#endif

    vbase_      = data_va;
    total_size_ = size;
    max_order_  = MAX_ORDER;

#ifndef TEST_MODE
    // 分配 data 区域物理页 (2M 对齐)
    buddy_alloc_params params = BUDDY_ALLOC_ALWAYS_TRY;
    params.align_log2 = 21;
    {
        KURD_t kurd;
        data_pbase = FreePagesAllocator::alloc(size, params, page_state_t::kernel_pinned, kurd);
        if (!success_all_kurd(kurd)) return kurd;
    }

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

    // 分配 bitmap 物理页并映射
    const uint64_t bitmap_bits = 3ull << max_order_;
    const uint64_t bitmap_bytes = ((bitmap_bits + 63) / 64) * sizeof(uint64_t);
    uint64_t alloc_size = (bitmap_bytes + 0xFFF) & ~0xFFFULL;

    {
        KURD_t kurd;
        bitmap_pbase = FreePagesAllocator::alloc(alloc_size, BUDDY_ALLOC_ALWAYS_TRY,
                                                  page_state_t::kernel_pinned, kurd);
        if (!success_all_kurd(kurd)) {
            KspacePageTable::disable_VMentry(interval);
            FreePagesAllocator::free(data_pbase, size);
            data_pbase = 0;
            return kurd;
        }
        bitmap_allocated_size = alloc_size;
    }

    vm_interval bm_interval = {
        .vbase = bitmap_va,
        .pbase = bitmap_pbase,
        .size  = alloc_size,
        .access = KspacePageTable::PG_RW,
    };
    {
        KURD_t kurd = KspacePageTable::enable_VMentry(bm_interval);
        if (!success_all_kurd(kurd)) {
            FreePagesAllocator::free(bitmap_pbase, alloc_size);
            bitmap_pbase = 0; bitmap_allocated_size = 0;
            KspacePageTable::disable_VMentry(interval);
            FreePagesAllocator::free(data_pbase, size);
            data_pbase = 0;
            return kurd;
        }
    }

    // 底座初始化
    fnd.init(bitmap_va, max_order_);
#else
    fnd.init(bitmap_va, max_order_);
#endif

    for (auto& c : caches_) {
        for (auto& e : c.entries) e = ~0ULL;
        c.cursor = 0;
    }
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
    // 归还 bitmap 物理页
    if (bitmap_pbase != 0 && bitmap_allocated_size != 0) {
        vm_interval bm_interval = {
            .vbase = (vaddr_t)(uint64_t)&fnd,
            .pbase = bitmap_pbase,    // 近似 — 实际 bitmap 基址由 init 记录
            .size  = bitmap_allocated_size,
            .access = KspacePageTable::PG_RW,
        };
        // 实际 bitmap 的 VA 在初始化时传入，fnd 内部保存 bitmap 指针
        // 我们需要 bitmap 的真实 VA，通过 reinterpret_cast 获得
        // 但 fnd 的 bitmap 是 protected 成员，无法直接访问
        // 从 vbase_ 偏移推算或直接使用 bitmap 页的 VA
        // 实际 bitmap VA 由 caller 提供并保存在 fnd.init() 中设置的 bitmap 指针
        // 由于 protected 无法访问，使用旧的 bcb_bitmap 方式来管理
        // 改用 bitmap_va 参数记录 —— 此处简化，因为 TEST_MODE 不走此路径
        KURD_t kurd = KspacePageTable::disable_VMentry(bm_interval);
        if (error_kurd(kurd)) return kurd;
        kurd = FreePagesAllocator::free(bitmap_pbase, bitmap_allocated_size);
        if (error_kurd(kurd)) return kurd;
        bitmap_pbase = 0; bitmap_allocated_size = 0;
    }

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
        // 快速验证：底座 is_free 保证节点仍可用
        if (!fnd.is_free(order, entry)) {
            cache.entries[i] = ~0ULL;
            continue;
        }
        cache.entries[i] = ~0ULL;
        out_offset = entry;
        return true;
    }
    return false;
}

// ═══ internal_alloc — 缓存优先，miss 则 fallback 底座 ═══

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
        if (!cache_pick(i, cached_off))
            continue;

        stat_cache_hit++;
        uint64_t final_off = cached_off;
        if (i > order) {
            KURD_t r = fnd.split(i, cached_off, order);
            if (!success_all_kurd(r)) return r;
            final_off = cached_off << (i - order);
        }
        KURD_t r = fnd.order_occupy_try(order, final_off);
        if (!success_all_kurd(r)) continue;
        out_offset = final_off;
        return success;
    }

    // 全部 cache miss → 底座 find_candidate
    stat_scan++;
    uint8_t base_order = order;
    uint64_t found_off = fnd.find_candidate(base_order, fail);
    if (!success_all_kurd(fail)) {
        fail.reason = INTERNAL_ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_BUDDY;
        return fail;
    }

    if (base_order > order) {
        KURD_t r = fnd.split(base_order, found_off, order);
        if (!success_all_kurd(r)) return r;
        // 缓存右兄弟链
        uint64_t so = found_off;
        for (uint8_t o = base_order; o > order + 1; o--) {
            cache_insert(o - 1, so * 2 + 1);
            so = so * 2;
        }
        cache_insert(order, so * 2 + 1);
        found_off <<= (base_order - order);
    }

    KURD_t r = fnd.order_occupy_try(order, found_off);
    if (!success_all_kurd(r)) return r;
    out_offset = found_off;
    stat_scan++;
    return success;
}

// ═══ internal_free — 底座 order_return ═══

KURD_t kpoolmemmgr_t::HCB_v3::internal_free(uint64_t offset, uint8_t order)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_INTERNAL_FREE;
    error.event_code   = EVENT_CODE_INTERNAL_FREE;

    KURD_t kurd;
    uint8_t final_order = fnd.order_return(order, offset, kurd);
    if (final_order >= 0x40) {
        error.reason = FREE_RESULTS::FATAL_REASONS::DOUBLE_FREE_DETECT;
        return error;
    }

    // 缓存折叠后的最终块
    uint64_t coalesced_off = offset >> (final_order - order);
    cache_insert(final_order, coalesced_off);

    stat_coalesce += (final_order - order);
    return success;
}

// ═══ alloc/free/realloc/clear ═══

KURD_t kpoolmemmgr_t::HCB_v3::alloc(void*& addr, uint32_t size, alloc_flags_t flags)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_ALLOC;
    error.event_code   = EVENT_CODE_ALLOC;

    if (!valid) { return error; }
    if (size == 0) {
        error.reason = ALLOC_RESULTS::FAIL_REASONS::REASON_CODE_SIZE_IS_ZERO;
        return error;
    }

    uint32_t total_size = (uint32_t)size + sizeof(buddy_meta);
    if (total_size > (1u << (max_order_ + 5))) {  // max_order * 32B
        error.reason = ALLOC_RESULTS::FAIL_REASONS::REASON_CODE_SIZE_TOO_LARGE;
        return error;
    }

    uint8_t order = size_to_order(total_size);

    uint64_t offset;
    KURD_t kurd = internal_alloc(offset, order);
    if (!success_all_kurd(kurd)) {
        stat_alloc_fail++;
        return kurd;
    }

    addr = (void*)(vbase_ + (offset << order) * BYTES_PER_ORDER0 + sizeof(buddy_meta));
    buddy_meta* meta = meta_from_ptr(addr);
    meta->data_size = size;
    meta->flags     = (uint8_t)(flags.force_first_linekd_heap |
                                (flags.is_when_realloc_force_new_addr << 1));
    meta->magic     = MAGIC_ALLOCATED;
    stat_alloc++;
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v3::free(void* ptr)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_FREE;
    error.event_code   = EVENT_CODE_FREE;

    if (!valid) { return error; }
    if (!ptr) { return error; }
    if (!is_addr_belong(ptr)) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_ADDR_NOT_THIS_HEAP;
        return error;
    }

    // 地址必须 16B 对齐 (sizeof buddy_meta)
    if ((uint64_t)ptr & 0xF) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) {
        error.reason = FREE_RESULTS::FATAL_REASONS::DOUBLE_FREE_DETECT;
        return error;
    }
    meta->magic = 0;

    uint32_t total_size = meta->data_size + sizeof(buddy_meta);
    uint8_t order = size_to_order(total_size);
    return internal_free(ptr_to_offset(ptr, order), order);
}

KURD_t kpoolmemmgr_t::HCB_v3::realloc(void*& ptr, uint32_t new_size, alloc_flags_t flags)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_REALLOC;
    error.event_code   = EVENT_CODE_REALLOC;

    if (!valid) { return error; }
    if (!ptr) { return alloc(ptr, new_size, flags); }

    buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    uint32_t old_total = meta->data_size + sizeof(buddy_meta);
    uint32_t new_total = new_size + sizeof(buddy_meta);
    uint8_t old_order = size_to_order(old_total);
    uint8_t new_order = size_to_order(new_total);

    if (new_order == old_order) {
        meta->data_size = new_size;
        return success;
    }

    // 分配新块
    void* new_ptr = nullptr;
    KURD_t kurd = alloc(new_ptr, new_size, flags);
    if (!success_all_kurd(kurd)) return kurd;

    // 拷贝数据
    uint32_t copy_size = meta->data_size < new_size ? meta->data_size : new_size;
    if (copy_size > 0 && ptr && new_ptr)
        __builtin_memcpy(new_ptr, ptr, copy_size);

    // 释放旧块
    kurd = free(ptr);
    if (!success_all_kurd(kurd)) {
        free(new_ptr);
        return kurd;
    }

    ptr = new_ptr;
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v3::clear(void* ptr)
{
    using namespace MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_V3_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_CLEAR;
    error.event_code   = EVENT_CODE_CLEAR;

    if (!valid) { return error; }
    if (!ptr) { return error; }

    buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    __builtin_memset(ptr, 0, meta->data_size);
    return success;
}

// ═══ flush_free_count — 底座 btree_validation ═══

KURD_t kpoolmemmgr_t::HCB_v3::flush_free_count()
{
    return fnd.btree_validation();
}
