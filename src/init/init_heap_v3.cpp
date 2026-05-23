#include "init/init_heap_v3.h"
#include <cstring>

// ════════════════════════════════════════════════════════════════
// init_heap_v3 — 基于 HCB_v3 简化的单堆分配器
//
// layout:
//   [data_va, data_va + total_size)   — 用户 alloc 数据区
//   [bitmap_va ..)                     — BCB 位图 (24KB for 2MB heap)
//
// 最小分配单位: 32B (16B meta + 16B payload)
// 最大分配: ~2MB (单块上限)
// ════════════════════════════════════════════════════════════════

// ── 全局单例 ──
init_heap g_init_heap;

// ═══ 初始化 ═══

void init_heap::linktime_init(vaddr_t data_va, uint32_t size, vaddr_t bitmap_va)
{
    base_      = data_va;
    total_size_ = size;

    // 底座初始化（自动清位图 + 标记根节点为 NODE_FREE）
    fnd.init(bitmap_va, MAX_ORDER);

    // 清空缓存
    for (auto& row : cache_entries_)
        for (auto& e : row) e = ~0ULL;
    for (auto& c : cache_cursor_) c = 0;

    stat_alloc = stat_free = stat_alloc_fail = stat_coalesce = stat_split = stat_cache_hit = stat_scan = 0;
}

// ═══ 辅助函数 ═══

init_buddy_meta* init_heap::meta_from_ptr(void* ptr) const
{
    return reinterpret_cast<init_buddy_meta*>((uint8_t*)ptr - sizeof(init_buddy_meta));
}

uint8_t init_heap::size_to_order(uint32_t size_with_meta) const
{
    if (size_with_meta == 0) return 0;
    uint64_t units = ((uint64_t)size_with_meta + BYTES_PER_ORDER0 - 1) / BYTES_PER_ORDER0;
    if (units == 1) return 0;
    return (uint8_t)(64 - __builtin_clzll(units - 1));
}

uint64_t init_heap::ptr_to_offset(void* ptr, uint8_t order) const
{
    uint64_t block_va = (uint64_t)ptr - sizeof(init_buddy_meta);
    return (block_va - base_) >> (order + 5);  // ÷ (32 << order)
}

// ═══ 缓存 ═══

void init_heap::cache_insert(uint8_t order, uint64_t offset)
{
    if (order > MAX_ORDER) return;
    auto& entries = cache_entries_[order];
    entries[cache_cursor_[order]] = offset;
    cache_cursor_[order] = (cache_cursor_[order] + 1) % PER_ORDER_CACHE;
}

bool init_heap::cache_pick(uint8_t order, uint64_t& out_offset)
{
    if (order > MAX_ORDER) return false;
    auto& entries = cache_entries_[order];
    for (uint8_t i = 0; i < PER_ORDER_CACHE; ++i) {
        uint64_t entry = entries[i];
        if (entry == ~0ULL) continue;
        if (!fnd.is_free(order, entry)) {
            entries[i] = ~0ULL;
            continue;
        }
        entries[i] = ~0ULL;
        out_offset = entry;
        return true;
    }
    return false;
}

// ═══ internal_alloc — 缓存优先, fallback 底座 find_candidate ═══

bool init_heap::internal_alloc(uint64_t& out_offset, uint8_t order)
{
    // 先试缓存 (order 及以上)
    for (uint8_t i = order; i <= MAX_ORDER; i++) {
        uint64_t cached_off;
        if (!cache_pick(i, cached_off)) continue;

        stat_cache_hit++;
        if (i > order) {
            if (error_kurd(fnd.split(i, cached_off, order))) return false;
        }
        uint64_t final_off = (i > order)
            ? (cached_off << (i - order))
            : cached_off;
        if (error_kurd(fnd.order_occupy_try(order, final_off))) continue;
        out_offset = final_off;
        return true;
    }

    // 全 cache miss → 底座查找
    stat_scan++;
    KURD_t ignored;
    uint8_t base_order = order;
    uint64_t found_off = fnd.find_candidate(base_order, ignored);

    if (found_off == ~0ULL) {
        stat_alloc_fail++;
        return false;
    }

    if (base_order > order) {
        if (error_kurd(fnd.split(base_order, found_off, order))) return false;
        // 缓存右兄弟链
        uint64_t so = found_off;
        for (uint8_t o = base_order; o > order + 1; o--) {
            cache_insert(o - 1, so * 2 + 1);
            so = so * 2;
        }
        cache_insert(order, so * 2 + 1);
        found_off <<= (base_order - order);
    }

    if (error_kurd(fnd.order_occupy_try(order, found_off))) return false;
    out_offset = found_off;
    stat_scan++;
    return true;
}

// ═══ internal_free — 底座 order_return + 缓存折叠后的块 ═══

bool init_heap::internal_free(uint64_t offset, uint8_t order)
{
    KURD_t ignored;
    uint8_t final_order = fnd.order_return(order, offset, ignored);
    if (final_order >= 0x40) {
        return false;  // double-free
    }

    // 缓存折叠后的最终块
    uint64_t coalesced_off = offset >> (final_order - order);
    cache_insert(final_order, coalesced_off);

    stat_coalesce += (final_order - order);
    return true;
}

// ═══ alloc ═══

void* init_heap::alloc(uint32_t size, alloc_flags_t flags)
{
    if (size == 0) return nullptr;

    uint32_t total_size = size + sizeof(init_buddy_meta);
    if (total_size > (1u << (MAX_ORDER + 5)))  // max_order * 32B
        return nullptr;

    uint8_t order = size_to_order(total_size);
    uint64_t offset;

    if (!internal_alloc(offset, order))
        return nullptr;

    void* addr = (void*)(base_ + (offset << (order + 5)) + sizeof(init_buddy_meta));
    // 计算: order 0 = 32B blocks, offset 是 order 级索引
    // block_va = base_ + offset * BYTES_PER_ORDER0 * 2^order
    //         = base_ + offset * (32 << order)
    //         = base_ + (offset << (order + 5))
    // data_va  = block_va + sizeof(meta)

    init_buddy_meta* meta = meta_from_ptr(addr);
    meta->data_size = size;
    meta->flags     = (uint8_t)(flags.force_first_linekd_heap |
                                (flags.is_when_realloc_force_new_addr << 1));
    meta->magic     = MAGIC_ALLOCATED;
    stat_alloc++;
    return addr;
}

// ═══ free ═══

bool init_heap::free(void* ptr)
{
    if (!ptr) return false;

    // 地址必须在堆内
    if (!is_addr_belong(ptr)) return false;

    // 16B 对齐
    if ((uint64_t)ptr & 0xF) return false;

    init_buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) return false;
    meta->magic = 0;

    uint32_t total_size = meta->data_size + sizeof(init_buddy_meta);
    uint8_t order = size_to_order(total_size);
    stat_free++;
    return internal_free(ptr_to_offset(ptr, order), order);
}

// ═══ realloc ═══

void* init_heap::realloc(void* ptr, uint32_t new_size, alloc_flags_t flags)
{
    if (!ptr) return alloc(new_size, flags);

    init_buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) return nullptr;

    uint32_t old_total = meta->data_size + sizeof(init_buddy_meta);
    uint32_t new_total = new_size + sizeof(init_buddy_meta);
    uint8_t old_order = size_to_order(old_total);
    uint8_t new_order = size_to_order(new_total);

    if (new_order == old_order) {
        // 同一 order，原地调整
        meta->data_size = new_size;
        return ptr;
    }

    // 分配新块
    void* new_ptr = alloc(new_size, flags);
    if (!new_ptr) return nullptr;

    // 拷贝旧数据
    uint32_t copy_size = (meta->data_size < new_size) ? meta->data_size : new_size;
    if (copy_size > 0)
        __builtin_memcpy(new_ptr, ptr, copy_size);

    // 释放旧块
    if (!free(ptr)) {
        free(new_ptr);
        return nullptr;
    }

    return new_ptr;
}

// ═══ clear ═══

bool init_heap::clear(void* ptr)
{
    if (!ptr) return false;

    init_buddy_meta* meta = meta_from_ptr(ptr);
    if (meta->magic != MAGIC_ALLOCATED) return false;

    __builtin_memset(ptr, 0, meta->data_size);
    return true;
}

// ═══ is_addr_belong ═══

bool init_heap::is_addr_belong(void* addr) const
{
    uint64_t u = (uint64_t)addr;
    return u >= base_ && u < base_ + total_size_;
}

// ════════════════════════════════════════════════════════════════
// new/delete — 委托给 g_init_heap
// ════════════════════════════════════════════════════════════════

void* operator new(size_t size)
{
    void* p = g_init_heap.alloc((uint32_t)size);
    if (!p) __builtin_trap();
    return p;
}

void* operator new(size_t size, alloc_flags_t flags)
{
    void* p = g_init_heap.alloc((uint32_t)size, flags);
    if (!p) __builtin_trap();
    return p;
}

void* operator new[](size_t size)
{
    return ::operator new(size);
}

void* operator new[](size_t size, alloc_flags_t flags)
{
    return ::operator new(size, flags);
}

void operator delete(void* ptr) noexcept
{
    g_init_heap.free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
    g_init_heap.free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    g_init_heap.free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    g_init_heap.free(ptr);
}

// 放置 new
void* operator new(size_t, void* ptr) noexcept
{
    return ptr;
}

void* operator new[](size_t, void* ptr) noexcept
{
    return ptr;
}

// ════════════════════════════════════════════════════════════════
// C++ ABI 桩 — freestanding 环境
// ════════════════════════════════════════════════════════════════
extern "C" {
    void* __dso_handle = 0;
    int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
}
