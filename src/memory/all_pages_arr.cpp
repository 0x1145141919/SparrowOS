#include "memory/all_pages_arr.h"
#include "abi/os_error_definitions.h"
#include "util/OS_utils.h"
#include "memory/kpoolmemmgr.h"
#include "linker_symbols.h"
#include "memory/phyaddr_accessor.h"
#include "util/kout.h"
#include "panic.h"
#include "util/kptrace.h"
uint64_t all_pages_arr::mem_map_entry_count;
page*all_pages_arr::mem_map;
all_pages_arr::phyinterval_t*all_pages_arr::mem_map_intervals;
uint64_t all_pages_arr::mem_map_intervals_count;
void *ptr_dump(page *p)
{
    return (void*)(0xFFFF000000000000+(uint64_t(p->head.ptr)<<4));
}

all_pages_arr::free_segs_t* all_pages_arr::free_segs_get()
{
    if (!mem_map || mem_map_entry_count == 0) {
        return nullptr;
    }
    auto is_free_4kb = [](const page& p) -> bool {
        return !p.page_flags.bitfield.is_skipped
            && p.head.order == 0
            && p.head.type == static_cast<uint64_t>(page_state_t::free)
            && p.refcount == 0;
    };

    uint64_t seg_count = 0;
    bool in_run = false;
    uint64_t run_start = 0;
    for (uint64_t idx = 0; idx < mem_map_entry_count; ++idx) {
        if (is_free_4kb(mem_map[idx])) {
            if (!in_run) {
                in_run = true;
                run_start = idx;
            }
        } else if (in_run) {
            seg_count++;
            in_run = false;
        }
    }
    if (in_run) {
        seg_count++;
    }

    free_segs_t* result = new free_segs_t();
    result->count = seg_count;
    if (seg_count == 0) {
        result->entries = nullptr;
        return result;
    }

    result->entries = new free_segs_t::entry_t[seg_count];
    uint64_t out_idx = 0;
    in_run = false;
    run_start = 0;
    for (uint64_t idx = 0; idx < mem_map_entry_count; ++idx) {
        if (is_free_4kb(mem_map[idx])) {
            if (!in_run) {
                in_run = true;
                run_start = idx;
            }
        } else if (in_run) {
            uint64_t run_len = idx - run_start;
            result->entries[out_idx++] = {
                .base = run_start << 12,
                .size = run_len << 12
            };
            in_run = false;
        }
    }
    if (in_run) {
        uint64_t run_len = mem_map_entry_count - run_start;
        result->entries[out_idx++] = {
            .base = run_start << 12,
            .size = run_len << 12
        };
    }

    return result;
}
all_pages_arr dram_map;
KURD_t all_pages_arr::Init(init_to_kernel_info *info)
{
    KURD_t result;
    phymem_segment*segs=info->memory_map;
    uint64_t physegs_count=info->phymem_segment_count;
    loaded_VM_interval*mem_map_interval=nullptr;
    for(int i=0;i<info->loaded_VM_interval_count;i++)
    { 
        if(info->loaded_VM_intervals[i].VM_interval_specifyid==VM_ID_MEM_MAP){
            mem_map_interval=&info->loaded_VM_intervals[i];
        }
    }
    if(!mem_map_interval){
        return set_fatal_result_level(KURD_t());
    }
    mem_map=(page*)mem_map_interval->vbase;
    mem_map_entry_count=mem_map_interval->size/sizeof(page);
    for(int i=0;i<physegs_count;i++)
    {
        if(segs[i].type==PHY_MEM_TYPE::freeSystemRam){
            mem_map_intervals_count++;
        }
    }
    mem_map_intervals=new phyinterval_t[mem_map_intervals_count];
    
    int interval_idx = 0;
    uint64_t entry_base_idx=0;
    for(int i=0;i<physegs_count;i++)
    {
        if(segs[i].type==PHY_MEM_TYPE::freeSystemRam){
            mem_map_intervals[interval_idx].base = segs[i].start;
            mem_map_intervals[interval_idx].numof4kbpgs = segs[i].size >> 12;
            mem_map_intervals[interval_idx].baseidx_in_memmap = entry_base_idx;
            entry_base_idx += mem_map_intervals[interval_idx].numof4kbpgs;
            interval_idx++;
        }
    }
    loaded_VM_interval*kintervals_base=info->loaded_VM_intervals;
    for(int i=0;i<info->loaded_VM_interval_count;i++){

        const loaded_VM_interval& interval = kintervals_base[i];
        page_state_t type = page_state_t::kernel_persisit;
        bool allocatable = false;
        if (interval.VM_interval_specifyid == VM_ID_GRAPHIC_BUFFER) {
            continue;
        }
        result=simp_pages_set(interval.pbase, interval.size >> 12, type);
    }
    simp_pages_set(info->kmmu_interval.start, info->kmmu_interval.size >> 12, page_state_t::kernel_persisit);
    simp_pages_set(info->kmmu_interval.start, info->kmmu_interval.size>> 12, page_state_t::kernel_persisit);
    PhyAddrAccessor::BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG=mem_map_entry_count<<12;
    return KURD_t();
}
KURD_t all_pages_arr::simp_pages_set(phyaddr_t phybase, uint64_t _4kbpgscount, page_state_t TYPE)
{
    KURD_t success(
        result_code::SUCCESS,
        0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_PAGES_ARR,
        MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::EVENT_CODE_SIMP_PAGES_SET,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
    KURD_t fail = set_result_fail_and_error_level(success);

    if (_4kbpgscount == 0) {
        return success;
    }

    if (mem_map == nullptr || mem_map_intervals == nullptr || mem_map_intervals_count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
        return fail;
    }

    // 1) Locate start interval by binary search.
    uint64_t lo = 0;
    uint64_t hi = mem_map_intervals_count;
    uint64_t start_iv_idx = mem_map_intervals_count;
    while (lo < hi) {
        const uint64_t mid = lo + ((hi - lo) >> 1);
        const phyinterval_t& iv = mem_map_intervals[mid];
        if (phybase < iv.base) {
            hi = mid;
            continue;
        }
        const uint64_t delta_pages = (phybase - iv.base) >> 12;
        if (delta_pages >= iv.numof4kbpgs) {
            lo = mid + 1;
            continue;
        }
        start_iv_idx = mid;
        break;
    }
    if (start_iv_idx == mem_map_intervals_count) {
        fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
        return fail;
    }

    // 2) Validate full range coverage across intervals (must not cross a hole).
    uint64_t iv_idx = start_iv_idx;
    uint64_t page_off = (phybase - mem_map_intervals[iv_idx].base) >> 12;
    uint64_t remain = _4kbpgscount;
    phyaddr_t cursor = phybase;

    while (remain > 0) {
        if (iv_idx >= mem_map_intervals_count) {
            fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
            return fail;
        }
        const phyinterval_t& iv = mem_map_intervals[iv_idx];
        if (cursor < iv.base || page_off >= iv.numof4kbpgs) {
            fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
            return fail;
        }

        const uint64_t can_take = iv.numof4kbpgs - page_off;
        const uint64_t take = (remain < can_take) ? remain : can_take;
        remain -= take;
        if (take > (UINT64_MAX >> 12)) {
            fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
            return fail;
        }
        const phyaddr_t add = static_cast<phyaddr_t>(take << 12);
        if (cursor + add < cursor) {
            fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
            return fail;
        }
        cursor += add;
        if (remain == 0) {
            break;
        }

        ++iv_idx;
        page_off = 0;
        if (iv_idx >= mem_map_intervals_count || cursor != mem_map_intervals[iv_idx].base) {
            fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
            return fail;
        }
    }

    // 3) Write mem_map directly by interval mapping.
    iv_idx = start_iv_idx;
    page_off = (phybase - mem_map_intervals[iv_idx].base) >> 12;
    remain = _4kbpgscount;
    while (remain > 0) {
        const phyinterval_t& iv = mem_map_intervals[iv_idx];
        const uint64_t can_take = iv.numof4kbpgs - page_off;
        const uint64_t take = (remain < can_take) ? remain : can_take;
        const uint64_t mem_idx_base = iv.baseidx_in_memmap + page_off;
        if (mem_idx_base >= mem_map_entry_count || take > (mem_map_entry_count - mem_idx_base)) {
            fail.reason = MEMMODULE_LOCAIONS::PAGES_ARR_EVENTS::SIMP_PAGES_SET_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM;
            return fail;
        }
        for (uint64_t j = 0; j < take; ++j) {
            mem_map[mem_idx_base + j].head.type = static_cast<uint64_t>(TYPE);
            mem_map[mem_idx_base + j].refcount = 1;
        }
        remain -= take;
        ++iv_idx;
        page_off = 0;
    }
    return success;
}
page* all_pages_arr::operator[](phyaddr_t phyaddr){
    if (mem_map == nullptr || mem_map_intervals == nullptr || mem_map_intervals_count == 0) {
        return nullptr;
    }

    uint64_t lo = 0;
    uint64_t hi = mem_map_intervals_count; // [lo, hi)

    while (lo < hi) {
        const uint64_t mid = lo + ((hi - lo) >> 1);
        const phyinterval_t& iv = mem_map_intervals[mid];

        if (phyaddr < iv.base) {
            hi = mid;
            continue;
        }

        const uint64_t delta_pages = (phyaddr - iv.base) >> 12;
        if (delta_pages >= iv.numof4kbpgs) {
            lo = mid + 1;
            continue;
        }

        const uint64_t mem_idx = iv.baseidx_in_memmap + delta_pages;
        if (mem_idx >= mem_map_entry_count) {
            return nullptr;
        }
        return &mem_map[mem_idx];
    }

    return nullptr;
}
