#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/AddresSpace.h"
#include "memory/FreePagesAllocator.h"
#include "panic.h"
#include "util/OS_utils.h"
#include "ktime.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "util/arch/x86-64/cpuid_intel.h"

// 远端 TLB 失效函数，实现在 KspacMapMgr_pagediretct_operate.cpp
extern "C" uint64_t remote_invalidate_seg(void* ptr);
#ifdef USER_MODE
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif
// ── 模块错误树：LOCATION_CODE_OUT_SURFACES ──────────────────────
namespace MEMMODULE_LOCATIONS{
    namespace OUT_SURFACES_EVENTS{

        constexpr uint8_t EVENT_CODE_PAGES_VALLOC             = 0;
        constexpr uint8_t EVENT_CODE_PAGES_VFREE              = 1;
        constexpr uint8_t EVENT_CODE_PAGES_ALLOC              = 2;
        // 3,4,5 reserved (unused)
        constexpr uint8_t EVENT_CODE_DIRECT_MAP_PADDR         = 6;
        constexpr uint8_t EVENT_CODE_DIRECT_UNMAP_PADDR       = 7;
        constexpr uint8_t EVENT_CODE_PINTERVAL_ALLOC_AND_MAP  = 8;
        constexpr uint8_t EVENT_CODE_BCAST_INVALIDATE_TLB     = 9;

        // COMMON_FAIL_REASONS [0x00, 0x100)
        namespace COMMON_FAIL_REASONS {
            constexpr uint16_t BAD_INTERVAL           = 0x00;
            constexpr uint16_t BAD_PARAM              = 0x01;
            constexpr uint16_t VADDR_ALLOC_FAIL       = 0x02;
            constexpr uint16_t INSERT_VM_DESC_FAIL    = 0x03;
            constexpr uint16_t REMOVE_VM_DESC_FAIL    = 0x04;
            constexpr uint16_t PHYS_ALLOC_FAIL        = 0x05;
        }

        // COMMON_FATAL_REASONS [0x00, 0x100)
        namespace COMMON_FATAL_REASONS {
            constexpr uint16_t TLB_SHOOTDOWN_TIMEOUT  = 0x00;
        }

        // 结果容器（私有原因 ≥ 0x100, 当前不需）
        namespace direct_map_paddr_results {}
        namespace direct_unmap_paddr_results {}
        namespace pinterval_alloc_and_map_results {}
        namespace bcast_invalidate_tlb_results {}
    }
}

// ── 位置级 KURD 模板函数 ────────────────────────────────────────
namespace {
static KURD_t out_surfaces_default_kurd()
{
    return KURD_t(0, 0, module_code::MEMORY,
                  MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
                  0, 0, err_domain::CORE_MODULE);
}

static KURD_t out_surfaces_default_success()
{
    KURD_t k = out_surfaces_default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    return k;
}

static KURD_t out_surfaces_default_failure()
{
    KURD_t k = out_surfaces_default_kurd();
    k = set_result_fail_and_error_level(k);
    return k;
}

static KURD_t out_surfaces_default_fatal()
{
    KURD_t k = out_surfaces_default_kurd();
    k = set_fatal_result_level(k);
    return k;
}
}
#ifdef USER_MODE
namespace {
int g_userspace_devmem_fd = -1;
struct userspace_direct_map_record_t {
    void* user_vbase;
    void* mmap_base;
    uint64_t mmap_len;
};
static constexpr uint32_t USERSPACE_DIRECT_MAP_MAX = 1024;
userspace_direct_map_record_t g_userspace_direct_maps[USERSPACE_DIRECT_MAP_MAX] = {};

int userspace_direct_map_record_add(void* user_vbase, void* mmap_base, uint64_t mmap_len)
{
    for (uint32_t i = 0; i < USERSPACE_DIRECT_MAP_MAX; ++i) {
        if (g_userspace_direct_maps[i].user_vbase == nullptr) {
            g_userspace_direct_maps[i].user_vbase = user_vbase;
            g_userspace_direct_maps[i].mmap_base = mmap_base;
            g_userspace_direct_maps[i].mmap_len = mmap_len;
            return OS_SUCCESS;
        }
    }
    return OS_OUT_OF_RESOURCE;
}

userspace_direct_map_record_t* userspace_direct_map_record_find(void* user_vbase)
{
    for (uint32_t i = 0; i < USERSPACE_DIRECT_MAP_MAX; ++i) {
        if (g_userspace_direct_maps[i].user_vbase == user_vbase) {
            return &g_userspace_direct_maps[i];
        }
    }
    return nullptr;
}

void userspace_direct_map_record_clear(userspace_direct_map_record_t* rec)
{
    if (!rec) return;
    rec->user_vbase = nullptr;
    rec->mmap_base = nullptr;
    rec->mmap_len = 0;
}
}
#endif

extern "C" int userspace_compatible_phymem_direct_map_enable()
{
#ifdef USER_MODE
    if (g_userspace_devmem_fd >= 0) return OS_SUCCESS;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -1;
    g_userspace_devmem_fd = fd;
    return OS_SUCCESS;
#else
    return OS_NOT_SUPPORT;
#endif
}

extern "C" int userspace_compatible_phymem_direct_map_disable()
{
#ifdef USER_MODE
    for (uint32_t i = 0; i < USERSPACE_DIRECT_MAP_MAX; ++i) {
        if (g_userspace_direct_maps[i].user_vbase != nullptr &&
            g_userspace_direct_maps[i].mmap_base != nullptr &&
            g_userspace_direct_maps[i].mmap_len != 0) {
            (void)munmap(g_userspace_direct_maps[i].mmap_base, g_userspace_direct_maps[i].mmap_len);
            userspace_direct_map_record_clear(&g_userspace_direct_maps[i]);
        }
    }
    if (g_userspace_devmem_fd >= 0) {
        (void)close(g_userspace_devmem_fd);
    }
    g_userspace_devmem_fd = -1;
    return OS_SUCCESS;
#else
    return OS_NOT_SUPPORT;
#endif
}

extern "C" KURD_t Kspace_phyaddr_direct_map(vm_interval interval)
{
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    KURD_t success = out_surfaces_default_success();
    KURD_t fail    = out_surfaces_default_failure();
    success.event_code = EVENT_CODE_DIRECT_MAP_PADDR;
    fail.event_code    = EVENT_CODE_DIRECT_MAP_PADDR;

    if (!interval.is_kernel_address() || interval.npages == 0) {
        fail.reason = COMMON_FAIL_REASONS::BAD_INTERVAL;
        return fail;
    }

    interrupt_guard g;
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    VM_DESC new_desc = {
        .start = interval.vbase(),
        .end = interval.vbase() + interval.byte_cnt(),
        .map_type = VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start = interval.pbase(),
        .access = interval.access,
        .committed_full = true,
        .is_vaddr_alloced = false,
    };
    int res = kspace_vm_table->insert(new_desc);
    if (res != OS_SUCCESS) {
        fail.reason = COMMON_FAIL_REASONS::INSERT_VM_DESC_FAIL;
        return fail;
    }
    KURD_t enable_kurd = KspacePageTable::enable_VMentry(interval);
    if (error_kurd(enable_kurd)) {
        (void)kspace_vm_table->remove(interval.vbase());
        return enable_kurd;
    }
    return success;
}

extern "C" vaddr_t Kspace_pinterval_alloc_and_map(vm_interval interval, KURD_t* kurd)
{
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    KURD_t success = out_surfaces_default_success();
    KURD_t fail    = out_surfaces_default_failure();
    success.event_code = EVENT_CODE_PINTERVAL_ALLOC_AND_MAP;
    fail.event_code    = EVENT_CODE_PINTERVAL_ALLOC_AND_MAP;

    if (kurd == nullptr) return 0;
    if (interval.vpn != 0 || interval.ppn == 0 || interval.npages == 0) {
        fail.reason = COMMON_FAIL_REASONS::BAD_INTERVAL;
        *kurd = fail;
        return 0;
    }

    interrupt_guard g;
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    vaddr_t vbase = kspace_vm_table->alloc_available_space(
        interval.byte_cnt(), interval.pbase() % 0x40000000);
    if (vbase == 0) {
        fail.reason = COMMON_FAIL_REASONS::VADDR_ALLOC_FAIL;
        *kurd = fail;
        return 0;
    }

    vm_interval mapped = {
        .vpn    = vbase >> 12,
        .ppn    = interval.ppn,
        .npages = interval.npages,
        .access = interval.access,
    };
    VM_DESC new_desc = {
        .start = vbase,
        .end = vbase + interval.byte_cnt(),
        .map_type = VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start = interval.pbase(),
        .access = interval.access,
        .committed_full = true,
        .is_vaddr_alloced = true,
    };
    int res = kspace_vm_table->insert(new_desc);
    if (res != OS_SUCCESS) {
        fail.reason = COMMON_FAIL_REASONS::INSERT_VM_DESC_FAIL;
        *kurd = fail;
        return 0;
    }
    KURD_t enable_kurd = KspacePageTable::enable_VMentry(mapped);
    if (error_kurd(enable_kurd)) {
        (void)kspace_vm_table->remove(vbase);
        *kurd = enable_kurd;
        return 0;
    }
    *kurd = success;
    return vbase;
}

extern "C" KURD_t Kspace_phyaddr_direct_unmap(vm_interval interval)
{
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    interrupt_guard g;
    KURD_t success = out_surfaces_default_success();
    KURD_t fail    = out_surfaces_default_failure();
    success.event_code = EVENT_CODE_DIRECT_UNMAP_PADDR;
    fail.event_code    = EVENT_CODE_DIRECT_UNMAP_PADDR;

    if (!interval.is_kernel_address()) {
        fail.reason = COMMON_FAIL_REASONS::REMOVE_VM_DESC_FAIL;
        return fail;
    }
    KURD_t status;
    seg_to_pages_info_pakage_t pak;
    {
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    int result = kspace_vm_table->remove(interval.vbase());
    if (result != 0) {
        fail.reason = COMMON_FAIL_REASONS::REMOVE_VM_DESC_FAIL;
        return fail;
    }
    pak = KspacePageTable::disable_VMentry(interval, status);
    if (error_kurd(status))
        return status;
    }

    KURD_t tlb_status = broadcast_invalidate_tlb(&pak);
    if (error_kurd(tlb_status))
        return tlb_status;
    return success;
}
KURD_t broadcast_invalidate_tlb(seg_to_pages_info_pakage_t *pak)
{
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    KURD_t success = out_surfaces_default_success();
    KURD_t fatal   = out_surfaces_default_fatal();
    success.event_code = EVENT_CODE_BCAST_INVALIDATE_TLB;
    fatal.event_code   = EVENT_CODE_BCAST_INVALIDATE_TLB;

    if (!pak)
        return success;

    uint32_t self = fast_get_processor_id();
    uint32_t nproc = logical_processor_count;

    remote_invalidate_seg(pak);

    if (nproc <= 1)
        return success;

    uint8_t done_bitmap[512];
    ksetmem_8(done_bitmap, 0, sizeof(done_bitmap));

    done_bitmap[self / 8] |= (1 << (self % 8));
    uint32_t confirmed = 1;

    uint64_t deadline = ktime::get_microsecond_stamp() + 50000;

    while (confirmed < nproc) {
        if (ktime::get_microsecond_stamp() >= deadline) {
            fatal.reason = COMMON_FATAL_REASONS::TLB_SHOOTDOWN_TIMEOUT;
            panic_info_inshort inshort{
                .is_bug = true, .is_policy = true,
                .is_hw_fault = false, .is_mem_corruption = false,
                .is_escalated = false
            };
            Panic::panic(default_panic_behaviors_flags,
                (char*)"broadcast_invalidate_tlb: deadline exceeded",
                nullptr, &inshort, fatal);
            __builtin_unreachable();
        }

        bool made_progress = false;

        for (uint32_t pid = 0; pid < nproc; pid++) {
            uint32_t byte_idx = pid / 8;
            uint8_t  bit_mask = 1 << (pid % 8);
            if (done_bitmap[byte_idx] & bit_mask)
                continue;

            ipi_package_t ipi;
            ipi.arg         = pak;
            ipi.func        = (uint64_t)remote_invalidate_seg;
            ipi.id          = pid;
            ipi.is_apicid   = false;
            ipi.is_returnable = true;

            __uint128_t result = returnable_ipi_send(&ipi);
            uint64_t ipi_status = (uint64_t)result;

            if (ipi_status == 1) {
                done_bitmap[byte_idx] |= bit_mask;
                confirmed++;
                made_progress = true;
            }
        }

        if (!made_progress) {
            for (int i = 0; i < 8; i++)
                asm volatile("pause");
        }
    }

    return success;
}
void* __wrapped_pgs_valloc(KURD_t*kurd_out,uint64_t _4kbpgscount, page_state_t TYPE, uint8_t alignment_log2) {
#ifdef USER_MODE
    (void)TYPE;
    (void)alignment_log2;
    if (kurd_out == nullptr || _4kbpgscount == 0) {
        if (kurd_out) {
            *kurd_out = KURD_t(result_code::FAIL,
                OS_INVALID_PARAMETER,
                module_code::MEMORY,
                MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
                MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_VALLOC,
                level_code::ERROR,
                err_domain::CORE_MODULE
            );
        }
        return nullptr;
    }

    const uint64_t size = _4kbpgscount * 0x1000;
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        *kurd_out = KURD_t(result_code::FAIL,
            (errno == ENOMEM) ? OS_OUT_OF_MEMORY : OS_FAIL_PAGE_ALLOC,
            module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_VALLOC,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
        return nullptr;
    }

    *kurd_out = KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_VALLOC,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
    return p;
#else
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    KURD_t fail = out_surfaces_default_failure();
    fail.event_code = EVENT_CODE_PAGES_VALLOC;

    interrupt_guard g;
    phyaddr_t result = FreePagesAllocator::alloc(
        _4kbpgscount * 0x1000,
        buddy_alloc_params{
            .numa = 0,
            .try_lock_always_try = 0,
            .align_log2 = alignment_log2
        },
        TYPE,
        *kurd_out
    );
    if (result == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(*kurd_out))
        return nullptr;

    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    vaddr_t vbase = kspace_vm_table->alloc_available_space(
        _4kbpgscount * 0x1000, result % 0x40000000);
    if (vbase == 0) {
        (void)FreePagesAllocator::free(result, _4kbpgscount * 0x1000);
        fail.reason = COMMON_FAIL_REASONS::VADDR_ALLOC_FAIL;
        *kurd_out = fail;
        return nullptr;
    }

    VM_DESC new_desc = {
        .start = vbase,
        .end = vbase + _4kbpgscount * 0x1000,
        .map_type = VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start = result,
        .access = KspacePageTable::PG_RW,
        .committed_full = true,
        .is_vaddr_alloced = true,
    };
    int res = kspace_vm_table->insert(new_desc);
    if (res != OS_SUCCESS) {
        (void)FreePagesAllocator::free(result, _4kbpgscount * 0x1000);
        fail.reason = COMMON_FAIL_REASONS::INSERT_VM_DESC_FAIL;
        *kurd_out = fail;
        return nullptr;
    }

    vm_interval interval = {
        .vpn    = vbase >> 12,
        .ppn    = result >> 12,
        .npages = _4kbpgscount,
        .access = KspacePageTable::PG_RW,
    };
    *kurd_out = KspacePageTable::enable_VMentry(interval);
    return (void*)vbase;
#endif
}
// 栈分配：_4kbpgscount = 可用页数（不含 guard page）
// 返回 priv_stack_base（栈顶，4K对齐）
// 布局（高位→低位）：
//   base + 4K * _4kbpgscount - 64B     — 初始RSP
//   [base, base + 4K * _4kbpgscount)   — 可用栈空间（RSP 向下增长到 base）
//   base                                 — 栈顶（priv_stack_base）
//   [base - 4K, base)                    — guard page（未映射，#PF not-present）
vaddr_t stack_alloc(KURD_t *kurd_out, uint64_t _4kbpgscount)
{
#ifdef USER_MODE
    if (kurd_out == nullptr || _4kbpgscount == 0) {
        if (kurd_out) {
            *kurd_out = KURD_t(result_code::FAIL,
                OS_INVALID_PARAMETER,
                module_code::MEMORY,
                MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
                MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_ALLOC,
                level_code::ERROR,
                err_domain::CORE_MODULE
            );
        }
        return 0;
    }

    // 总页数 = 1 guard + _4kbpgscount 可用
    const uint64_t total_pages = _4kbpgscount + 1;
    void* raw = mmap(nullptr, total_pages * 0x1000,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        *kurd_out = KURD_t(result_code::FAIL,
            (errno == ENOMEM) ? OS_OUT_OF_MEMORY : OS_FAIL_PAGE_ALLOC,
            module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_ALLOC,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
        return 0;
    }

    // 最低页作为 guard page
    mmap(raw, 0x1000, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    *kurd_out = KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_ALLOC,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
    // priv_stack_base = raw + 0x1000
    return reinterpret_cast<vaddr_t>(raw) + 0x1000;
#else
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    KURD_t fail = out_surfaces_default_failure();
    fail.event_code = EVENT_CODE_PAGES_ALLOC;

    interrupt_guard g;
    if (_4kbpgscount == 0) {
        fail.reason = COMMON_FAIL_REASONS::BAD_PARAM;
        *kurd_out = fail;
        return 0;
    }

    phyaddr_t phys_base = FreePagesAllocator::alloc(
        _4kbpgscount * 0x1000,
        buddy_alloc_params{
            .numa = 0,
            .try_lock_always_try = 0,
            .align_log2 = 12
        },
        page_state_t::kernel_pinned,
        *kurd_out
    );
    if (phys_base == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(*kurd_out))
        return phys_base;

    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    uint64_t total_virt_pages = _4kbpgscount + 1;
    vaddr_t vaddr = kspace_vm_table->alloc_available_space(
        total_virt_pages * 0x1000, phys_base % 0x40000000);
    if (vaddr == 0) {
        (void)FreePagesAllocator::free(phys_base, _4kbpgscount * 0x1000);
        fail.reason = COMMON_FAIL_REASONS::VADDR_ALLOC_FAIL;
        *kurd_out = fail;
        return 0;
    }

    vaddr_t base = vaddr + 0x1000;

    VM_DESC new_desc = {
        .start = vaddr,
        .end   = vaddr + total_virt_pages * 0x1000,
        .map_type = VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start = phys_base,
        .access = KspacePageTable::PG_RW,
        .committed_full = true,
        .is_vaddr_alloced = true,
    };
    int res = kspace_vm_table->insert(new_desc);
    if (res != OS_SUCCESS) {
        (void)FreePagesAllocator::free(phys_base, _4kbpgscount * 0x1000);
        fail.reason = COMMON_FAIL_REASONS::INSERT_VM_DESC_FAIL;
        *kurd_out = fail;
        return 0;
    }

    vm_interval interval = {
        .vpn    = base >> 12,
        .ppn    = phys_base >> 12,
        .npages = _4kbpgscount,
        .access = KspacePageTable::PG_RW,
    };
    *kurd_out = KspacePageTable::enable_VMentry(interval);
    return base;
#endif
}

KURD_t __wrapped_pgs_vfree(void* vbase, uint64_t _4kbpgscount)
{
#ifdef USER_MODE
    if (vbase == nullptr || _4kbpgscount == 0) {
        return KURD_t(result_code::FAIL,
            OS_INVALID_PARAMETER,
            module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
    }

    const uint64_t size = _4kbpgscount * 0x1000;
    if (munmap(vbase, size) != 0) {
        return KURD_t(result_code::FAIL,
            OS_MEMORY_FREE_FAULT,
            module_code::MEMORY,
            MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
    }

    return KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCATIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
#else
    using namespace MEMMODULE_LOCATIONS::OUT_SURFACES_EVENTS;
    KURD_t fail = out_surfaces_default_failure();
    fail.event_code = EVENT_CODE_PAGES_VFREE;

    interrupt_guard g;
    phyaddr_t pbase = 0;
    KURD_t status = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)vbase, pbase);
    if (status.result != result_code::SUCCESS)
        return status;

    status = FreePagesAllocator::free(pbase, _4kbpgscount * 0x1000);

    vm_interval interval = {
        .vpn    = reinterpret_cast<vaddr_t>(vbase) >> 12,
        .ppn    = pbase >> 12,
        .npages = _4kbpgscount,
        .access = KspacePageTable::PG_RW
    };
    seg_to_pages_info_pakage_t pak;
    {
        spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
        int result = kspace_vm_table->remove((vaddr_t)vbase);
        if (result != 0) {
            fail.reason = COMMON_FAIL_REASONS::REMOVE_VM_DESC_FAIL;
            return fail;
        }
        KURD_t inner_kurd;
        pak = KspacePageTable::disable_VMentry(interval, inner_kurd);
        if (error_kurd(inner_kurd))
            return inner_kurd;
    }

    KURD_t tlb_status = broadcast_invalidate_tlb(&pak);
    if (error_kurd(tlb_status))
        return tlb_status;

    KURD_t success = out_surfaces_default_success();
    success.event_code = EVENT_CODE_PAGES_VFREE;
    return success;
#endif
}
#ifdef KERNEL_MODE
void* __wrapped_heap_alloc(uint64_t size,KURD_t*kurd,alloc_flags_t flags) {
    interrupt_guard g;
    return kpoolmemmgr_t::kalloc(size,*kurd,flags);
}
void __wrapped_heap_free(void*addr){
    interrupt_guard g;
    kpoolmemmgr_t::kfree(addr);
}
void* __wrapped_heap_realloc(void*addr,uint64_t size,KURD_t*kurd,alloc_flags_t flags) {
    interrupt_guard g;
    return kpoolmemmgr_t::realloc(addr,*kurd, size, flags);
}
void* operator new(size_t size) {
    KURD_t kurd;
    void* result= __wrapped_heap_alloc(size,&kurd,default_flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void *operator new(size_t size, alloc_flags_t flags)
{
    KURD_t kurd;
    void* result= __wrapped_heap_alloc(size,&kurd,flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}
void *operator new[](size_t size)
{
    KURD_t kurd;
    void* result= kpoolmemmgr_t::kalloc(size,kurd,default_flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void *operator new[](size_t size, alloc_flags_t flags)
{
    KURD_t kurd;
    void* result= __wrapped_heap_alloc(size,&kurd,flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void operator delete(void* ptr) noexcept {
    kpoolmemmgr_t::kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept {

}

void operator delete[](void* ptr) noexcept {
    kpoolmemmgr_t::kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {

}

// 放置 new 操作符
void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

void* operator new[](size_t, void* ptr) noexcept {
    return ptr;
}

#endif
