#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/AddresSpace.h"
#include "linker_symbols.h"
#include "abi/os_error_definitions.h"
#include "util/OS_utils.h"
#include "panic.h"
#ifdef USER_MODE
#include "stdlib.h"
#include "kpoolmemmgr.h"
#endif  
#include <limits.h>
#ifdef USER_MODE
    constexpr uint64_t FIRST_STATIC_HEAP_SIZE=1ULL<<24;
    
    kpoolmemmgr_t::HCB_v2::HCB_v2()
    {
        bitmap_controller.Init();
    }
#endif
kpoolmemmgr_t::HCB_v2::HCB_v2()
{
    statistics = {};
}
kpoolmemmgr_t::HCB_v2::HCB_v2(uint32_t size, vaddr_t vbase)
{
    this->total_size_in_bytes=size;
    this->vbase=vbase;
    ksetmem_8(&statistics,0,sizeof(HCB_statistics_t));
}

namespace {
static inline uint32_t size_bucket_index(uint32_t size)
{
    if (size == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < kpoolmemmgr_t::HCB_v2::SIZE_BUCKET_COUNT - 1; ++i) {
        uint32_t upper = 1u << (5 + i);
        if (size < upper) {
            return i;
        }
    }
    return kpoolmemmgr_t::HCB_v2::SIZE_BUCKET_COUNT - 1;
}

static inline void update_peak_used_bytes(
    kpoolmemmgr_t::HCB_v2::HCB_statistics_t& statistics,
    uint64_t used_bytes)
{
    if (used_bytes > statistics.peak_used_bytes_count) {
        statistics.peak_used_bytes_count = static_cast<uint32_t>(used_bytes);
    }
}
}
kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t kpoolmemmgr_t::HCB_v2::HCB_bitmap::param_checkment(uint64_t bit_idx, uint64_t bit_count)
{
    if(bit_idx+bit_count>bitmap_size_in_64bit_units*64){
        return kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t::HCB_BITMAP_BAD_PARAM;
    }
    if(bit_count>bitmap_size_in_64bit_units*64){
        return kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t::TOO_BIG_MEM_DEMAND;
    }
    return kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t::SUCCESS;
}
int kpoolmemmgr_t::HCB_v2::HCB_bitmap::Init(loaded_VM_interval *first_static_heap_bitmap)
{
    bitmap=(uint64_t*)first_static_heap_bitmap->vbase;
    bitmap_used_bit=0;
    bitmap_size_in_64bit_units=first_static_heap_bitmap->size/sizeof(uint64_t);
    byte_bitmap_base=(uint8_t*)bitmap;
    scan_cache.hint_u64_idx = invalid_cache;
    scan_cache.last_success_idx = invalid_cache;
    scan_cache.largest_free_hint_len = 0;
    scan_cache.largest_free_hint_base = invalid_cache;
    statistics = {};
    return OS_SUCCESS;
}
KURD_t kpoolmemmgr_t::HCB_v2::HCB_bitmap::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB_BITMAP,0,0,err_domain::CORE_MODULE);
}
KURD_t kpoolmemmgr_t::HCB_v2::HCB_bitmap::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}
KURD_t kpoolmemmgr_t::HCB_v2::HCB_bitmap::default_fail()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}
KURD_t kpoolmemmgr_t::HCB_v2::HCB_bitmap::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}
KURD_t kpoolmemmgr_t::HCB_v2::HCB_bitmap::second_stage_Init(uint32_t entries_count)
{
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_BITMAP_EVENTS::EVENT_CODE_INIT;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_BITMAP_EVENTS::EVENT_CODE_INIT;
    if(this==&kpoolmemmgr_t::first_linekd_heap.bitmap_controller){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_BITMAP_EVENTS::INIT_RESULTS::FAIL_RESONS::REASON_CODE_HCB_BITMAP_INIT_FAIL;
    }
    bitmap_size_in_64bit_units=entries_count/64;
    KURD_t kurd;
#ifdef KERNEL_MODE
    this->bitmap=(uint64_t*)__wrapped_pgs_valloc(
     &kurd,align_up(bitmap_size_in_64bit_units,512)/512,page_state_t::kernel_pinned,12
    );
#endif
#ifdef USER_MODE
    this->bitmap=(uint64_t*)malloc((bitmap_size_in_64bit_units*8));
#endif
    if(this->bitmap==nullptr||kurd.result!=result_code::SUCCESS)return kurd;
    ksetmem_8(bitmap,0,bitmap_size_in_64bit_units*8);
    byte_bitmap_base=(uint8_t*)this->bitmap;
    scan_cache.hint_u64_idx = invalid_cache;
    scan_cache.last_success_idx = invalid_cache;
    scan_cache.largest_free_hint_len = 0;
    scan_cache.largest_free_hint_base = invalid_cache;
    statistics = {};
    return success;
}
kpoolmemmgr_t::HCB_v2::HCB_bitmap::HCB_bitmap()
{
    scan_cache.hint_u64_idx = invalid_cache;
    scan_cache.last_success_idx = invalid_cache;
    scan_cache.largest_free_hint_len = 0;
    scan_cache.largest_free_hint_base = invalid_cache;
    statistics = {};
}
kpoolmemmgr_t::HCB_v2::HCB_bitmap::~HCB_bitmap()
{
    byte_bitmap_base=nullptr;
    #ifdef KERNEL_MODE 
    KURD_t status=__wrapped_pgs_vfree(bitmap,align_up(bitmap_size_in_64bit_units,512)/512);
    panic_info_inshort inshort={
        .is_bug=true,
        .is_policy=false,
        .is_hw_fault=false,
        .is_mem_corruption=false,
        .is_escalated=false,  
    };

    if(status.result!=result_code::SUCCESS){
        Panic::panic(default_panic_behaviors_flags,"kpoolmemmgr_t::HCB_v2::HCB_bitmap::~HCB_bitmap cancel memmap failed",nullptr,&inshort,status);
    }

    #endif
    #ifdef USER_MODE
    std::free(this->bitmap);
    #endif
    this->bitmap=nullptr;
}
kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t
kpoolmemmgr_t::HCB_v2::HCB_bitmap::continual_avaliable_bits_search(uint64_t bit_count, uint64_t &result_base_idx)
{
    statistics.call_bits_scan_count++;
    uint64_t steps = 0;
    if (bit_count == 0) {
        return SUCCESS;
    }
    const uint64_t total_bits = bitmap_size_in_64bit_units << 6;
    const uint64_t needed_u64s = (bit_count + 63) >> 6;

    auto is_bits_free = [&](uint64_t start_bit) -> bool {
        if (start_bit > total_bits || start_bit + bit_count > total_bits) {
            return false;
        }
        for (uint64_t i = 0; i < bit_count; ++i) {
            if (bit_get(start_bit + i)) {
                return false;
            }
        }
        return true;
    };

    if (scan_cache.largest_free_hint_len >= needed_u64s &&
        scan_cache.largest_free_hint_base != invalid_cache) {
        uint64_t base_bit = scan_cache.largest_free_hint_base << 6;
        if (is_bits_free(base_bit)) {
            result_base_idx = base_bit;
            scan_cache.last_success_idx = scan_cache.largest_free_hint_base;
            uint64_t next = scan_cache.largest_free_hint_base + needed_u64s;
            scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.bits_scan_largest_free_hint_hit_count++;
            statistics.bits_scan_all_steps_accum += steps;
            if (steps > statistics.bits_scan_max_steps_in_a_term) {
                statistics.bits_scan_max_steps_in_a_term = steps;
            }
            return SUCCESS;
        } else {
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.bits_scan_largest_free_hint_miss_count++;
        }
    }

    uint64_t start_bit = 0;
    if (scan_cache.hint_u64_idx != invalid_cache &&
        scan_cache.hint_u64_idx < bitmap_size_in_64bit_units) {
        start_bit = scan_cache.hint_u64_idx << 6;
    } else {
        scan_cache.hint_u64_idx = invalid_cache;
    }

    auto scan_range = [&](uint64_t range_start, uint64_t range_end) -> HCB_bitmap_error_code_t {
        if (range_start >= range_end) {
            return AVALIBLE_MEMSEG_SEARCH_FAIL;
        }
        uint64_t seg_base_bit_idx = range_start;
        bool seg_value = bit_get(range_start);
        uint64_t seg_length = 1;
        steps++;

        for (uint64_t current_bit_idx = range_start + 1; current_bit_idx < range_end; ++current_bit_idx) {
            steps++;
            if ((current_bit_idx & 0x3F) == 0) {
                uint64_t* to_scan_u64 = reinterpret_cast<uint64_t*>(
                    byte_bitmap_base + (current_bit_idx >> 3));
                if (seg_value) {
                    if (*to_scan_u64 == U64_FULL_UNIT) {
                        seg_length += 64;
                        steps += 63;
                        current_bit_idx += 63;
                        continue;
                    }
                } else {
                    if (*to_scan_u64 == 0x00) {
                        seg_length += 64;
                        steps += 63;
                        if (seg_length >= bit_count) {
                            result_base_idx = seg_base_bit_idx;
                            scan_cache.last_success_idx = seg_base_bit_idx >> 6;
                            uint64_t next = scan_cache.last_success_idx + needed_u64s;
                            scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
                            scan_cache.largest_free_hint_len = 0;
                            scan_cache.largest_free_hint_base = invalid_cache;
                            statistics.bits_scan_all_steps_accum += steps;
                            if (steps > statistics.bits_scan_max_steps_in_a_term) {
                                statistics.bits_scan_max_steps_in_a_term = steps;
                            }
                            return SUCCESS;
                        }
                        current_bit_idx += 63;
                        continue;
                    }
                }
            }

            bool new_bit_value = bit_get(current_bit_idx);
            if (seg_value == new_bit_value) {
                seg_length++;
                if (!seg_value && seg_length >= bit_count) {
                    result_base_idx = seg_base_bit_idx;
                    scan_cache.last_success_idx = seg_base_bit_idx >> 6;
                    uint64_t next = scan_cache.last_success_idx + needed_u64s;
                    scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
                    scan_cache.largest_free_hint_len = 0;
                    scan_cache.largest_free_hint_base = invalid_cache;
                    statistics.bits_scan_all_steps_accum += steps;
                    if (steps > statistics.bits_scan_max_steps_in_a_term) {
                        statistics.bits_scan_max_steps_in_a_term = steps;
                    }
                    return SUCCESS;
                }
            } else {
                if (!seg_value) {
                    uint64_t seg_u64_len = seg_length >> 6;
                    if (seg_u64_len > scan_cache.largest_free_hint_len) {
                        scan_cache.largest_free_hint_len = seg_u64_len;
                        scan_cache.largest_free_hint_base = seg_base_bit_idx >> 6;
                    }
                }
                seg_base_bit_idx = current_bit_idx;
                seg_value = new_bit_value;
                seg_length = 1;
            }
        }

        if (!seg_value) {
            if (seg_length >= bit_count) {
                result_base_idx = seg_base_bit_idx;
                scan_cache.last_success_idx = seg_base_bit_idx >> 6;
                uint64_t next = scan_cache.last_success_idx + needed_u64s;
                scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
                scan_cache.largest_free_hint_len = 0;
                scan_cache.largest_free_hint_base = invalid_cache;
                statistics.bits_scan_all_steps_accum += steps;
                if (steps > statistics.bits_scan_max_steps_in_a_term) {
                    statistics.bits_scan_max_steps_in_a_term = steps;
                }
                return SUCCESS;
            }
            uint64_t seg_u64_len = seg_length >> 6;
            if (seg_u64_len > scan_cache.largest_free_hint_len) {
                scan_cache.largest_free_hint_len = seg_u64_len;
                scan_cache.largest_free_hint_base = seg_base_bit_idx >> 6;
            }
        }
        return AVALIBLE_MEMSEG_SEARCH_FAIL;
    };

    HCB_bitmap_error_code_t rc = scan_range(start_bit, total_bits);
    if (rc == SUCCESS) {
        return rc;
    }
    if (start_bit != 0) {
        rc = scan_range(0, start_bit);
        if (rc == SUCCESS) {
            return rc;
        }
    }

    if (scan_cache.largest_free_hint_len == 0) {
        scan_cache.largest_free_hint_base = invalid_cache;
    }
    scan_cache.hint_u64_idx = invalid_cache;
    statistics.bits_scan_all_steps_accum += steps;
    if (steps > statistics.bits_scan_max_steps_in_a_term) {
        statistics.bits_scan_max_steps_in_a_term = steps;
    }
    return AVALIBLE_MEMSEG_SEARCH_FAIL;
}

kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t
kpoolmemmgr_t::HCB_v2::HCB_bitmap::continual_avaliable_bytes_search(uint64_t byte_count, uint64_t &result_base_idx)
{
    statistics.call_bytes_scan_count++;
    uint64_t steps = 0;
    if (byte_count == 0) {
        return SUCCESS;
    }
    const uint64_t total_bytes = bitmap_size_in_64bit_units * 8;
    const uint64_t needed_u64s = (byte_count + 7) >> 3;

    auto is_bytes_free = [&](uint64_t start_byte) -> bool {
        if (start_byte > total_bytes || start_byte + byte_count > total_bytes) {
            return false;
        }
        for (uint64_t i = 0; i < byte_count; ++i) {
            if (byte_bitmap_base[start_byte + i]) {
                return false;
            }
        }
        return true;
    };

    if (scan_cache.largest_free_hint_len >= needed_u64s &&
        scan_cache.largest_free_hint_base != invalid_cache) {
        uint64_t base_byte = scan_cache.largest_free_hint_base << 3;
        if (is_bytes_free(base_byte)) {
            result_base_idx = base_byte;
            scan_cache.last_success_idx = scan_cache.largest_free_hint_base;
            uint64_t next = scan_cache.largest_free_hint_base + needed_u64s;
            scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.bytes_scan_largest_free_hint_hit_count++;
            statistics.bytes_scan_all_steps_accum += steps;
            if (steps > statistics.bytes_scan_max_steps_in_a_term) {
                statistics.bytes_scan_max_steps_in_a_term = steps;
            }
            return SUCCESS;
        } else {
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.bytes_scan_largest_free_hint_miss_count++;
        }
    }

    uint64_t start_byte = 0;
    if (scan_cache.hint_u64_idx != invalid_cache &&
        scan_cache.hint_u64_idx < bitmap_size_in_64bit_units) {
        start_byte = scan_cache.hint_u64_idx << 3;
    } else {
        scan_cache.hint_u64_idx = invalid_cache;
    }

    auto scan_range = [&](uint64_t range_start, uint64_t range_end) -> HCB_bitmap_error_code_t {
        if (range_start >= range_end) {
            return AVALIBLE_MEMSEG_SEARCH_FAIL;
        }
        uint64_t seg_base_byte_idx = range_start;
        bool seg_value = !!byte_bitmap_base[range_start];
        uint64_t seg_length = 1;
        steps++;

        for (uint64_t current_byte_idx = range_start + 1; current_byte_idx < range_end; ++current_byte_idx) {
            steps++;
            if ((current_byte_idx & 0x7) == 0) {
                uint64_t* to_scan_u64 = reinterpret_cast<uint64_t*>(
                    byte_bitmap_base + current_byte_idx);
                if (seg_value) {
                    if (*to_scan_u64 == U64_FULL_UNIT) {
                        seg_length += 8;
                        steps += 7;
                        current_byte_idx += 7;
                        continue;
                    }
                } else {
                    if (*to_scan_u64 == 0x00ULL) {
                        seg_length += 8;
                        steps += 7;
                        if (seg_length >= byte_count) {
                            result_base_idx = seg_base_byte_idx;
                            scan_cache.last_success_idx = seg_base_byte_idx >> 3;
                            uint64_t next = scan_cache.last_success_idx + needed_u64s;
                            scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
                            scan_cache.largest_free_hint_len = 0;
                            scan_cache.largest_free_hint_base = invalid_cache;
                            statistics.bytes_scan_all_steps_accum += steps;
                            if (steps > statistics.bytes_scan_max_steps_in_a_term) {
                                statistics.bytes_scan_max_steps_in_a_term = steps;
                            }
                            return SUCCESS;
                        }
                        current_byte_idx += 7;
                        continue;
                    }
                }
            }

            bool new_value = !!byte_bitmap_base[current_byte_idx];
            if (seg_value == new_value) {
                seg_length++;
                if (!seg_value && seg_length >= byte_count) {
                    result_base_idx = seg_base_byte_idx;
                    scan_cache.last_success_idx = seg_base_byte_idx >> 3;
                    uint64_t next = scan_cache.last_success_idx + needed_u64s;
                    scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
                    scan_cache.largest_free_hint_len = 0;
                    scan_cache.largest_free_hint_base = invalid_cache;
                    statistics.bytes_scan_all_steps_accum += steps;
                    if (steps > statistics.bytes_scan_max_steps_in_a_term) {
                        statistics.bytes_scan_max_steps_in_a_term = steps;
                    }
                    return SUCCESS;
                }
            } else {
                if (!seg_value) {
                    uint64_t seg_u64_len = seg_length >> 3;
                    if (seg_u64_len > scan_cache.largest_free_hint_len) {
                        scan_cache.largest_free_hint_len = seg_u64_len;
                        scan_cache.largest_free_hint_base = seg_base_byte_idx >> 3;
                    }
                }
                seg_base_byte_idx = current_byte_idx;
                seg_value = new_value;
                seg_length = 1;
            }
        }

        if (!seg_value) {
            if (seg_length >= byte_count) {
                result_base_idx = seg_base_byte_idx;
                scan_cache.last_success_idx = seg_base_byte_idx >> 3;
                uint64_t next = scan_cache.last_success_idx + needed_u64s;
                scan_cache.hint_u64_idx = next < bitmap_size_in_64bit_units ? next : invalid_cache;
                scan_cache.largest_free_hint_len = 0;
                scan_cache.largest_free_hint_base = invalid_cache;
                statistics.bytes_scan_all_steps_accum += steps;
                if (steps > statistics.bytes_scan_max_steps_in_a_term) {
                    statistics.bytes_scan_max_steps_in_a_term = steps;
                }
                return SUCCESS;
            }
            uint64_t seg_u64_len = seg_length >> 3;
            if (seg_u64_len > scan_cache.largest_free_hint_len) {
                scan_cache.largest_free_hint_len = seg_u64_len;
                scan_cache.largest_free_hint_base = seg_base_byte_idx >> 3;
            }
        }
        return AVALIBLE_MEMSEG_SEARCH_FAIL;
    };

    HCB_bitmap_error_code_t rc = scan_range(start_byte, total_bytes);
    if (rc == SUCCESS) {
        return rc;
    }
    if (start_byte != 0) {
        rc = scan_range(0, start_byte);
        if (rc == SUCCESS) {
            return rc;
        }
    }

    if (scan_cache.largest_free_hint_len == 0) {
        scan_cache.largest_free_hint_base = invalid_cache;
    }
    scan_cache.hint_u64_idx = invalid_cache;
    statistics.bytes_scan_all_steps_accum += steps;
    if (steps > statistics.bytes_scan_max_steps_in_a_term) {
        statistics.bytes_scan_max_steps_in_a_term = steps;
    }
    return AVALIBLE_MEMSEG_SEARCH_FAIL;
}

kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t
kpoolmemmgr_t::HCB_v2::HCB_bitmap::continual_avaliable_u64s_search(uint64_t u64_count, uint64_t &result_base_idx)
{
    statistics.call_u64s_scan_count++;
    uint64_t steps = 0;
    if (u64_count == 0) {
        return SUCCESS;
    }
    const uint64_t total_u64s = bitmap_size_in_64bit_units;

    auto is_u64s_free = [&](uint64_t start_u64) -> bool {
        if (start_u64 > total_u64s || start_u64 + u64_count > total_u64s) {
            return false;
        }
        for (uint64_t i = 0; i < u64_count; ++i) {
            if (bitmap[start_u64 + i] != 0) {
                return false;
            }
        }
        return true;
    };

    if (scan_cache.largest_free_hint_len >= u64_count &&
        scan_cache.largest_free_hint_base != invalid_cache) {
        uint64_t base = scan_cache.largest_free_hint_base;
        if (is_u64s_free(base)) {
            result_base_idx = base;
            scan_cache.last_success_idx = base;
            uint64_t next = base + u64_count;
            scan_cache.hint_u64_idx = next < total_u64s ? next : invalid_cache;
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.u64s_scan_largest_free_hint_hit_count++;
            statistics.u64s_scan_all_steps_accum += steps;
            if (steps > statistics.u64s_scan_max_steps_in_a_term) {
                statistics.u64s_scan_max_steps_in_a_term = steps;
            }
            return SUCCESS;
        } else {
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.u64s_scan_largest_free_hint_miss_count++;
        }
    }

    uint64_t start_u64 = 0;
    if (scan_cache.hint_u64_idx != invalid_cache &&
        scan_cache.hint_u64_idx < total_u64s) {
        start_u64 = scan_cache.hint_u64_idx;
    } else {
        scan_cache.hint_u64_idx = invalid_cache;
    }

    auto scan_range = [&](uint64_t range_start, uint64_t range_end) -> HCB_bitmap_error_code_t {
        if (range_start >= range_end) {
            return AVALIBLE_MEMSEG_SEARCH_FAIL;
        }
        uint64_t seg_base_u64_idx = range_start;
        bool seg_value = !!bitmap[range_start];
        uint64_t seg_length = 1;
        steps++;

        for (uint64_t current_u64_idx = range_start + 1; current_u64_idx < range_end; ++current_u64_idx) {
            steps++;
            bool new_value = !!bitmap[current_u64_idx];
            if (seg_value == new_value) {
                seg_length++;
                if (!seg_value && seg_length >= u64_count) {
                    result_base_idx = seg_base_u64_idx;
                    scan_cache.last_success_idx = seg_base_u64_idx;
                    uint64_t next = seg_base_u64_idx + u64_count;
                    scan_cache.hint_u64_idx = next < total_u64s ? next : invalid_cache;
                    scan_cache.largest_free_hint_len = 0;
                    scan_cache.largest_free_hint_base = invalid_cache;
                    statistics.u64s_scan_all_steps_accum += steps;
                    if (steps > statistics.u64s_scan_max_steps_in_a_term) {
                        statistics.u64s_scan_max_steps_in_a_term = steps;
                    }
                    return SUCCESS;
                }
            } else {
                if (!seg_value) {
                    if (seg_length > scan_cache.largest_free_hint_len) {
                        scan_cache.largest_free_hint_len = seg_length;
                        scan_cache.largest_free_hint_base = seg_base_u64_idx;
                    }
                }
                seg_base_u64_idx = current_u64_idx;
                seg_value = new_value;
                seg_length = 1;
            }
        }

        if (!seg_value) {
            if (seg_length >= u64_count) {
                result_base_idx = seg_base_u64_idx;
                scan_cache.last_success_idx = seg_base_u64_idx;
                uint64_t next = seg_base_u64_idx + u64_count;
                scan_cache.hint_u64_idx = next < total_u64s ? next : invalid_cache;
                scan_cache.largest_free_hint_len = 0;
                scan_cache.largest_free_hint_base = invalid_cache;
                statistics.u64s_scan_all_steps_accum += steps;
                if (steps > statistics.u64s_scan_max_steps_in_a_term) {
                    statistics.u64s_scan_max_steps_in_a_term = steps;
                }
                return SUCCESS;
            }
            if (seg_length > scan_cache.largest_free_hint_len) {
                scan_cache.largest_free_hint_len = seg_length;
                scan_cache.largest_free_hint_base = seg_base_u64_idx;
            }
        }
        return AVALIBLE_MEMSEG_SEARCH_FAIL;
    };

    HCB_bitmap_error_code_t rc = scan_range(start_u64, total_u64s);
    if (rc == SUCCESS) {
        return rc;
    }
    if (start_u64 != 0) {
        rc = scan_range(0, start_u64);
        if (rc == SUCCESS) {
            return rc;
        }
    }

    if (scan_cache.largest_free_hint_len == 0) {
        scan_cache.largest_free_hint_base = invalid_cache;
    }
    scan_cache.hint_u64_idx = invalid_cache;
    statistics.u64s_scan_all_steps_accum += steps;
    if (steps > statistics.u64s_scan_max_steps_in_a_term) {
        statistics.u64s_scan_max_steps_in_a_term = steps;
    }
    return AVALIBLE_MEMSEG_SEARCH_FAIL;
}
kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t
kpoolmemmgr_t::HCB_v2::HCB_bitmap::
continual_avaliable_u64s_search_higher_alignment(
    uint64_t u64idx_align_log2,
    uint64_t u64_count,
    uint64_t &result_base_idx
)
{
    statistics.call_u64s_scan_higher_alignment_count++;
    uint64_t steps = 0;
    if (u64_count == 0)
        return SUCCESS;

    const uint64_t total_u64s = bitmap_size_in_64bit_units;

    // 用完即弃的对齐计算
    auto align_u64_idx = [&](uint64_t idx) -> uint64_t {
        if (u64idx_align_log2 == 0)
            return idx;
        const uint64_t align = 1ULL << u64idx_align_log2;
        return (idx + align - 1) & ~(align - 1);
    };

    auto is_u64s_free = [&](uint64_t start_u64) -> bool {
        if (start_u64 > total_u64s || start_u64 + u64_count > total_u64s) {
            return false;
        }
        for (uint64_t i = 0; i < u64_count; ++i) {
            if (bitmap[start_u64 + i] != 0) {
                return false;
            }
        }
        return true;
    };

    if (scan_cache.largest_free_hint_len >= u64_count &&
        scan_cache.largest_free_hint_base != invalid_cache) {
        uint64_t seg_base = scan_cache.largest_free_hint_base;
        uint64_t seg_end = seg_base + scan_cache.largest_free_hint_len;
        uint64_t aligned_base = align_u64_idx(seg_base);
        if (aligned_base >= seg_base &&
            aligned_base + u64_count <= seg_end &&
            is_u64s_free(aligned_base)) {
            result_base_idx = aligned_base;
            scan_cache.last_success_idx = aligned_base;
            uint64_t next = aligned_base + u64_count;
            scan_cache.hint_u64_idx = next < total_u64s ? next : invalid_cache;
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.u64s_scan_largest_free_hint_hit_count++;
            statistics.u64s_scan_all_steps_accum += steps;
            if (steps > statistics.u64s_scan_max_steps_in_a_term) {
                statistics.u64s_scan_max_steps_in_a_term = steps;
            }
            return SUCCESS;
        } else {
            scan_cache.largest_free_hint_len = 0;
            scan_cache.largest_free_hint_base = invalid_cache;
            statistics.u64s_scan_largest_free_hint_miss_count++;
        }
    }

    uint64_t start_u64 = 0;
    if (scan_cache.hint_u64_idx != invalid_cache &&
        scan_cache.hint_u64_idx < total_u64s) {
        start_u64 = scan_cache.hint_u64_idx;
    } else {
        scan_cache.hint_u64_idx = invalid_cache;
    }

    auto scan_range = [&](uint64_t range_start, uint64_t range_end) -> HCB_bitmap_error_code_t {
        if (range_start >= range_end) {
            return AVALIBLE_MEMSEG_SEARCH_FAIL;
        }
        uint64_t seg_base_u64_idx = range_start;
        bool seg_value = !!bitmap[range_start];
        uint64_t seg_length = 1;
        steps++;

        for (uint64_t cur_idx = range_start + 1; cur_idx < range_end; ++cur_idx) {
            steps++;
            bool new_value = !!bitmap[cur_idx];
            if (new_value == seg_value) {
                seg_length++;
            } else {
                if (!seg_value) {
                    uint64_t seg_end = seg_base_u64_idx + seg_length;
                    uint64_t aligned_base = align_u64_idx(seg_base_u64_idx);
                    if (aligned_base >= seg_base_u64_idx &&
                        aligned_base + u64_count <= seg_end) {
                        result_base_idx = aligned_base;
                        scan_cache.last_success_idx = aligned_base;
                        uint64_t next = aligned_base + u64_count;
                        scan_cache.hint_u64_idx = next < total_u64s ? next : invalid_cache;
                        scan_cache.largest_free_hint_len = 0;
                        scan_cache.largest_free_hint_base = invalid_cache;
                        statistics.u64s_scan_all_steps_accum += steps;
                        if (steps > statistics.u64s_scan_max_steps_in_a_term) {
                            statistics.u64s_scan_max_steps_in_a_term = steps;
                        }
                        return SUCCESS;
                    }
                    if (seg_length > scan_cache.largest_free_hint_len) {
                        scan_cache.largest_free_hint_len = seg_length;
                        scan_cache.largest_free_hint_base = seg_base_u64_idx;
                    }
                }
                seg_base_u64_idx = cur_idx;
                seg_value = new_value;
                seg_length = 1;
            }
        }

        if (!seg_value) {
            uint64_t seg_end = seg_base_u64_idx + seg_length;
            uint64_t aligned_base = align_u64_idx(seg_base_u64_idx);
            if (aligned_base >= seg_base_u64_idx &&
                aligned_base + u64_count <= seg_end) {
                result_base_idx = aligned_base;
                scan_cache.last_success_idx = aligned_base;
                uint64_t next = aligned_base + u64_count;
                scan_cache.hint_u64_idx = next < total_u64s ? next : invalid_cache;
                scan_cache.largest_free_hint_len = 0;
                scan_cache.largest_free_hint_base = invalid_cache;
                statistics.u64s_scan_all_steps_accum += steps;
                if (steps > statistics.u64s_scan_max_steps_in_a_term) {
                    statistics.u64s_scan_max_steps_in_a_term = steps;
                }
                return SUCCESS;
            }
            if (seg_length > scan_cache.largest_free_hint_len) {
                scan_cache.largest_free_hint_len = seg_length;
                scan_cache.largest_free_hint_base = seg_base_u64_idx;
            }
        }
        return AVALIBLE_MEMSEG_SEARCH_FAIL;
    };

    HCB_bitmap_error_code_t rc = scan_range(start_u64, total_u64s);
    if (rc == SUCCESS) {
        return rc;
    }
    if (start_u64 != 0) {
        rc = scan_range(0, start_u64);
        if (rc == SUCCESS) {
            return rc;
        }
    }

    if (scan_cache.largest_free_hint_len == 0) {
        scan_cache.largest_free_hint_base = invalid_cache;
    }
    scan_cache.hint_u64_idx = invalid_cache;
    statistics.u64s_scan_all_steps_accum += steps;
    if (steps > statistics.u64s_scan_max_steps_in_a_term) {
        statistics.u64s_scan_max_steps_in_a_term = steps;
    }
    return AVALIBLE_MEMSEG_SEARCH_FAIL;
}

bool kpoolmemmgr_t::HCB_v2::HCB_bitmap::target_bit_seg_is_avaliable
(uint64_t bit_idx, 
    uint64_t bit_count,
kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t&err)
{
    if (bit_count == 0) return true;
    err=param_checkment(bit_idx,bit_count);
    if(err!=SUCCESS){
        return false;
    }

    bitmap_rwlock.read_lock();

    uint64_t start_bit = bit_idx;
    uint64_t end_bit = bit_idx + bit_count;

    uint64_t start_u64 = start_bit >> 6;
    uint64_t end_u64   = (end_bit - 1) >> 6;  // inclusive
    uint64_t start_off = start_bit & 63;
    uint64_t end_off   = end_bit & 63;

    // 1️⃣ 如果目标区间小于64bit，直接逐bit检查，可读性高
    if (bit_count <= 64)
    {
        for (uint64_t i = 0; i < bit_count; ++i)
        {
            if (bit_get(bit_idx + i))
            {
                bitmap_rwlock.read_unlock();
                return false;
            }
        }
        bitmap_rwlock.read_unlock();
        return true;
    }

    // 2️⃣ 跨越多个 u64 块的情况
    // ---- 前导未对齐部分 ----
    if (start_off != 0)
    {
        uint64_t mask = (~0ULL) << start_off;
        if (bitmap[start_u64] & mask)
        {
            bitmap_rwlock.read_unlock();
            return false;
        }
        start_u64++;
    }

    // ---- 中间完整的 u64 区域 ----
    for (uint64_t i = start_u64; i < end_u64; ++i)
    {
        if (bitmap[i] != 0ULL)
        {
            bitmap_rwlock.read_unlock();
            return false;
        }
    }

    // ---- 尾部未对齐部分 ----
    if (end_off != 0)
    {
        uint64_t mask = (1ULL << end_off) - 1ULL;
        if (bitmap[end_u64] & mask)
        {
            bitmap_rwlock.read_unlock();
            return false;
        }
    }

    bitmap_rwlock.read_unlock();
    return true;
}

kpoolmemmgr_t::HCB_v2::HCB_bitmap_error_code_t
 kpoolmemmgr_t::HCB_v2::HCB_bitmap::bits_seg_set(uint64_t bit_idx, uint64_t bit_count, bool value)
{
    if (bit_count == 0) return SUCCESS;
    statistics.call_bits_seg_set_count++;

    HCB_bitmap_error_code_t status=param_checkment(bit_idx,bit_count);
    if(status!=SUCCESS){
        return status;
    }

    bitmap_rwlock.write_lock();

    uint64_t end_bit = bit_idx + bit_count;
    uint64_t start_align64 = (bit_idx + 63) & ~63ULL;   // 下一个64位边界
    uint64_t end_align64 = end_bit & ~63ULL;            // 末尾对齐边界

    // ----------- 1. 处理前导未对齐部分 -----------
    if (bit_idx < start_align64 && bit_idx < end_bit)
    {
        uint64_t pre_bits = (start_align64 > end_bit ? end_bit : start_align64) - bit_idx;
        bits_set(bit_idx, pre_bits, value);
        bit_idx += pre_bits;
    }

    // ----------- 2. 处理中间完整的u64块 -----------
    if (bit_idx < end_align64)
    {
        uint64_t u64_start_idx = bit_idx >> 6;               // /64
        uint64_t u64_count = (end_align64 - bit_idx) >> 6;   // /64
        u64s_set(u64_start_idx, u64_count, value);
        bit_idx += u64_count << 6;
    }

    // ----------- 3. 处理尾部未对齐部分 -----------
    if (bit_idx < end_bit)
    {
        bits_set(bit_idx, end_bit - bit_idx, value);
    }

    bitmap_rwlock.write_unlock();
    return SUCCESS;
}

int kpoolmemmgr_t::HCB_v2::first_linekd_heap_Init(loaded_VM_interval *first_static_heap, loaded_VM_interval *first_static_heap_bitmap)
{
    bitmap_controller.Init(first_static_heap_bitmap);
    vbase=first_static_heap->vbase;
    phybase=first_static_heap->pbase;
    total_size_in_bytes=first_static_heap->size;
    return OS_SUCCESS;
}


KURD_t kpoolmemmgr_t::HCB_v2::second_stage_Init()
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INIT;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INIT;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INIT;
    if(this==&kpoolmemmgr_t::first_linekd_heap){
        fail.module_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INIT_RESULTS::FAIL_RESONS::REASON_CODE_first_linekd_heap_NOT_ALLOWED;
        return fail;
    }
    buddy_alloc_params param=BUDDY_ALLOC_DEFAULT_FLAG;
    param.align_log2=21;
    KURD_t contain;
    
    this->phybase=FreePagesAllocator::alloc(total_size_in_bytes,param,page_state_t::kernel_pinned,contain);
    if(!success_all_kurd(contain))return contain;
    #ifdef KERNEL_MODE
    vm_interval interval={
        .vbase=vbase,
        .pbase=phybase,
        .size=total_size_in_bytes,
        .access=KspacePageTable::PG_RW,
    };
    contain=KspacePageTable::enable_VMentry(interval);
    if(!success_all_kurd(contain))return contain;
    #endif
    
    #ifdef USER_MODE
    
    vbase=(vaddr_t)malloc(total_size_in_bytes);
    if(vbase==NULL)return OS_OUT_OF_MEMORY;
    phybase=vbase;
    #endif
    KURD_t status=bitmap_controller.second_stage_Init(
        total_size_in_bytes/bytes_per_bit
    );
    return status;
}
kpoolmemmgr_t::HCB_v2::~HCB_v2()
{
    bitmap_controller.~HCB_bitmap();
    KURD_t status=KURD_t();
    #ifdef KERNEL_MODE
    vm_interval interval={
        .vbase=vbase,
        .pbase=phybase,
        .size=total_size_in_bytes,
        .access=KspacePageTable::PG_RW,
    };
    status=KspacePageTable::disable_VMentry(interval);
    if(!success_all_kurd(status))goto free_wrong;
    status=FreePagesAllocator::free(
        phybase,
        total_size_in_bytes
    );
    if(!success_all_kurd(status))goto free_wrong;
    #endif
    #ifdef USER_MODE
    std::free((void*)vbase);
    return;
    #endif
free_wrong:    
    panic_info_inshort inshort={
        .is_bug=true,
        .is_policy=false,
        .is_hw_fault=false,
        .is_mem_corruption=false,
        .is_escalated=false,  
    };
    if(status.result!=result_code::SUCCESS){
        Panic::panic(default_panic_behaviors_flags,"kpoolmemmgr_t::HCB_v2::~HCB_v2 cancel memmap failed",nullptr,&inshort,status);
    }
    status=FreePagesAllocator::free(phybase,total_size_in_bytes);
    if(status.result!=result_code::SUCCESS){
        Panic::panic(default_panic_behaviors_flags,"kpoolmemmgr_t::HCB_v2::~HCB_v2 recycle phy pages failed",nullptr,&inshort,status);
    }
}
KURD_t kpoolmemmgr_t::HCB_v2::clear(void *ptr)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR;
    if (!is_addr_belong_to_this_hcb(ptr)){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::CLEAR_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;
    }

    // ptr 是用户可见地址，meta 在其前面
    data_meta *meta = (data_meta *)((uint8_t *)ptr - sizeof(data_meta));
    if (meta->magic != MAGIC_ALLOCATED)
    {
        fatal.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::CLEAR_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }

    // 清零用户数据区域（不清 meta）
    // setmem 的原型假设为 setmem(void* addr, size_t size, int value)
    ksetmem_8(ptr, 0, meta->data_size);

    return success;
}
KURD_t kpoolmemmgr_t::HCB_v2::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB,0,0,err_domain::CORE_MODULE);
}

KURD_t kpoolmemmgr_t::HCB_v2::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}

KURD_t kpoolmemmgr_t::HCB_v2::default_fail()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}

KURD_t kpoolmemmgr_t::HCB_v2::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}

/**
 * @brief 在堆中分配内存
 * 
 * 根据请求的大小和对齐要求，在堆中分配一块内存区域。
 * 分配策略根据大小分为三种：小块(4位对齐)、中块(7位对齐)和大块(10位对齐)
 * 
 * @param addr 返回分配的内存地址
 * @param size 请求分配的内存大小(字节)
 * @param is_crucial 是否为关键变量，当内存损坏时决定是否触发内核恐慌
 * @param vaddraquire true返回虚拟地址，false返回物理地址
 * @param alignment 对齐要求，实际对齐值为2^alignment字节对齐，最大支持10(1024字节对齐)
 * @return int OS_SUCCESS表示成功分配，其他值表示分配失败
 */
KURD_t kpoolmemmgr_t::HCB_v2::in_heap_alloc(
    void *&addr,
    uint32_t size,
    alloc_flags_t flags)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC;
    // -----------------------------
    // 基本参数与边界检查
    // -----------------------------
    auto record_alloc_fail = [&]() {
        statistics.alloc_fail_count++;
    };
    if (flags.align_log2 >=32)
        {
            record_alloc_fail();
            fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_TOO_HIGH_ALIGN_DEMAND;
            return fail;
        }
    if (size == 0)
        {
            record_alloc_fail();
            fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SIZE_DEMAND_IS_ZERO;
            return fail;
        }
    // 常量定义（在内核空间必须为常量或宏）
    static constexpr uint32_t SMALL_UNIT_BYTES=16;
    static constexpr uint32_t MID_UNIT_BYTES = SMALL_UNIT_BYTES<<3;
    static constexpr uint32_t LARGE_UNIT_BYTES = MID_UNIT_BYTES<<3;
    // -----------------------------
    // 计算对齐方式
    // -----------------------------
    uint8_t size_alignment = 0;
    uint8_t aquire_alignment = 0;
    uint8_t final_alignment_aquire;

    // 根据请求大小确定对齐档次
    if (size < 2 * 8 * bytes_per_bit)
        size_alignment = 4;    // 16B 对齐
    else if (size < 2 * 64 * bytes_per_bit)
        size_alignment = 7;    // 128B 对齐
    else
        size_alignment = 10;   // 1024B 对齐

    // 根据 alignment 参数映射档次
    if (flags.align_log2 <= 4)
        aquire_alignment = 4;
    else if (flags.align_log2 <= 7)
        aquire_alignment = 7;
    else if (flags.align_log2 <= 10)
        aquire_alignment = 10;
    else aquire_alignment = flags.align_log2;
    // 最终取较大者
    final_alignment_aquire = (size_alignment > aquire_alignment) ?
                             size_alignment : aquire_alignment;

    // -----------------------------
    // 分配逻辑（不同对齐档）
    // -----------------------------
    int status = OS_SUCCESS;
    uintptr_t base_addr = (uintptr_t)(flags.vaddraquire ? vbase : phybase);
    auto update_cache_on_hint_hit = [&](uint64_t base_u64_idx, uint64_t need_u64s) {
        auto& cache = bitmap_controller.scan_cache;
        if (cache.largest_free_hint_len >= need_u64s &&
            cache.largest_free_hint_base == base_u64_idx) {
            cache.last_success_idx = base_u64_idx;
            uint64_t next = base_u64_idx + need_u64s;
            cache.hint_u64_idx = next < bitmap_controller.bitmap_size_in_64bit_units
                ? next
                : bitmap_controller.invalid_cache;
            cache.largest_free_hint_len = 0;
            cache.largest_free_hint_base = bitmap_controller.invalid_cache;
        }
    };

    switch (final_alignment_aquire)
    {
    case 4: {  // 小块分配 (16字节对齐)
        uint32_t serial_bits_count = (size + SMALL_UNIT_BYTES - 1 + sizeof(data_meta)) >> 4;
        uint64_t base_bit_idx = 0;
        bitmap_controller.bitmap_rwlock.read_lock();
        status = bitmap_controller.continual_avaliable_bits_search(serial_bits_count, base_bit_idx);
        bitmap_controller.bitmap_rwlock.read_unlock();
        if (status != OS_SUCCESS)
            {
                record_alloc_fail();
                fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }
        update_cache_on_hint_hit(base_bit_idx >> 6, (serial_bits_count + 63) >> 6);
        bitmap_controller.bits_seg_set(base_bit_idx, serial_bits_count,true);
        bitmap_controller.used_bit_count_lock.lock();
        bitmap_controller.bitmap_used_bit+=serial_bits_count;
        uintptr_t offset = bytes_per_bit * (base_bit_idx + 1);
        uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
        update_peak_used_bytes(statistics, used_bytes);
        bitmap_controller.used_bit_count_lock.unlock();
        addr = (void *)(base_addr + offset);
        break;
    }
    case 7: {  // 中块分配 (128字节对齐)
        uint32_t serial_bytes_count = ((size + MID_UNIT_BYTES - 1) >> 7) + 1;
        uint64_t base_byte_idx = 0;
        bitmap_controller.bitmap_rwlock.read_lock();
        status = bitmap_controller.continual_avaliable_bytes_search(serial_bytes_count, base_byte_idx);
        bitmap_controller.bitmap_rwlock.read_unlock();
        if (status != OS_SUCCESS)
            {
                record_alloc_fail();
                fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }
        update_cache_on_hint_hit(base_byte_idx >> 3, (serial_bytes_count + 7) >> 3);

        bitmap_controller.bit_set(7 + (base_byte_idx << 3) , true);
        bitmap_controller.used_bit_count_lock.lock();
        bitmap_controller.bitmap_used_bit++;
        uint32_t total_bitcount = (size + SMALL_UNIT_BYTES - 1) >> 4;
        bitmap_controller.bits_seg_set((base_byte_idx+1)*8, total_bitcount,true);
        bitmap_controller.bitmap_used_bit+=total_bitcount;
        uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
        update_peak_used_bytes(statistics, used_bytes);
        bitmap_controller.used_bit_count_lock.unlock();
        uintptr_t offset = bytes_per_bit * ((base_byte_idx + 1) << 3);
        addr = (void *)(base_addr + offset);
        break;
    }
    case 10: {  // 大块分配 (1024字节对齐)
        uint32_t serial_u64_count = ((size + LARGE_UNIT_BYTES - 1) >> 10) + 1;
        uint64_t base_u64_idx = 0;
        bitmap_controller.bitmap_rwlock.read_lock();
        status = bitmap_controller.continual_avaliable_u64s_search(serial_u64_count, base_u64_idx);
        bitmap_controller.bitmap_rwlock.read_unlock();
        if (status != OS_SUCCESS)
            {
                record_alloc_fail();
                fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }
        update_cache_on_hint_hit(base_u64_idx, serial_u64_count);

        bitmap_controller.bit_set( 63 + (base_u64_idx << 6), true);
        bitmap_controller.used_bit_count_lock.lock();
        bitmap_controller.bitmap_used_bit++;
        uint32_t total_bitcount = (size + SMALL_UNIT_BYTES - 1) >> 4;
        bitmap_controller.bits_seg_set((base_u64_idx+1)*64, total_bitcount,true);
        bitmap_controller.bitmap_used_bit += total_bitcount;
        uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
        update_peak_used_bytes(statistics, used_bytes);
        bitmap_controller.used_bit_count_lock.unlock();
        uintptr_t offset = bytes_per_bit * ((base_u64_idx + 1) << 6);
        addr = (void *)(base_addr + offset);
        break;
    }
    default:
    {
    if (final_alignment_aquire <= 10)
        {
            record_alloc_fail();
            fatal.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FATAL_REASONS::REASON_CODE_ALIGN_DEMAND_INVALID;
            return fatal;
        }

    // ------------------------------------
    // 对齐参数（u64 粒度）
    // ------------------------------------
    const uint8_t u64_align_log2 = final_alignment_aquire - 10;
    const uint64_t u64_align = 1ULL << u64_align_log2;

    // ------------------------------------
    // 真实 payload 需要的 u64 数量
    // ------------------------------------
    const uint32_t payload_u64_count =
        (align_up(size, LARGE_UNIT_BYTES)) >> 10;

    // 最坏情况下需要的搜索长度（payload + padding）
    const uint32_t search_u64_count =
        align_up(payload_u64_count, u64_align)+ u64_align;

    uint64_t base_u64_idx = 0;

    // ------------------------------------
    // 搜索（只保证：有一个对齐起点能放下 payload）
    // ------------------------------------
    bitmap_controller.bitmap_rwlock.read_lock();
    status = bitmap_controller.continual_avaliable_u64s_search_higher_alignment(
        u64_align_log2,
        search_u64_count,
        base_u64_idx
    );
    bitmap_controller.bitmap_rwlock.read_unlock();

    if (status != OS_SUCCESS)
        {
                record_alloc_fail();
                fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }
    update_cache_on_hint_hit(base_u64_idx, payload_u64_count);

    // ------------------------------------
    // 计算真正的对齐起点
    // ------------------------------------
    uint64_t real_base_u64 =
        base_u64_idx+u64_align ;

    uint64_t real_base_bit = real_base_u64 << 6;

    // ------------------------------------
    // 标记 bitmap
    // sentinel: real_base_bit - 1
    // ------------------------------------
    bitmap_controller.bit_set(real_base_bit - 1, true);

    bitmap_controller.used_bit_count_lock.lock();
    bitmap_controller.bitmap_used_bit++;

    const uint32_t total_bitcount =
        (size + SMALL_UNIT_BYTES - 1) >> 4;

    bitmap_controller.bits_seg_set(
        real_base_bit,
        total_bitcount,
        true
    );

    bitmap_controller.bitmap_used_bit += total_bitcount;
    uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
    update_peak_used_bytes(statistics, used_bytes);
    bitmap_controller.used_bit_count_lock.unlock();

    // ------------------------------------
    // 返回地址（与 real_base_u64 严格一致）
    // ------------------------------------
    uintptr_t offset = bytes_per_bit * real_base_bit;
    addr = (void *)(base_addr + offset);

    break;
    }

    }

    // -----------------------------
    // 设置元数据（data_meta）
    // -----------------------------
    data_meta *meta = (data_meta *)((uint8_t *)addr - sizeof(data_meta));
    meta->magic = MAGIC_ALLOCATED;
    meta->data_size = size;
    meta->alloc_flags= flags;
    statistics.alloc_count++;
    statistics.alloc[size_bucket_index(size)]++;
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v2::free(void *ptr)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE;
    if(uint64_t(ptr)&15||!is_addr_belong_to_this_hcb(ptr)){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }
    uint32_t in_heap_offset;
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)ptr<MIN_KVADDR)
    {//物理地址
        if((uint64_t)ptr<phybase+sizeof(data_meta)||
    phybase+total_size_in_bytes<=(uint64_t)ptr
    ){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }
    in_heap_offset=(uint64_t)ptr-(uint64_t)phybase;
    }else{
        if((uint64_t)ptr<vbase+sizeof(data_meta)||
    vbase+total_size_in_bytes<=(uint64_t)ptr){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }
    in_heap_offset=(uint64_t)ptr-(uint64_t)vbase;
    }
    data_meta *meta = (data_meta *)((uint8_t *)ptr - sizeof(data_meta));
    if(meta->magic != MAGIC_ALLOCATED)
    {
        fatal.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }
    statistics.free_count++;
    statistics.free[size_bucket_index(meta->data_size)]++;
    uint32_t total_bits_count=(meta->data_size+15)/16;
    uint32_t base_bit_idx=in_heap_offset/16;
    bitmap_controller.bitmap_rwlock.write_lock();
    bitmap_controller.bit_set(base_bit_idx-1,false);
    bitmap_controller.bitmap_rwlock.write_unlock();
    bitmap_controller.used_bit_count_lock.lock();
    bitmap_controller.bitmap_used_bit--;
    bitmap_controller.bitmap_used_bit-=total_bits_count;
    bitmap_controller.used_bit_count_lock.unlock();
    bitmap_controller.bits_seg_set(
        base_bit_idx,
        total_bits_count,
        false
    );
    return success;
}

KURD_t kpoolmemmgr_t::HCB_v2::in_heap_realloc(
    void *&ptr, 
    uint32_t new_size,
    alloc_flags_t flags)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC;
    auto record_realloc_fail = [&]() {
        statistics.realloc_fail_count++;
    };
    if(uint64_t(ptr)&15||!is_addr_belong_to_this_hcb(ptr)){
        record_realloc_fail();
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }  //本堆的内存至少16字节对齐
    uint32_t in_heap_offset;
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)ptr<MIN_KVADDR)
    {//物理地址
        if((uint64_t)ptr<phybase+sizeof(data_meta)||
    phybase+total_size_in_bytes<=(uint64_t)ptr
    ){
        record_realloc_fail();
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    } 
    in_heap_offset=(uint64_t)ptr-(uint64_t)phybase;
    }else{
        if((uint64_t)ptr<vbase+sizeof(data_meta)||
    vbase+total_size_in_bytes<=(uint64_t)ptr){
        record_realloc_fail();
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    } 
    in_heap_offset=(uint64_t)ptr-(uint64_t)vbase;
    }
    data_meta *meta = (data_meta *)((uint8_t *)ptr - sizeof(data_meta));
    uint16_t old_size=meta->data_size;
    if(meta->magic != MAGIC_ALLOCATED)
    {
        record_realloc_fail();
        fatal.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }uint32_t old_bits_count=(meta->data_size+15)/16;
    uint32_t base_bit_idx=in_heap_offset/16;
    uint32_t new_bits_count=(new_size+15)/16;
    if(!flags.is_when_realloc_force_new_addr)
    {
    if(new_bits_count<=old_bits_count)
    {
        meta->data_size=new_size;
        bitmap_controller.bits_seg_set(
            base_bit_idx+new_bits_count,
            old_bits_count-new_bits_count,
            false
        );
        bitmap_controller.used_bit_count_lock.lock();
        bitmap_controller.bitmap_used_bit-=(old_bits_count-new_bits_count);
        uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
        update_peak_used_bytes(statistics, used_bytes);
        bitmap_controller.used_bit_count_lock.unlock();

        statistics.realloc_count++;
        return success;
    }else{
        HCB_bitmap_error_code_t err;
        bool try_append=bitmap_controller.target_bit_seg_is_avaliable(
            base_bit_idx+old_bits_count,
            new_bits_count-old_bits_count,
            err
        );
        if(err!=SUCCESS)try_append=false;
        if(try_append)
        {
             bitmap_controller.bits_seg_set(
                base_bit_idx+old_bits_count,
                new_bits_count-old_bits_count,
                true
            );
            bitmap_controller.used_bit_count_lock.lock();
            bitmap_controller.bitmap_used_bit+=(new_bits_count-old_bits_count);
            uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
            update_peak_used_bytes(statistics, used_bytes);
            bitmap_controller.used_bit_count_lock.unlock();
            statistics.realloc_count++;
            return success;
        }else{
            void*new_ptr;
            KURD_t status=in_heap_alloc(
                new_ptr,new_size,flags
            );
            if(status.result==result_code::SUCCESS){
                ksystemramcpy(ptr,new_ptr,old_size);
                status=free(ptr);
                ptr=new_ptr;
                bitmap_controller.used_bit_count_lock.lock();
                bitmap_controller.bitmap_used_bit+=(new_bits_count-old_bits_count);
                uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
                update_peak_used_bytes(statistics, used_bytes);
                bitmap_controller.used_bit_count_lock.unlock();
                statistics.realloc_count++;
                return success;
            }else{
                record_realloc_fail();
                return status;
            }
        }
    }
    }else{
        void*new_ptr;
            KURD_t status=in_heap_alloc(
                new_ptr,new_size,flags
            );
            if(status.result==result_code::SUCCESS){
                ksystemramcpy(ptr,new_ptr,old_size);
                status=free(ptr);
                ptr=new_ptr;
                bitmap_controller.used_bit_count_lock.lock();
                bitmap_controller.bitmap_used_bit+=(new_bits_count-old_bits_count);
                uint64_t used_bytes = bitmap_controller.bitmap_used_bit * bytes_per_bit;
                update_peak_used_bytes(statistics, used_bytes);
                bitmap_controller.used_bit_count_lock.unlock();
                statistics.realloc_count++;
                return success;
            }else{
                record_realloc_fail();
                return status;
            }
    }fatal.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_UNREACHABLE_CODE;
    record_realloc_fail();
    return fatal;
}

uint64_t kpoolmemmgr_t::HCB_v2::get_used_bytes_count()
{
    return this->bitmap_controller.bitmap_used_bit*this->bytes_per_bit;
}

bool kpoolmemmgr_t::HCB_v2::is_full()
{
    bitmap_controller.used_bit_count_lock.lock();
    uint64_t used_bit_count=bitmap_controller.bitmap_used_bit;
    bitmap_controller.used_bit_count_lock.unlock();
    return used_bit_count==bitmap_controller.bitmap_size_in_64bit_units*64 ;
}
void kpoolmemmgr_t::HCB_v2::count_used_bytes()
{
    bitmap_controller.count_bitmap_used_bit();
}

bool kpoolmemmgr_t::HCB_v2::is_addr_belong_to_this_hcb(void *addr)
{
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)addr<MAX_PHYADDR)
    {//物理地址
        if((uint64_t)addr<phybase+sizeof(data_meta)||
    phybase+total_size_in_bytes<=(uint64_t)addr
    )return false;

    }else{
        if(MAX_PHYADDR<=(uint64_t)addr&&(uint64_t)addr<MIN_KVADDR)return false;
        if((uint64_t)addr<vbase+sizeof(data_meta)||
    vbase+total_size_in_bytes<=(uint64_t)addr)return false;

    }
    return true;
}
phyaddr_t kpoolmemmgr_t::HCB_v2::tran_to_phy(void *addr)
{
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)addr<MAX_PHYADDR)return (phyaddr_t)addr;
    if(is_addr_belong_to_this_hcb(addr)){
        return (phyaddr_t)((uint64_t)addr-vbase+phybase);
    }
    return 0;
}
vaddr_t kpoolmemmgr_t::HCB_v2::tran_to_virt(phyaddr_t addr)
{
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if(MIN_KVADDR<=addr)return (vaddr_t)addr;
    if(is_addr_belong_to_this_hcb((void*)addr)){
        return (vaddr_t)((uint64_t)addr-phybase+vbase);
    }
    return 0;
}
