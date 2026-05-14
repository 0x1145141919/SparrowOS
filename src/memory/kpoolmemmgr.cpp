#include "memory/memory_base.h"
#include "util/OS_utils.h"
#include "abi/os_error_definitions.h"
#include "util/kout.h"
#include "panic.h"
#include "memory/kpoolmemmgr.h"
#include "memory/AddresSpace.h"
#include "memory/all_pages_arr.h"
#include "firmware/ACPI_APIC.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/kptrace.h"
#include "util/OS_utils.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include <cstring>

// ════════════════════════════════════════════════════════════════
// kpoolmemmgr_t — HCB_v3 全局堆管理器
//
// first_linekd_heap 在 BSS 中, 内核入口尽早 online()
// (mmu_init 阶段调用 first_linekd_heap.online()).
// multi_heap_enable() 后启用 per-processor 堆.
// ════════════════════════════════════════════════════════════════

// 静态成员定义
bool    kpoolmemmgr_t::is_muli_heap_enabled = false;
kpoolmemmgr_t::HCB_v3 kpoolmemmgr_t::first_linekd_heap;
kpoolmemmgr_t::HCB_v3* kpoolmemmgr_t::HCB_ARRAY = nullptr;
spinrwlock_cpp_t kpoolmemmgr_t::HCB_ARRAY_lock;
VM_DESC kpoolmemmgr_t::heap_area_bitmaps = { .end=0, .map_type=VM_DESC::MAP_NONE };
VM_DESC kpoolmemmgr_t::heap_area = { .start=0, .end=0, .map_type=VM_DESC::MAP_NONE };

kpoolmemmgr_t::HCB_v3* kpoolmemmgr_t::find_hcb_by_address(void* ptr)
{
    if (first_linekd_heap.is_addr_belong(ptr))
        return &first_linekd_heap;
    if (!HCB_ARRAY) return nullptr;
    uint64_t uptr = (uint64_t)ptr;
    if (uptr < heap_area.start || uptr >= heap_area.end)
        return nullptr;
    uint64_t idx = (uptr - heap_area.start) / HCB_DEFAULT_SIZE;
    HCB_ARRAY_lock.read_lock();
    HCB_v3* hcb = &HCB_ARRAY[idx];
    HCB_ARRAY_lock.read_unlock();
    if (!hcb->valid) return nullptr;
    return hcb;
}

KURD_t kpoolmemmgr_t::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR,0,0,err_domain::CORE_MODULE);
}
KURD_t kpoolmemmgr_t::default_success() { KURD_t k=default_kurd(); k.result=result_code::SUCCESS; k.level=level_code::INFO; return k; }
KURD_t kpoolmemmgr_t::default_fail()   { KURD_t k=default_kurd(); k=set_result_fail_and_error_level(k); return k; }
KURD_t kpoolmemmgr_t::default_fatal()  { KURD_t k=default_kurd(); k=set_fatal_result_level(k); return k; }

void kpoolmemmgr_t::Init()
{
    first_linekd_heap.linktime_init();
}

// ── multi_heap_enable ──
// 逻辑同 HCB_v2, 但 HCB_ARRAY 为 HCB_v3 对象数组 (不落 first_linekd_heap)
KURD_t kpoolmemmgr_t::multi_heap_enable()
{
    KURD_t success = default_success();
    KURD_t fail    = default_fail();
    success.event_code = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    fail.event_code    = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;

    if (is_muli_heap_enabled) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_ALREADY_ENABLED;
        return fail;
    }
    uint64_t processor_count = logical_processor_count;
    if (processor_count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_BAD_PROCESSOR_COUNT;
        return fail;
    }
    uint64_t hcb_count = processor_count * (1 << PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);

    HCB_ARRAY_lock.write_lock();
    KURD_t kurd;
    HCB_ARRAY = (HCB_v3*)__wrapped_pgs_valloc(&kurd,
        alignup_and_shift_right(sizeof(HCB_v3) * hcb_count, 12),
        page_state_t::kernel_pinned, 12);
    ksetmem_8(HCB_ARRAY, 0, hcb_count * sizeof(HCB_v3));
    HCB_ARRAY_lock.write_unlock();

    uint64_t heap_area_size = HCB_DEFAULT_SIZE * hcb_count;
    heap_area.start = kspace_vm_table->alloc_available_space(heap_area_size, 0);
    if (heap_area.start == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_NO_VADDR_SPACE;
        return fail;
    }
    heap_area.end = heap_area.start + heap_area_size;
    heap_area.is_vaddr_alloced = 1;
    if (kspace_vm_table->insert(heap_area) != OS_SUCCESS) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_VM_ADD_FAIL;
        return fail;
    }

    uint64_t bitmap_area_size = heap_area_size / 128;
    heap_area_bitmaps.start = kspace_vm_table->alloc_available_space(bitmap_area_size, 0);
    if (heap_area_bitmaps.start == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_NO_VADDR_SPACE;
        return fail;
    }
    heap_area_bitmaps.end = heap_area_bitmaps.start + bitmap_area_size;
    heap_area_bitmaps.is_vaddr_alloced = 1;
    if (kspace_vm_table->insert(heap_area_bitmaps) != OS_SUCCESS) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_VM_ADD_FAIL;
        return fail;
    }

    is_muli_heap_enabled = true;
    return success;
}

KURD_t kpoolmemmgr_t::alloc_heap(uint32_t idx)
{
    KURD_t success = default_success();
    KURD_t fail    = default_fail();
    success.event_code = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    fail.event_code    = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    uint64_t processor_count = logical_processor_count;
    uint64_t hcb_count = processor_count * (1 << PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);

    if (idx >= hcb_count) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_IDX_OUT_OF_RANGE;
        return fail;
    }
    if (HCB_ARRAY[idx].valid) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_HEAP_ALREADY_EXISTS;
        return fail;
    }

    vaddr_t data_va   = heap_area.start + HCB_DEFAULT_SIZE * idx;
    vaddr_t bitmap_va = heap_area_bitmaps.start + (HCB_DEFAULT_SIZE / 128) * idx;
    return HCB_ARRAY[idx].online(HCB_DEFAULT_SIZE, data_va, bitmap_va);
}

KURD_t kpoolmemmgr_t::free_heap(uint32_t idx)
{
    KURD_t success = default_success();
    KURD_t fail    = default_fail();
    success.event_code = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    fail.event_code    = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    uint64_t processor_count = logical_processor_count;
    uint64_t hcb_count = processor_count * (1 << PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);
    if (idx >= hcb_count) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_IDX_OUT_OF_RANGE;
        return fail;
    }
    if (!HCB_ARRAY[idx].valid) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_HEAP_NOT_EXIST;
        return fail;
    }
    return HCB_ARRAY[idx].offline();
}

// ── kalloc ──
void* kpoolmemmgr_t::kalloc(uint64_t size, KURD_t& no_succes_report, alloc_flags_t flags)
{
    KURD_t success = default_success();
    KURD_t fail    = default_fail();
    success.event_code = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_ALLOC;
    fail.event_code    = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_ALLOC;

    if (size == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SIZE_IS_ZERO;
        no_succes_report = fail;
        return nullptr;
    }

    void* ptr = nullptr;
    KURD_t contain;

    if (!flags.force_first_linekd_heap && is_muli_heap_enabled && HCB_ARRAY) {
        uint32_t id = fast_get_processor_id();
        uint64_t hcb_count = logical_processor_count * (1 << PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);
        uint32_t hotspot_start = id << PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2;
        uint32_t hotspot_end   = hotspot_start + (1 << PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);

        // Phase 1: 本 CPU 热点堆（优先，cache 亲和）
        for (uint32_t i = hotspot_start; i < hotspot_end; ++i) {
            HCB_v3* hcb = &HCB_ARRAY[i];
            { spintrylock_try_guard _g(&hcb->hcb_lock);
            if (!_g.is_locked()) continue;

            if (!hcb->valid) {
                contain = alloc_heap(i);
                if (!success_all_kurd(contain)) continue;  // online 失败跳过
            }
            contain = hcb->alloc(ptr, size, flags);
            if (success_all_kurd(contain)) {
                no_succes_report = contain;
                return ptr;
            }
            }
        }

        // Phase 2: 全局扫描（跳过已试过热点堆）
        for (uint32_t i = 0; i < hcb_count; ++i) {
            if (i >= hotspot_start && i < hotspot_end) continue;
            HCB_v3* hcb = &HCB_ARRAY[i];
            { spintrylock_try_guard _g(&hcb->hcb_lock);
            if (!_g.is_locked()) continue;

            if (!hcb->valid) {
                contain = alloc_heap(i);
                if (!success_all_kurd(contain)) continue;
            }
            contain = hcb->alloc(ptr, size, flags);
            if (success_all_kurd(contain)) {
                no_succes_report = contain;
                return ptr;
            }
            }
        }
    }

    // Phase 3: first_linekd_heap fallback（自旋等待）
    { spintrylock_spin_guard _g(first_linekd_heap.hcb_lock);
    contain = first_linekd_heap.alloc(ptr, size, flags);
    if (success_all_kurd(contain)) {
        no_succes_report = contain;
        return ptr;
    }
    }

    fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_MEM;
    no_succes_report = fail;
    return nullptr;
}

// ── realloc ──
void* kpoolmemmgr_t::realloc(void* ptr, KURD_t& no_succes_report, uint64_t size, alloc_flags_t flags)
{
    if (!ptr) return kalloc(size, no_succes_report, flags);
    if (size == 0) {
        kfree(ptr);
        no_succes_report = default_success();
        return nullptr;
    }

    HCB_v3* hcb = find_hcb_by_address(ptr);
    if (!hcb) {
        KURD_t fail = default_fail();
        fail.event_code = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_REALLOC;
        fail.reason = MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::REALLOC_RESULTS::FAIL_RESONS::REASON_CODE_PTR_NOT_IN_ANY_HEAP;
        no_succes_report = fail;
        return nullptr;
    }
    KURD_t contain;
    {// 锁定目标 HCB（自旋等待）
        spintrylock_spin_guard _g(hcb->hcb_lock);
        contain = hcb->realloc(ptr, size, flags);
    }
    if (!success_all_kurd(contain)) {
        // fallback: alloc new + copy + free old
        void* new_ptr = kalloc(size, no_succes_report, flags);
        if (new_ptr) {
            buddy_meta* meta = (buddy_meta*)((uint8_t*)ptr - sizeof(buddy_meta));
            ksystemramcpy(ptr, new_ptr, meta->data_size < size ? meta->data_size : size);
            kfree(ptr);
            return new_ptr;
        }
        no_succes_report = contain;
        return nullptr;
    }
    no_succes_report = contain;
    return ptr;
}

// ── clear ──
void kpoolmemmgr_t::clear(void* ptr)
{
    if (!ptr) return;
    HCB_v3* hcb = find_hcb_by_address(ptr);
    if (!hcb) return;
    spintrylock_spin_guard _g(hcb->hcb_lock);
    hcb->clear(ptr);
}

// ── kfree ──
void kpoolmemmgr_t::kfree(void* ptr)
{
    if (!ptr) return;
    HCB_v3* hcb = find_hcb_by_address(ptr);
    if (!hcb) return;

    // 锁定目标 HCB（自旋等待）
    spintrylock_spin_guard _g(hcb->hcb_lock);

    KURD_t contain = hcb->free(ptr);
    if (contain.result == result_code::FATAL && contain.level == level_code::FATAL) {
        bsp_kout << "kpoolmemmgr_t::kfree: METADATA_DESTROYED at " << ptr << kendl;
        panic_info_inshort inshort = { .is_bug=false, .is_policy=true, .is_hw_fault=false, .is_mem_corruption=true, .is_escalated=false };
        Panic::panic(default_panic_behaviors_flags, "kfree: metadata corrupted", nullptr, &inshort, contain);
    }
}
