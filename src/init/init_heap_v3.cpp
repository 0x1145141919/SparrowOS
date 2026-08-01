#include "init/init_heap_v3.h"
#include <cstddef>

// ════════════════════════════════════════════════════════════════
// init_heap_v3 — 复刻 kpoolmemmgr HCB_v2.1 扁平位图分配器（摘去 KURD）
//
// layout:
//   [data_va, data_va + total_size)   — 用户 alloc 数据区 (8B/bit)
//   [bitmap_va ..)                     — flat bitmap (1 bit = 8 bytes)
//
// 最小分配单位: 8B + 8B meta
// 分配算法: next-fit scan + scan_cache（无锁，单线程 boot）
// ════════════════════════════════════════════════════════════════

// ── 全局单例 ──
init_heap g_init_heap;

// ═══ 辅助函数 ═══

init_heap::data_meta* init_heap::meta_from_ptr(void* ptr) const
{
    return reinterpret_cast<data_meta*>((uint8_t*)ptr - sizeof(data_meta));
}

uint32_t init_heap::bits_needed(uint32_t user_size) const
{
    uint64_t total = (uint64_t)user_size + sizeof(data_meta);
    return (uint32_t)((total + BYTES_PER_BIT - 1) / BYTES_PER_BIT);
}

uint64_t init_heap::bit_idx_from_ptr(void* ptr) const
{
    uint64_t meta_addr = (uint64_t)ptr - sizeof(data_meta);
    return (meta_addr - base_) / BYTES_PER_BIT;
}

void init_heap::fill_canary(uint8_t* user_ptr, uint32_t user_size,
                            uint64_t total_bits_alloced)
{
    uint32_t total_bytes = (uint32_t)(total_bits_alloced * BYTES_PER_BIT);
    uint32_t canary_start = sizeof(data_meta) + user_size;
    if (total_bytes > canary_start) {
        uint32_t pad = total_bytes - canary_start;
        __builtin_memset(user_ptr + user_size, 0xFF, pad);
    }
}

void init_heap::check_canary(uint8_t* user_ptr, uint32_t user_size,
                             uint64_t total_bits_alloced)
{
    uint32_t total_bytes = (uint32_t)(total_bits_alloced * BYTES_PER_BIT);
    uint32_t canary_start = sizeof(data_meta) + user_size;
    if (total_bytes > canary_start) {
        uint32_t pad = total_bytes - canary_start;
        for (uint32_t i = 0; i < pad; i++) {
            if (user_ptr[user_size + i] != 0xFF) {
                break;
            }
        }
    }
}

// ═══ Bitmap 操作 ═══

bool init_heap::bitmap_test(uint64_t bit_idx) const
{
    uint64_t* word = (uint64_t*)bitmap_va_ + (bit_idx / 64);
    return (*word >> (bit_idx % 64)) & 1ULL;
}

void init_heap::bitmap_set(uint64_t bit_idx, uint64_t count)
{
    uint64_t* words = (uint64_t*)bitmap_va_;
    uint64_t end = bit_idx + count;
    for (uint64_t i = bit_idx; i < end; ) {
        uint64_t wi = i / 64;
        uint64_t bi = i % 64;
        uint64_t batch = end - i;
        if (batch > 64 - bi) batch = 64 - bi;
        uint64_t mask = ((1ULL << batch) - 1) << bi;
        words[wi] |= mask;
        i += batch;
    }
}

void init_heap::bitmap_clear(uint64_t bit_idx, uint64_t count)
{
    uint64_t* words = (uint64_t*)bitmap_va_;
    uint64_t end = bit_idx + count;
    for (uint64_t i = bit_idx; i < end; ) {
        uint64_t wi = i / 64;
        uint64_t bi = i % 64;
        uint64_t batch = end - i;
        if (batch > 64 - bi) batch = 64 - bi;
        uint64_t mask = ((1ULL << batch) - 1) << bi;
        words[wi] &= ~mask;
        i += batch;
    }
}

// ═══ Bitmap 扫描 — next-fit, 返回连续空闲 bits ═══

bool init_heap::bitmap_scan_from(uint64_t start, uint64_t needed,
                                 uint64_t& out_idx, uint64_t& out_runlen)
{
    if (needed == 0 || needed > total_bits_) return false;
    if (cache_.free_bit_count < needed) return false;

    uint64_t* words = (uint64_t*)bitmap_va_;
    uint64_t best_run = 0;
    uint64_t run = 0;
    uint64_t run_start = 0;

    // Two passes: from start → end, then wrap 0 → start
    for (int pass = 0; pass < 2; pass++) {
        uint64_t begin = (pass == 0) ? start : 0;
        uint64_t end   = (pass == 0) ? total_bits_ : start;
        uint64_t i = begin;

        while (i < end) {
            uint64_t word_idx = i / 64;
            uint64_t bit_off  = i % 64;
            uint64_t bits_left = 64 - bit_off;
            uint64_t examine = end - i;
            if (examine > bits_left) examine = bits_left;

            uint64_t word = words[word_idx];
            uint64_t free_bits = ~word;          // 1 = free

            uint64_t chunk = (free_bits >> bit_off);
            if (examine < 64)
                chunk &= ((1ULL << examine) - 1);

            if (chunk == 0) {
                if (run > best_run) best_run = run;
                run = 0;
                i += examine;
                continue;
            }

            while (chunk != 0 && i < end) {
                uint64_t skip = __builtin_ctzll(chunk);
                if (skip > 0) {
                    if (run > best_run) best_run = run;
                    run = 0;
                    i += skip;
                    if (i >= end) break;
                    chunk >>= skip;
                }

                if (chunk == 0) break;

                uint64_t inv = ~chunk;
                uint64_t free_run;
                if (inv == 0) {
                    free_run = 64;
                } else {
                    free_run = __builtin_ctzll(inv);
                }
                uint64_t remain_in_pass = end - i;
                if (free_run > remain_in_pass) free_run = remain_in_pass;

                if (run == 0) run_start = i;
                run += free_run;

                if (run >= needed) {
                    out_idx = run_start;
                    out_runlen = run;
                    uint64_t after = i + free_run;
                    cache_.hint_u64_idx = after / 64;
                    return true;
                }

                i += free_run;
                if (free_run < 64)
                    chunk >>= free_run;
                else
                    chunk = 0;
            }
        }

        if (run > best_run) best_run = run;
        run = 0;

        if (cache_.free_bit_count < needed) break;
    }

    if (best_run > cache_.largest_free_hint)
        cache_.largest_free_hint = best_run;
    return false;
}

// ═══ 初始化 ═══

void init_heap::linktime_init(vaddr_t data_va, uint32_t size, vaddr_t bitmap_va)
{
    base_         = data_va;
    total_size_   = size;
    total_bits_   = size / BYTES_PER_BIT;

    bitmap_va_    = bitmap_va;
    bitmap_bytes_ = (total_bits_ + 7) / 8;

    __builtin_memset((void*)bitmap_va_, 0, bitmap_bytes_);

    cache_ = {};
    cache_.free_bit_count = total_bits_;
    cache_.largest_free_hint = total_bits_;

    stat_alloc = stat_free = stat_alloc_fail = stat_oom = 0;
    stat_realloc_expand = stat_scan_fail = 0;
}

// ═══ alloc ═══

void* init_heap::alloc(uint32_t size, alloc_flags_t flags)
{
    if (size == 0) return nullptr;

    uint32_t need_bits = bits_needed(size);

    uint8_t align_log2 = flags.align_require_log2;
    if (align_log2 < 3) align_log2 = 3;
    if (align_log2 > 15) align_log2 = 15;

    uint32_t search_bits = need_bits;
    uint32_t max_align_pad = 0;
    if (align_log2 > 3) {
        uint8_t align_shift = align_log2 - 3;
        max_align_pad = (1u << align_shift) - 1;
        search_bits = need_bits + max_align_pad;
    }

    uint64_t search_bytes = (uint64_t)search_bits * BYTES_PER_BIT;
    if (search_bytes > total_size_) {
        stat_alloc_fail++;
        return nullptr;
    }

    if (cache_.free_bit_count < search_bits) {
        stat_alloc_fail++;
        stat_oom++;
        return nullptr;
    }

    uint64_t out_idx, out_runlen;
    if (!bitmap_scan_from(cache_.last_success_idx, search_bits, out_idx, out_runlen)) {
        stat_alloc_fail++;
        cache_.largest_free_hint = 0;
        stat_scan_fail++;
        return nullptr;
    }

    uint64_t bit_idx = out_idx;
    uint32_t align_pad = 0;
    if (align_log2 > 3) {
        uint8_t align_shift = align_log2 - 3;
        uint64_t align_mask = (1ULL << align_shift) - 1;
        uint64_t aligned = ((bit_idx + 1 + align_mask) & ~align_mask) - 1;
        align_pad = (uint32_t)(aligned - bit_idx);
        bit_idx = aligned;
    }

    uint32_t total_use = align_pad + need_bits;
    bitmap_set(out_idx, total_use);

    cache_.last_success_idx = bit_idx + need_bits;
    if (cache_.last_success_idx >= total_bits_)
        cache_.last_success_idx = 0;
    cache_.free_bit_count -= total_use;
    cache_.entry_count++;
    cache_.used_bytes_acc += size;

    void* addr = (void*)(base_ + bit_idx * BYTES_PER_BIT + sizeof(data_meta));

    data_meta* meta = meta_from_ptr(addr);
    meta->data_size      = size;
    meta->alloc_flags    = (uint8_t)(flags.force_first_linekd_heap |
                                     (flags.is_when_realloc_force_new_addr << 1) |
                                     (flags.align_require_log2 << 2));
    meta->magic          = 0;
    meta->align_pad_bits = align_pad;

    fill_canary((uint8_t*)addr, size, need_bits);

    stat_alloc++;
    return addr;
}

// ═══ free ═══

bool init_heap::free(void* ptr)
{
    if (!ptr) return false;
    if (!is_addr_belong(ptr)) return false;

    if ((uint64_t)ptr & 0x7) return false;

    data_meta* meta = meta_from_ptr(ptr);

    if (meta->data_size == 0) {
        return false;  // double-free
    }

    uint32_t user_size  = meta->data_size;
    uint32_t need_bits  = bits_needed(user_size);
    uint32_t align_pad  = meta->align_pad_bits;
    uint64_t bit_idx    = bit_idx_from_ptr(ptr);
    uint64_t run_start  = bit_idx - align_pad;
    uint32_t total_bits = align_pad + need_bits;

    check_canary((uint8_t*)ptr, user_size, need_bits);

    __builtin_memset(meta, 0, sizeof(data_meta));

    bitmap_clear(run_start, total_bits);

    cache_.free_bit_count += total_bits;
    cache_.entry_count--;
    cache_.used_bytes_acc -= user_size;

    if (total_bits > cache_.largest_free_hint)
        cache_.largest_free_hint = total_bits;

    stat_free++;
    return true;
}

// ═══ realloc — 含原地扩缩 ═══

void* init_heap::realloc(void* ptr, uint32_t new_size, alloc_flags_t flags)
{
    if (!ptr) return alloc(new_size, flags);

    data_meta* meta = meta_from_ptr(ptr);
    if (meta->data_size == 0) return nullptr;

    uint32_t old_size = meta->data_size;
    uint32_t old_bits = bits_needed(old_size);
    uint32_t new_bits = bits_needed(new_size);

    if (new_bits == old_bits) {
        meta->data_size = new_size;
        fill_canary((uint8_t*)ptr, new_size, old_bits);
        return ptr;
    }

    if (new_bits < old_bits) {
        bitmap_clear(bit_idx_from_ptr(ptr) + new_bits, old_bits - new_bits);
        cache_.free_bit_count += (old_bits - new_bits);
        cache_.used_bytes_acc -= (old_size - new_size);
        meta->data_size = new_size;
        fill_canary((uint8_t*)ptr, new_size, new_bits);
        return ptr;
    }

    uint64_t bit_idx = bit_idx_from_ptr(ptr);
    uint64_t extra   = new_bits - old_bits;

    bool can_expand = true;
    for (uint64_t i = 0; i < extra; i++) {
        if (bitmap_test(bit_idx + old_bits + i)) {
            can_expand = false;
            break;
        }
    }

    if (can_expand) {
        bitmap_set(bit_idx + old_bits, extra);
        cache_.free_bit_count -= extra;
        cache_.used_bytes_acc += (new_size - old_size);
        meta->data_size = new_size;
        fill_canary((uint8_t*)ptr, new_size, new_bits);
        stat_realloc_expand++;
        return ptr;
    }

    void* new_ptr = alloc(new_size, flags);
    if (!new_ptr) return nullptr;

    uint32_t copy_size = old_size < new_size ? old_size : new_size;
    if (copy_size > 0 && ptr && new_ptr)
        __builtin_memcpy(new_ptr, ptr, copy_size);

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

    data_meta* meta = meta_from_ptr(ptr);
    if (meta->data_size == 0) return false;

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
