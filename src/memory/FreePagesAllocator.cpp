#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "panic.h"
#include "util/kout.h"
#include "util/arch/x86-64/cpuid_intel.h"
#ifdef KERNEL_MODE
#include "memory/kpoolmemmgr.h"
#endif
#ifdef USER_MODE
#include "new"
#include <unistd.h>
#include <stdlib.h>
#endif
namespace {
static uint64_t fpa_get_cpu_count()
{
return logical_processor_count;
}

static void fpa_wrapped_pgs_vfree(void* addr, uint64_t pages)
{
#ifdef KERNEL_MODE
    __wrapped_pgs_vfree(addr, pages);
#endif
#ifdef USER_MODE
    (void)addr;
    size_t bytes = static_cast<size_t>(pages) * 4096;
    void* tmp = malloc(bytes ? bytes : 1);
    (void)tmp;
#endif
}

static void* fpa_wrapped_pgs_valloc(KURD_t* kurd, uint64_t pages, page_state_t state, uint8_t align_log2)
{
#ifdef KERNEL_MODE
    return __wrapped_pgs_valloc(kurd, pages, state, align_log2);
#endif
#ifdef USER_MODE
    (void)state;
    (void)align_log2;
    if (kurd) {
        *kurd = KURD_t();
    }
    size_t bytes = static_cast<size_t>(pages) * 4096;
    void* ptr = malloc(bytes ? bytes : 1);
    if (!ptr && kurd) {
        kurd->result = result_code::FAIL;
        kurd->level = level_code::ERROR;
    }
    return ptr;
#endif
}
}
KURD_t FreePagesAllocator::default_kurd()
{
    return KURD_t(
            0, 0, module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
            0, 0, err_domain::CORE_MODULE
        );
}
KURD_t FreePagesAllocator::default_success()
{
    KURD_t kurd = default_kurd();
    kurd.result = result_code::SUCCESS;
    kurd.level = level_code::INFO;
    return kurd;
}
KURD_t FreePagesAllocator::default_retry()
{
    KURD_t kurd = default_kurd();
    kurd.result = result_code::RETRY;
    kurd.level = level_code::INFO;
    return kurd;
}
KURD_t FreePagesAllocator::default_error()
{
    return set_result_fail_and_error_level(default_kurd());
}
KURD_t FreePagesAllocator::default_fatal()
{
    return set_fatal_result_level(default_kurd());
}

FreePagesAllocator::flags_t FreePagesAllocator::flags;
uint64_t FreePagesAllocator::BCB_count;
FreePagesAllocator::BuddyControlBlock*FreePagesAllocator::BCBS;
all_pages_arr::free_segs_t* FreePagesAllocator::memory_crumbs;
fpa_stats* FreePagesAllocator::statistics_arr;
uint64_t*FreePagesAllocator::processors_preffered_bcb_idx;
namespace {
    struct BCB_plan_entry{
        phyaddr_t base;
        uint8_t order;
    };
    constexpr uint8_t min_bcb_order = 10;//4mb
    uint64_t g_all_avaliable_mem_accumulate = 0;
    Ktemplats::list_doubly<BCB_plan_entry>* g_bcb_candidate = nullptr;
}
KURD_t FreePagesAllocator::Init(strategy_t strategy,loaded_VM_interval* VM_intervals_bcbs_bitmap)
{
    flags.allow_new_BCB = false;
    g_all_avaliable_mem_accumulate = 0;

    KURD_t success(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_INIT,
        level_code::INFO, err_domain::CORE_MODULE
    );
    KURD_t fatal = set_fatal_result_level(success);

    all_pages_arr::free_segs_t* free_segs = all_pages_arr::free_segs_get();
    if (!free_segs) {
        return fatal;
    }
    auto free_free_segs = [&]() {
        if (!free_segs) return;
        if (free_segs->entries) {
            delete[] free_segs->entries;
            free_segs->entries = nullptr;
        }
        delete free_segs;
        free_segs = nullptr;
    };
    if (free_segs->count != 0 && free_segs->entries == nullptr) {
        free_free_segs();
        return fatal;
    }

    if (g_bcb_candidate == nullptr) {
        g_bcb_candidate = new Ktemplats::list_doubly<BCB_plan_entry>();
        if (g_bcb_candidate == nullptr) {
            free_free_segs();
            return fatal;
        }
    } else {
        g_bcb_candidate->clear();
    }

    // 重建 memory_crumbs（堆上持久对象，可被其它结构读取）。
    if (memory_crumbs != nullptr) {
        if (memory_crumbs->entries != nullptr) {
            delete[] memory_crumbs->entries;
            memory_crumbs->entries = nullptr;
        }
        delete memory_crumbs;
        memory_crumbs = nullptr;
    }
    memory_crumbs = new all_pages_arr::free_segs_t();
    if (memory_crumbs == nullptr) {
        free_free_segs();
        return fatal;
    }
    memory_crumbs->count = 0;
    memory_crumbs->entries = nullptr;

    for (uint64_t i = 0; i < free_segs->count; ++i) {
        auto& seg = free_segs->entries[i];
        g_all_avaliable_mem_accumulate += seg.size;

        uint64_t order_max = 0;
        phyaddr_t base = seg.base;
        if (seg.base != 0) {
            uint64_t tz = __builtin_ctzll(seg.base);
            order_max = (tz > 12) ? (tz - 12) : 0;
        }
        const phyaddr_t end = seg.base + seg.size;

        while (true) {
            uint8_t order = static_cast<uint8_t>(order_max);
            phyaddr_t top = base + (1ull << (order + 12));
            while (top > end) {
                if (order == 0) break;
                --order;
                top = base + (1ull << (order + 12));
            }

            // 候选阶段不过滤 order（0~63 都允许进入候选集）。
            g_bcb_candidate->push_back(BCB_plan_entry{base, order});

            if (top >= end) {
                break;
            }

            base = top;
            if (base == 0) {
                order_max = 0;
            } else {
                uint64_t tz = __builtin_ctzll(base);
                order_max = (tz > 12) ? (tz - 12) : 0;
            }
        }
    }
    free_free_segs();

    const uint64_t candidate_count = g_bcb_candidate->size();
    if (candidate_count == 0) {
        return fatal;
    }

    // 先将候选集分流：低于 min_bcb_order 的进入 memory_crumbs，其余进入 selected。
    Ktemplats::list_doubly<BCB_plan_entry> selected_candidates;
    uint64_t crumbs_count = 0;
    for (auto it = g_bcb_candidate->begin(); it != g_bcb_candidate->end(); ++it) {
        const BCB_plan_entry plan = *it;
        if (plan.order < min_bcb_order) {
            ++crumbs_count;
        } else {
            selected_candidates.push_back(plan);
        }
    }
    memory_crumbs->count = crumbs_count;
    if (crumbs_count > 0) {
        memory_crumbs->entries = new all_pages_arr::free_segs_t::entry_t[crumbs_count];
        if (memory_crumbs->entries == nullptr) {
            return fatal;
        }
        uint64_t out = 0;
        for (auto it = g_bcb_candidate->begin(); it != g_bcb_candidate->end(); ++it) {
            const BCB_plan_entry plan = *it;
            if (plan.order >= min_bcb_order) {
                continue;
            }
            if (plan.order >= 52) {
                continue;
            }
            memory_crumbs->entries[out++] = all_pages_arr::free_segs_t::entry_t{
                .base = plan.base,
                .size = (1ull << (plan.order + 12))
            };
        }
        memory_crumbs->count = out;
    }

    // strategy 变换：得到最终构造计划。
    Ktemplats::list_doubly<BCB_plan_entry> construct_plan;
    if (strategy.strategy == INIT_STRATEGY_MATCH_THREAD) {
        uint64_t cpu_count = fpa_get_cpu_count();
        if (cpu_count == 0) {
            cpu_count = 1;
        }
        uint64_t per_thread_bytes = g_all_avaliable_mem_accumulate / cpu_count;
        uint8_t thread_fit_order = 0;
        if (per_thread_bytes >= (1ull << 12)) {
            thread_fit_order = static_cast<uint8_t>((63 - __builtin_clzll(per_thread_bytes)) - 12);
        }
        if (thread_fit_order < min_bcb_order) {
            thread_fit_order = min_bcb_order;
        }

        for (auto it = selected_candidates.begin(); it != selected_candidates.end(); ++it) {
            const BCB_plan_entry plan = *it;
            if (plan.order <= thread_fit_order) {
                construct_plan.push_back(plan);
                continue;
            }
            const uint64_t split_count = 1ull << (plan.order - thread_fit_order);
            const uint64_t split_size = 1ull << (thread_fit_order + 12);
            for (uint64_t i = 0; i < split_count; ++i) {
                construct_plan.push_back(BCB_plan_entry{
                    static_cast<phyaddr_t>(plan.base + i * split_size),
                    thread_fit_order
                });
            }
        }
    } else {
        for (auto it = selected_candidates.begin(); it != selected_candidates.end(); ++it) {
            construct_plan.push_back(*it);
        }
    }

    const uint64_t plan_count = construct_plan.size();
    if (plan_count == 0) {
        return fatal;
    }

    // 将 construct_plan 排序为 base 单调递增，保证 BCBS 数组有序。
    BCB_plan_entry* sorted_plan = new BCB_plan_entry[plan_count];
    if (sorted_plan == nullptr) {
        return fatal;
    }
    uint64_t sorted_count = 0;
    for (auto it = construct_plan.begin(); it != construct_plan.end(); ++it) {
        sorted_plan[sorted_count++] = *it;
    }
    for (uint64_t i = 1; i < sorted_count; ++i) {
        BCB_plan_entry key = sorted_plan[i];
        uint64_t j = i;
        while (j > 0 && sorted_plan[j - 1].base > key.base) {
            sorted_plan[j] = sorted_plan[j - 1];
            --j;
        }
        sorted_plan[j] = key;
    }

    // Allocate storage for all BCB objects first, then placement-new one by one.
    uint8_t* bcb_storage = new uint8_t[plan_count * sizeof(BuddyControlBlock)];
    if (bcb_storage == nullptr) {
        delete[] sorted_plan;
        return fatal;
    }
    BCBS = reinterpret_cast<BuddyControlBlock*>(bcb_storage);

    const uint64_t bitmap_pool_base = VM_intervals_bcbs_bitmap ? VM_intervals_bcbs_bitmap->vbase : 0;
    const uint64_t bitmap_pool_size = VM_intervals_bcbs_bitmap ? VM_intervals_bcbs_bitmap->size : 0;
    uint64_t bitmap_cursor = bitmap_pool_base;
    uint64_t bitmap_end = bitmap_pool_base + bitmap_pool_size;

    auto align_up_u64 = [](uint64_t x, uint64_t a) -> uint64_t {
        if (a == 0) return x;
        return (x + (a - 1)) & ~(a - 1);
    };

    uint64_t constructed = 0;
    for (uint64_t pi = 0; pi < sorted_count; ++pi) {
        const BCB_plan_entry plan = sorted_plan[pi];
        const uint64_t need_bytes = (plan.order >= 2) ? (1ull << (plan.order - 2)) : 0;
        uint64_t alloc_base = align_up_u64(bitmap_cursor, 8);

        bool enough_bitmap = false;
        if (need_bytes == 0) {
            enough_bitmap = true;
        } else if (alloc_base <= bitmap_end && need_bytes <= (bitmap_end - alloc_base)) {
            enough_bitmap = true;
        }

        if (!enough_bitmap) {
            bsp_kout << "[FPA::Init] skip BCB(base=0x" << HEX << plan.base
                     << ", order=" << DEC << (uint64_t)plan.order
                     << ") bitmap bytes need=" << need_bytes
                     << " remain=" << ((alloc_base <= bitmap_end) ? (bitmap_end - alloc_base) : 0)
                     << kendl;
            continue;
        }

        new (BCBS + constructed) BuddyControlBlock(plan.base, plan.order);
        BCBS[constructed].corebcb_mixedbitmap_base_acclaim(alloc_base);
        ++constructed;
        bitmap_cursor = alloc_base + need_bytes;
    }
    delete[] sorted_plan;

    BCB_count = constructed;
    if (BCB_count == 0) {
        bsp_kout << "[FPA::Init] no BCB constructed due to bitmap pool shortage" << kendl;
        return fatal;
    }

    uint64_t processor_count = fpa_get_cpu_count();
    statistics_arr = new fpa_stats[processor_count];
    processors_preffered_bcb_idx = new uint64_t[processor_count];
    if (!statistics_arr || !processors_preffered_bcb_idx) {
        return fatal;
    }
    ksetmem_8(statistics_arr, 0, processor_count * sizeof(fpa_stats));
    ksetmem_64(processors_preffered_bcb_idx, ~0ULL, processor_count * sizeof(uint64_t));

    flags.allow_new_BCB = true;
    return success;
}
KURD_t FreePagesAllocator::second_stage(strategy_t strategy)
{
    (void)strategy;
    KURD_t kurd(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_INIT_SECOND_STAGE,
        level_code::INFO, err_domain::CORE_MODULE
    );
    return kurd;
}
all_pages_arr::free_segs_t* FreePagesAllocator::get_memory_crumbs()
{
    return memory_crumbs;
}
uint8_t size_to_order(uint64_t size)
{
    if (size == 0) {
        return 0;
    }
    
    // 计算需要多少个4KB页面（向上取整）
    uint64_t numof_4kbpgs = (size + 4095) / 4096;
    
    // 使用匿名函数计算向上取整的2的幂
    auto next_pow2 = [](uint64_t n) -> uint64_t {
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        n++;
        return n;
    };
    
    // 将页面数向上取整到2的幂
    uint64_t np2 = next_pow2(numof_4kbpgs);
    
    // 用__builtin_clzll取log2
    // 注意：np2 保证是2的幂且不为0
    return 63 - __builtin_clzll(np2);
}

phyaddr_t FreePagesAllocator::alloc
    (   
    uint64_t size, 
    buddy_alloc_params params,
    page_state_t interval_type,
    KURD_t&kurd
)
{
    constexpr uint16_t try_fail_max = 0x40;
    uint16_t retry_rounds = 0;

    KURD_t success = default_success();
    KURD_t fail = default_error();
    KURD_t retry = default_retry();

    success.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_ALLOC;
    fail.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_ALLOC;
    retry.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_ALLOC;

    using namespace MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::ALLOC_RESULTS_CODE;

    uint64_t processor_count = fpa_get_cpu_count();
    if (processor_count == 0) {
        processor_count = 1;
    }
    uint64_t pid = fast_get_processor_id();
    if (pid >= processor_count) {
        pid = pid % processor_count;
    }
    fpa_stats* stat_ptr = (statistics_arr != nullptr) ? &statistics_arr[pid] : nullptr;
    uint64_t* preferred_idx_ptr = (processors_preffered_bcb_idx != nullptr) ? &processors_preffered_bcb_idx[pid] : nullptr;
    uint64_t scan_this_alloc = 0;

    auto finalize_stats = [&]() {
        if (!stat_ptr) {
            return;
        }
        stat_ptr->bcb_scan_total += scan_this_alloc;
        if (scan_this_alloc > stat_ptr->bcb_scan_max) {
            stat_ptr->bcb_scan_max = scan_this_alloc;
        }
    };

    if (size == 0) {
        kurd = fail;
        kurd.reason = FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_SIZE;
        if (stat_ptr) {
            stat_ptr->alloc_fail++;
        }
        finalize_stats();
        return INVALID_ALLOC_BASE;
    }
    if (params.numa != 0) {
        kurd = fail;
        kurd.reason = FAIL_REASONS_CODE::FAIL_REASON_CODE_NUMA_NOT_SUPPORTED;
        if (stat_ptr) {
            stat_ptr->alloc_fail++;
        }
        finalize_stats();
        return INVALID_ALLOC_BASE;
    }
    if (BCBS == nullptr || BCB_count == 0) {
        kurd = fail;
        kurd.reason = FAIL_REASONS_CODE::FAIL_REASON_CODE_NO_AVALIABLE_BCB;
        if (stat_ptr) {
            stat_ptr->alloc_fail++;
        }
        finalize_stats();
        return INVALID_ALLOC_BASE;
    }
    if (stat_ptr) {
        stat_ptr->alloc_count++;
    }

    const uint8_t need_order = size_to_order(size);
    const uint64_t page_count = (size + 4095) >> 12;
    uint8_t* permanant_fail_map = reinterpret_cast<uint8_t*>(__builtin_alloca(BCB_count));
    ksetmem_8(permanant_fail_map, 0, BCB_count);
    uint64_t permanent_fail_count = 0;
    bool saw_busy_candidate = false;

    auto mark_permanent_fail = [&](uint64_t idx) {
        if (!permanant_fail_map[idx]) {
            permanant_fail_map[idx] = 1;
            ++permanent_fail_count;
        }
    };
    auto attempt_one_bcb = [&](uint64_t idx) -> phyaddr_t {
        if (idx >= BCB_count || permanant_fail_map[idx]) {
            return INVALID_ALLOC_BASE;
        }

        ++scan_this_alloc;
        BuddyControlBlock& bcb = BCBS[idx];
        if (!bcb.can_alloc(need_order)) {
            mark_permanent_fail(idx);
            return INVALID_ALLOC_BASE;
        }

        if (!bcb.lock.try_lock()) {
            saw_busy_candidate = true;
            if (stat_ptr) {
                stat_ptr->lock_try_fail++;
            }
            return INVALID_ALLOC_BASE;
        }

        phyaddr_t alloc_base = bcb.allocate_buddy_way(size, kurd, params.align_log2);
        if (error_kurd(kurd)) {
            bcb.lock.unlock();
            mark_permanent_fail(idx);
            return INVALID_ALLOC_BASE;
        }

        kurd = all_pages_arr::simp_pages_set(alloc_base, page_count, interval_type);
        if (error_kurd(kurd)) {
            (void)bcb.free_buddy_way(alloc_base, size);
            bcb.lock.unlock();
            mark_permanent_fail(idx);
            return INVALID_ALLOC_BASE;
        }

        bcb.lock.unlock();
        if (preferred_idx_ptr) {
            *preferred_idx_ptr = idx;
        }
        kurd = success;
        finalize_stats();
        return alloc_base;
    };

    while (true) {
        saw_busy_candidate = false;

        if (preferred_idx_ptr) {
            const uint64_t preferred_idx = *preferred_idx_ptr;
            if (preferred_idx < BCB_count && !permanant_fail_map[preferred_idx]) {
                phyaddr_t hit = attempt_one_bcb(preferred_idx);
                if (hit != INVALID_ALLOC_BASE) {
                    return hit;
                }
            }
        }

        for (uint64_t i = 0; i < BCB_count; ++i) {
            if (permanant_fail_map[i]) {
                continue;
            }
            if (preferred_idx_ptr && *preferred_idx_ptr < BCB_count && i == *preferred_idx_ptr) {
                continue;
            }
            phyaddr_t hit = attempt_one_bcb(i);
            if (hit != INVALID_ALLOC_BASE) {
                return hit;
            }
        }

        if (permanent_fail_count >= BCB_count) {
            kurd = fail;
            kurd.reason = FAIL_REASONS_CODE::FAIL_REASON_CODE_NO_AVALIABLE_BCB;
            if (stat_ptr) {
                stat_ptr->alloc_fail++;
            }
            finalize_stats();
            return INVALID_ALLOC_BASE;
        }

        if (params.try_lock_always_try) {
            continue;
        }

        if (saw_busy_candidate && retry_rounds < try_fail_max) {
            ++retry_rounds;
            continue;
        }

        if (saw_busy_candidate) {
            kurd = retry;
            kurd.reason = RETRY_REASONS_CODE::RETRY_REASON_CODE_TIME_OUT;
            if (stat_ptr) {
                stat_ptr->alloc_fail++;
            }
            finalize_stats();
            return INVALID_ALLOC_BASE;
        }

        kurd = retry;
        kurd.reason = RETRY_REASONS_CODE::RETRY_REASON_CODE_TIME_OUT;
        if (stat_ptr) {
            stat_ptr->alloc_fail++;
        }
        finalize_stats();
        return INVALID_ALLOC_BASE;
    }
}
KURD_t FreePagesAllocator::free(phyaddr_t base, uint64_t size)
{
    KURD_t success = default_success();
    KURD_t fail = default_error();

    success.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_FREE;
    fail.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::EVENT_CODE_FREE;
    using namespace MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::FREE_RESULTS_CODE;
    auto make_not_belong = [&]() -> KURD_t {
        KURD_t r = fail;
        r.reason = FAIL_REASONS_CODE::FAIL_REASON_CODE_BASE_NOT_BELONG;
        return r;
    };

    if (size == 0) {
        return make_not_belong();
    }
    if (BCBS == nullptr || BCB_count == 0) {
        return make_not_belong();
    }

    // BCBS 按 base 单调递增：先二分找最后一个 base <= addr 的 BCB。
    uint64_t lo = 0;
    uint64_t hi = BCB_count;
    while (lo < hi) {
        uint64_t mid = lo + ((hi - lo) >> 1);
        phyaddr_t mid_base = BCBS[mid].get_base();
        if (mid_base <= base) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return make_not_belong();
    }
    const uint64_t bcb_idx = lo - 1;
    BuddyControlBlock& bcb = BCBS[bcb_idx];

    const phyaddr_t bcb_base = bcb.get_base();
    const uint8_t bcb_order = bcb.get_max_order();
    const uint64_t bcb_span = (bcb_order < 52) ? (1ULL << (bcb_order + 12)) : 0;
    if (bcb_span == 0) {
        return make_not_belong();
    }
    if (base < bcb_base || base >= (bcb_base + bcb_span)) {
        return make_not_belong();
    }

    bcb.lock.lock();
    KURD_t bcb_kurd = bcb.free_buddy_way(base, size);
    if (error_kurd(bcb_kurd)) {
        bcb.lock.unlock();
        return bcb_kurd;
    }
    bcb.lock.unlock();

    uint64_t cpu_count = fpa_get_cpu_count();
    if (cpu_count == 0) {
        cpu_count = 1;
    }
    uint64_t pid = fast_get_processor_id();
    if (pid >= cpu_count) {
        pid %= cpu_count;
    }
    if (statistics_arr != nullptr) {
        statistics_arr[pid].free_count++;
    }
    return success;
}
fpa_stats FreePagesAllocator::get_fpa_stats()
{
    return get_fpa_stats(fast_get_processor_id());
}

fpa_stats FreePagesAllocator::get_fpa_stats(uint64_t pid)
{
    return statistics_arr[pid];
}

fpa_stats FreePagesAllocator::get_fpa_stats_all()
{
    fpa_stats total = {};
    uint64_t processor_count = 1;
        processor_count = fpa_get_cpu_count();
        if (processor_count == 0) {
            processor_count = 1;
        }
    

    for (uint64_t pid = 0; pid < processor_count; ++pid) {
        const fpa_stats& current = statistics_arr[pid];
        total.alloc_count += current.alloc_count;
        if (total.bcb_scan_max < current.bcb_scan_max) {
            total.bcb_scan_max = current.bcb_scan_max;
        }
        total.bcb_scan_total += current.bcb_scan_total;
        total.alloc_fail += current.alloc_fail;
        total.lock_try_fail += current.lock_try_fail;
        total.free_count += current.free_count;
    }
    return total;
}
