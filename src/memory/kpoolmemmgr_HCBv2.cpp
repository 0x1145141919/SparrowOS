#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/AddresSpace.h"
#include "util/OS_utils.h"
#include <cstring>

// ════════════════════════════════════════════════════════════════
// HCB_v2.1 — Flat bitmap + next-fit scan
//
// 替换 HCB_v3 (buddy tree):
//   - 8B/bit flat bitmap  vs  3×2^N buddy tree
//   - 8B data_meta        vs  16B buddy_meta
//   - next-fit scan+cache vs  per-order buddy cache + split/coalesce
// ════════════════════════════════════════════════════════════════

// first_linekd_heap 的静态 BSS 数据
constexpr uint32_t DEFAULT_FIRST_HEAP_SIZE  = 1 << 22;  // 4MB
constexpr uint32_t DEFAULT_FIRST_BITMAP_SIZE = DEFAULT_FIRST_HEAP_SIZE / 8 / 8;  // 4MB/64 = 64KB
uint8_t first_heap[DEFAULT_FIRST_HEAP_SIZE];
uint8_t first_heap_bitmap[DEFAULT_FIRST_BITMAP_SIZE];

// ═══ KURD 位置级模板 — <LOCATION_CODE_KPOOLMEMMGR_HCB> ═══

KURD_t kpoolmemmgr_t::HCB_v2::kurd_default_success()
{
    KURD_t k;
    k.domain            = err_domain::CORE_MODULE;
    k.module_code       = module_code::MEMORY;
    k.in_module_location = MEMMODULE_LOCATIONS::LOCATION_CODE_KPOOLMEMMGR_HCB;
    k.result            = result_code::SUCCESS;
    k.level             = level_code::INFO;
    return k;
}

KURD_t kpoolmemmgr_t::HCB_v2::kurd_default_error()
{
    KURD_t k;
    k.domain            = err_domain::CORE_MODULE;
    k.module_code       = module_code::MEMORY;
    k.in_module_location = MEMMODULE_LOCATIONS::LOCATION_CODE_KPOOLMEMMGR_HCB;
    k.result            = result_code::FAIL;
    k.level             = level_code::ERROR;
    return k;
}

KURD_t kpoolmemmgr_t::HCB_v2::kurd_default_fatal()
{
    KURD_t k;
    k.domain            = err_domain::CORE_MODULE;
    k.module_code       = module_code::MEMORY;
    k.in_module_location = MEMMODULE_LOCATIONS::LOCATION_CODE_KPOOLMEMMGR_HCB;
    k.result            = result_code::FATAL;
    k.level             = level_code::FATAL;
    return k;
}

// ═══ 辅助函数 ═══

kpoolmemmgr_t::data_meta* kpoolmemmgr_t::HCB_v2::meta_from_ptr(void* ptr) const
{
    return reinterpret_cast<data_meta*>((uint8_t*)ptr - sizeof(data_meta));
}

uint32_t kpoolmemmgr_t::HCB_v2::bits_needed(uint32_t user_size) const
{
    uint64_t total = (uint64_t)user_size + sizeof(data_meta);
    return (uint32_t)((total + BYTES_PER_BIT - 1) / BYTES_PER_BIT);
}

uint64_t kpoolmemmgr_t::HCB_v2::bit_idx_from_ptr(void* ptr) const
{
    uint64_t meta_addr = (uint64_t)ptr - sizeof(data_meta);
    return (meta_addr - vbase_) / BYTES_PER_BIT;
}

void kpoolmemmgr_t::HCB_v2::fill_canary(uint8_t* user_ptr, uint32_t user_size,
                                          uint64_t total_bits_alloced)
{
    uint32_t total_bytes = (uint32_t)(total_bits_alloced * BYTES_PER_BIT);
    uint32_t canary_start = sizeof(data_meta) + user_size;
    if (total_bytes > canary_start) {
        uint32_t pad = total_bytes - canary_start;
        __builtin_memset(user_ptr + user_size, 0xFF, pad);
    }
}

void kpoolmemmgr_t::HCB_v2::check_canary(uint8_t* user_ptr, uint32_t user_size,
                                           uint64_t total_bits_alloced)
{
    uint32_t total_bytes = (uint32_t)(total_bits_alloced * BYTES_PER_BIT);
    uint32_t canary_start = sizeof(data_meta) + user_size;
    if (total_bytes > canary_start) {
        uint32_t pad = total_bytes - canary_start;
        for (uint32_t i = 0; i < pad; i++) {
            if (user_ptr[user_size + i] != 0xFF) {
                // 0xFF canary 被破坏 → 越界写
                // 返回后由调用方处理
                break;
            }
        }
    }
}

// ═══ Bitmap 操作 ═══

bool kpoolmemmgr_t::HCB_v2::bitmap_test(uint64_t bit_idx) const
{
    uint64_t* word = (uint64_t*)bitmap_va_ + (bit_idx / 64);
    return (*word >> (bit_idx % 64)) & 1ULL;
}

void kpoolmemmgr_t::HCB_v2::bitmap_set(uint64_t bit_idx, uint64_t count)
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

void kpoolmemmgr_t::HCB_v2::bitmap_clear(uint64_t bit_idx, uint64_t count)
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

bool kpoolmemmgr_t::HCB_v2::bitmap_scan_from(uint64_t start, uint64_t needed,
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

            // Extract relevant bits from bit_off
            uint64_t chunk = (free_bits >> bit_off);
            if (examine < 64)
                chunk &= ((1ULL << examine) - 1);

            if (chunk == 0) {
                // No free bits here
                if (run > best_run) best_run = run;
                run = 0;
                i += examine;
                continue;
            }

            // Process free/allocated runs in this chunk
            while (chunk != 0 && i < end) {
                // Skip allocated bits (trailing zeros in chunk)
                uint64_t skip = __builtin_ctzll(chunk);
                if (skip > 0) {
                    if (run > best_run) best_run = run;
                    run = 0;
                    i += skip;
                    if (i >= end) break;
                    chunk >>= skip;
                }

                if (chunk == 0) break;

                // Count free bits (trailing ones in chunk)
                uint64_t inv = ~chunk;
                uint64_t free_run;
                if (inv == 0) {
                    free_run = 64;  // all remaining bits in chunk are free
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
                    // Update scan hint: next word boundary after this alloc
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

        // Fast exit: not enough total free bits
        if (cache_.free_bit_count < needed) break;
    }

    // Failed — cache the largest run seen for next time
    if (best_run > cache_.largest_free_hint)
        cache_.largest_free_hint = best_run;
    return false;
}

// ═══ first_linekd_heap: BSS static ═══

void kpoolmemmgr_t::HCB_v2::linktime_init()
{
    vbase_       = (uint64_t)first_heap;
    total_size_  = HCB_DEFAULT_SIZE;
    total_bits_  = total_size_ / BYTES_PER_BIT;
    data_pbase   = 0;

    bitmap_va_   = (uint64_t)first_heap_bitmap;
    bitmap_pbase = 0;
    bitmap_bytes_ = (total_bits_ + 7) / 8;  // round up to byte

    // Clear bitmap
    __builtin_memset((void*)bitmap_va_, 0, bitmap_bytes_);

    // Init scan cache: entire heap is free
    cache_ = {};
    cache_.free_bit_count = total_bits_;
    cache_.largest_free_hint = total_bits_;
    valid = true;

    stat_alloc = stat_free = stat_alloc_fail = stat_oom = 0;
    stat_realloc_expand = stat_scan_fail = 0;
}

#ifdef TEST_MODE
void kpoolmemmgr_t::HCB_v2::test_init(vaddr_t data_va, vaddr_t bitmap_va, uint32_t size)
{
    vbase_       = data_va;
    total_size_  = size;
    total_bits_  = total_size_ / BYTES_PER_BIT;
    data_pbase   = 0;

    bitmap_va_   = bitmap_va;
    bitmap_pbase = 0;
    bitmap_bytes_ = (total_bits_ + 7) / 8;

    __builtin_memset((void*)bitmap_va_, 0, bitmap_bytes_);

    cache_ = {};
    cache_.free_bit_count = total_bits_;
    cache_.largest_free_hint = total_bits_;
    valid = true;

    stat_alloc = stat_free = stat_alloc_fail = stat_oom = 0;
    stat_realloc_expand = stat_scan_fail = 0;
}
#endif

// ═══ online — 动态 HCB 初始化（物理页分配 + 映射） ═══

KURD_t kpoolmemmgr_t::HCB_v2::online(uint32_t size, vaddr_t data_va, vaddr_t bitmap_va)
{
    using namespace MEMMODULE_LOCATIONS::KPOOLMEMMGR_HCB_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_ONLINE;
    error.event_code   = EVENT_CODE_ONLINE;

    if (valid) {
        error.reason = online_results::FAIL_REASONS::REASON_CODE_ALREADY_ONLINE;
        return error;
    }

    // data_va 必须 2MB 对齐
    if (data_va & ((1ULL << 21) - 1)) {
        return error;
    }

    vbase_      = data_va;
    total_size_ = size;
    total_bits_ = total_size_ / BYTES_PER_BIT;

    // 分配 data 物理页 (2MB aligned)
    {
        buddy_alloc_params params = BUDDY_ALLOC_ALWAYS_TRY;
        params.align_log2 = 21;
        KURD_t kurd;
        data_pbase = FreePagesAllocator::alloc(size, params, page_state_t::kernel_pinned, kurd);
        if (!success_all_kurd(kurd)) return kurd;
    }

    // 映射 data 区域
    vm_interval interval = {
        .vpn = data_va >> 12,
        .ppn = data_pbase >> 12,
        .npages = size >> 12,
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

    // 分配 bitmap 物理页
    bitmap_bytes_ = (total_bits_ + 7) / 8;
    uint64_t bm_alloc = (bitmap_bytes_ + 0xFFF) & ~0xFFFULL;
    {
        KURD_t kurd;
        bitmap_pbase = FreePagesAllocator::alloc(bm_alloc, BUDDY_ALLOC_ALWAYS_TRY,
                                                  page_state_t::kernel_pinned, kurd);
        if (!success_all_kurd(kurd)) {
            KspacePageTable::disable_VMentry(interval, kurd);
            FreePagesAllocator::free(data_pbase, size);
            data_pbase = 0;
            return kurd;
        }
    }

    // 映射 bitmap
    vm_interval bm_interval = {
        .vpn = bitmap_va >> 12,
        .ppn = bitmap_pbase >> 12,
        .npages = bm_alloc >> 12,
        .access = KspacePageTable::PG_RW,
    };
    {
        KURD_t kurd = KspacePageTable::enable_VMentry(bm_interval);
        if (!success_all_kurd(kurd)) {
            FreePagesAllocator::free(bitmap_pbase, bm_alloc);
            bitmap_pbase = 0;
            KspacePageTable::disable_VMentry(interval, kurd);
            FreePagesAllocator::free(data_pbase, size);
            data_pbase = 0;
            return kurd;
        }
    }
    bitmap_va_=bm_interval.vbase();
    // 清 bitmap, 初始化为全空闲
    __builtin_memset((void*)bitmap_va_, 0, bitmap_bytes_);
    cache_ = {};
    cache_.free_bit_count = total_bits_;
    cache_.largest_free_hint = total_bits_;
    valid = true;

    stat_alloc = stat_free = stat_alloc_fail = stat_oom = 0;
    stat_realloc_expand = stat_scan_fail = 0;
    return success;
}

// ═══ offline — 归还物理页 + 解映射 ═══

KURD_t kpoolmemmgr_t::HCB_v2::offline()
{
    using namespace MEMMODULE_LOCATIONS::KPOOLMEMMGR_HCB_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_OFFLINE;
    error.event_code   = EVENT_CODE_OFFLINE;

    if (!valid) return error;

    // 归还 bitmap 物理页
    if (bitmap_pbase != 0) {
        uint64_t bm_size = (bitmap_bytes_ + 0xFFF) & ~0xFFFULL;
        KURD_t kurd;
        vm_interval bm_int = {
            .vpn = bitmap_va_ >> 12,
            .ppn = bitmap_pbase >> 12,
            .npages = bm_size >> 12,
            .access = KspacePageTable::PG_RW,
        };
        seg_to_pages_info_pakage_t pak = KspacePageTable::disable_VMentry(bm_int, kurd);
        if (error_kurd(kurd)) return kurd;
        kurd = FreePagesAllocator::free(bitmap_pbase, bm_size);
        if (error_kurd(kurd)) return kurd;
        broadcast_invalidate_tlb(&pak);
        bitmap_pbase = 0;
    }

    // 归还 data 物理页
    if (data_pbase != 0) {
        KURD_t kurd;
        vm_interval intv = {
            .vpn = vbase_ >> 12,
            .ppn = data_pbase >> 12,
            .npages = total_size_ >> 12,
            .access = KspacePageTable::PG_RW,
        };
        seg_to_pages_info_pakage_t pak = KspacePageTable::disable_VMentry(intv, kurd);
        if (error_kurd(kurd)) return kurd;
        kurd = FreePagesAllocator::free(data_pbase, total_size_);
        if (error_kurd(kurd)) return kurd;
        broadcast_invalidate_tlb(&pak);
        data_pbase = 0;
    }

    valid = false;
    vbase_ = 0;
    total_size_ = 0;
    return success;
}

// ═══ 查询 ═══

bool kpoolmemmgr_t::HCB_v2::is_addr_belong(void* addr) const
{
    uint64_t u = (uint64_t)addr;
    return u >= vbase_ && u < vbase_ + total_size_;
}

uint64_t kpoolmemmgr_t::HCB_v2::used_bytes() const
{
    return cache_.used_bytes_acc;
}

bool kpoolmemmgr_t::HCB_v2::is_full() const
{
    return cache_.free_bit_count == 0;
}

KURD_t kpoolmemmgr_t::HCB_v2::flush_free_count()
{
    // Verify bitmap consistency: count free bits, compare to cache
    KURD_t success = kurd_default_success();
    uint64_t* words = (uint64_t*)bitmap_va_;
    uint64_t total_words = (total_bits_ + 63) / 64;
    uint64_t counted_free = 0;
    uint64_t counted_best_run = 0;
    uint64_t run = 0;

    for (uint64_t wi = 0; wi < total_words; wi++) {
        uint64_t free_bits = ~words[wi];
        // Mask out trailing bits beyond total_bits_ in last word
        if (wi == total_words - 1) {
            uint64_t remain = total_bits_ - wi * 64;
            if (remain < 64)
                free_bits &= (1ULL << remain) - 1;
        }
        counted_free += __builtin_popcountll(free_bits);
    }

    if (counted_free != cache_.free_bit_count) {
        // Desync — recover
        cache_.free_bit_count = counted_free;
    }
    return success;
}

// ═══ alloc ═══

KURD_t kpoolmemmgr_t::HCB_v2::alloc(void*& addr, uint32_t size, alloc_flags_t flags)
{
    using namespace MEMMODULE_LOCATIONS::KPOOLMEMMGR_HCB_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_ALLOC;
    error.event_code   = EVENT_CODE_ALLOC;

    if (!valid) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_HEAP_NOT_ONLINE;
        return error;
    }
    if (size == 0) {
        error.reason = alloc_results::FAIL_REASONS::REASON_CODE_SIZE_IS_ZERO;
        return error;
    }

    uint32_t need_bits = bits_needed(size);
    uint64_t need_bytes = (uint64_t)need_bits * BYTES_PER_BIT;

    uint8_t align_log2 = flags.align_require_log2;
    if (align_log2 < 3) align_log2 = 3;  // minimum 8B
    if (align_log2 > 15) align_log2 = 15; // cap at 32KB alignment

    // Compute search bits including alignment slack
    uint32_t search_bits = need_bits;
    uint32_t max_align_pad = 0;
    if (align_log2 > 3) {
        uint8_t align_shift = align_log2 - 3;
        max_align_pad = (1u << align_shift) - 1;
        search_bits = need_bits + max_align_pad;
    }

    // Check size with search_bits
    uint64_t search_bytes = (uint64_t)search_bits * BYTES_PER_BIT;
    if (search_bytes > total_size_) {
        error.reason = alloc_results::FAIL_REASONS::REASON_CODE_SIZE_TOO_LARGE;
        return error;
    }

    // Fast filter
    if (cache_.free_bit_count < search_bits) {
        stat_alloc_fail++;
        error.reason = alloc_results::FAIL_REASONS::REASON_CODE_NO_AVAILABLE_BLOCK;
        return error;
    }

    uint64_t out_idx, out_runlen;
    bool found = bitmap_scan_from(cache_.last_success_idx, search_bits, out_idx, out_runlen);
    if (!found) {
        stat_alloc_fail++;
        cache_.largest_free_hint = 0;
        error.reason = alloc_results::FAIL_REASONS::REASON_CODE_NO_AVAILABLE_BLOCK;
        return error;
    }

    // Apply alignment: round up bit_idx so (bit_idx + 1) * 8 % align == 0
    uint64_t bit_idx = out_idx;
    uint32_t align_pad = 0;
    if (align_log2 > 3) {
        uint8_t align_shift = align_log2 - 3;
        uint64_t align_mask = (1ULL << align_shift) - 1;
        // (bit_idx + 1) must be multiple of (1 << align_shift)
        uint64_t aligned = ((bit_idx + 1 + align_mask) & ~align_mask) - 1;
        align_pad = (uint32_t)(aligned - bit_idx);
        bit_idx = aligned;
    }

    // Mark full run (align_pad + data) in bitmap
    uint32_t total_use = align_pad + need_bits;
    bitmap_set(out_idx, total_use);

    // Update scan cache
    cache_.last_success_idx = bit_idx + need_bits;
    if (cache_.last_success_idx >= total_bits_)
        cache_.last_success_idx = 0;
    cache_.free_bit_count -= total_use;
    cache_.entry_count++;
    cache_.used_bytes_acc += size;

    // Compute user pointer
    addr = (void*)(vbase_ + bit_idx * BYTES_PER_BIT + sizeof(data_meta));

    // Write metadata
    data_meta* meta = meta_from_ptr(addr);
    meta->data_size      = size;
    meta->alloc_flags    = (uint8_t)(flags.force_first_linekd_heap |
                                     (flags.is_when_realloc_force_new_addr << 1) |
                                     (flags.align_require_log2 << 2));
    meta->magic          = 0;
    meta->align_pad_bits = align_pad;

    // Fill 0xFF canary in padding
    fill_canary((uint8_t*)addr, size, need_bits);

    stat_alloc++;
    return success;
}

// ═══ free ═══

KURD_t kpoolmemmgr_t::HCB_v2::free(void* ptr)
{
    using namespace MEMMODULE_LOCATIONS::KPOOLMEMMGR_HCB_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    KURD_t fatal   = kurd_default_fatal();
    success.event_code = EVENT_CODE_FREE;
    error.event_code   = EVENT_CODE_FREE;
    fatal.event_code   = EVENT_CODE_FREE;

    if (!valid) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_HEAP_NOT_ONLINE;
        return error;
    }
    if (!ptr) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }
    if (!is_addr_belong(ptr)) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_ADDR_NOT_THIS_HEAP;
        return error;
    }

    // 8B 对齐
    if ((uint64_t)ptr & 0x7) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    data_meta* meta = meta_from_ptr(ptr);

    // Double-free detection: data_size == 0 means already freed
    if (meta->data_size == 0) {
        error.reason = free_results::FAIL_REASONS::REASON_CODE_DOUBLE_FREE_DETECT;
        return error;
    }

    uint32_t user_size    = meta->data_size;
    uint32_t need_bits    = bits_needed(user_size);
    uint32_t align_pad    = meta->align_pad_bits;
    uint64_t bit_idx      = bit_idx_from_ptr(ptr);
    uint64_t run_start    = bit_idx - align_pad;
    uint32_t total_bits   = align_pad + need_bits;

    // Check 0xFF canary (padding bytes)
    check_canary((uint8_t*)ptr, user_size, need_bits);

    // Nuke metadata: ksetmem_8(meta, 0, 8)
    __builtin_memset(meta, 0, sizeof(data_meta));

    // Clear full run (including alignment padding)
    bitmap_clear(run_start, total_bits);

    // Update cache
    cache_.free_bit_count += total_bits;
    cache_.entry_count--;
    cache_.used_bytes_acc -= user_size;

    // Try to update largest_free_hint
    if (total_bits > cache_.largest_free_hint)
        cache_.largest_free_hint = total_bits;

    stat_free++;
    return success;
}

// ═══ realloc — 含原地扩缩 ═══

KURD_t kpoolmemmgr_t::HCB_v2::realloc(void*& ptr, uint32_t new_size, alloc_flags_t flags)
{
    using namespace MEMMODULE_LOCATIONS::KPOOLMEMMGR_HCB_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_REALLOC;
    error.event_code   = EVENT_CODE_REALLOC;

    if (!ptr) return alloc(ptr, new_size, flags);
    if (!valid) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_HEAP_NOT_ONLINE;
        return error;
    }

    data_meta* meta = meta_from_ptr(ptr);
    if (meta->data_size == 0) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    uint32_t old_size   = meta->data_size;
    uint32_t old_bits   = bits_needed(old_size);
    uint32_t new_bits   = bits_needed(new_size);

    if (new_bits == old_bits) {
        // Same block size — just update data_size and canary
        meta->data_size = new_size;
        fill_canary((uint8_t*)ptr, new_size, old_bits);
        return success;
    }

    if (new_bits < old_bits) {
        // Shrink: clear extra bits, update metadata
        bitmap_clear(bit_idx_from_ptr(ptr) + new_bits, old_bits - new_bits);
        cache_.free_bit_count += (old_bits - new_bits);
        cache_.used_bytes_acc -= (old_size - new_size);  // correct: old_size - new_size
        meta->data_size = new_size;
        fill_canary((uint8_t*)ptr, new_size, new_bits);
        return success;
    }

    // new_bits > old_bits: try in-place expansion
    uint64_t bit_idx  = bit_idx_from_ptr(ptr);
    uint64_t extra    = new_bits - old_bits;

    // Check if adjacent bits after current block are free
    bool can_expand = true;
    for (uint64_t i = 0; i < extra; i++) {
        if (bitmap_test(bit_idx + old_bits + i)) {
            can_expand = false;
            break;
        }
    }

    if (can_expand) {
        // Expand in place
        bitmap_set(bit_idx + old_bits, extra);
        cache_.free_bit_count -= extra;
        cache_.used_bytes_acc += (new_size - old_size);
        meta->data_size = new_size;
        fill_canary((uint8_t*)ptr, new_size, new_bits);
        stat_realloc_expand++;
        return success;
    }

    // Fallback: alloc-copy-free
    void* new_ptr = nullptr;
    KURD_t kurd = alloc(new_ptr, new_size, flags);
    if (!success_all_kurd(kurd)) return kurd;

    uint32_t copy_size = old_size < new_size ? old_size : new_size;
    if (copy_size > 0 && ptr && new_ptr)
        __builtin_memcpy(new_ptr, ptr, copy_size);

    kurd = free(ptr);
    if (!success_all_kurd(kurd)) {
        free(new_ptr);
        return kurd;
    }

    ptr = new_ptr;
    return success;
}

// ═══ clear ═══

KURD_t kpoolmemmgr_t::HCB_v2::clear(void* ptr)
{
    using namespace MEMMODULE_LOCATIONS::KPOOLMEMMGR_HCB_EVENTS;
    KURD_t success = kurd_default_success();
    KURD_t error   = kurd_default_error();
    success.event_code = EVENT_CODE_CLEAR;
    error.event_code   = EVENT_CODE_CLEAR;

    if (!valid) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_HEAP_NOT_ONLINE;
        return error;
    }
    if (!ptr) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    data_meta* meta = meta_from_ptr(ptr);
    if (meta->data_size == 0) {
        error.reason = COMMON_FAIL_REASONS::REASON_CODE_BAD_ADDR;
        return error;
    }

    __builtin_memset(ptr, 0, meta->data_size);
    return success;
}
