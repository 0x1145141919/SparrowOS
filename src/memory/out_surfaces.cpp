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
// 重载全局 new/delete 操作符
namespace MEMMODULE_LOCAIONS{
    namespace OUT_SRFACES_EVENTS{
        uint8_t EVENT_CODE_PAGES_VALLOC=0;
        uint8_t EVENT_CODE_PAGES_VFREE=1;
        namespace PAGES_VFREE_RESULTS{
            namespace FAIL_REASONS{
                uint16_t REMOVE_VMENTRY_FAIL=1;

            };
        };
        uint8_t EVENT_CODE_PAGES_ALLOC=2;
        uint8_t EVENT_CODE_PAGES_FREE=3;
        uint8_t EVENT_CODE_KEYWORD_NEW=4;
        uint8_t EVENT_CODE_KEYWORD_DELETE=5;
        uint8_t EVENT_CODE_DIRECT_MAP_PADDR=6;
        namespace DIRECT_MAP_PADDR_RESULTS{
            namespace FAIL_REASONS{
                uint16_t REASON_CODE_BAD_INTERVAL=1;
                uint16_t REASON_CODE_NO_AVALIABLE_VADDR_SPACE=2;
                uint16_t REASON_CODE_INSERT_VM_DESC_FAIL=3;
                uint16_t REASON_CODE_ENABLE_VM_ENTRY_FAIL=4;
            };
        };
        uint8_t EVENT_CODE_DIRECT_UNMAP_PADDR=7;
        namespace DIRECT_UNMAP_PADDR_RESULTS{
            namespace FAIL_REASONS{
                uint16_t REASON_CODE_REMOVE_VM_ENTRY_FAIL=1;
            };
        };
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
    using namespace MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::DIRECT_MAP_PADDR_RESULTS;
    KURD_t success=KURD_t(result_code::SUCCESS,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_MAP_PADDR,
            level_code::INFO,
            err_domain::CORE_MODULE
        );
    KURD_t fail=KURD_t(result_code::FAIL,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_MAP_PADDR,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );

    if (!interval.is_kernel_address() || interval.npages == 0) {
        fail.reason = FAIL_REASONS::REASON_CODE_BAD_INTERVAL;
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
        fail.reason = FAIL_REASONS::REASON_CODE_INSERT_VM_DESC_FAIL;
        return fail;
    }
    KURD_t enable_kurd = KspacePageTable::enable_VMentry(interval);
    if (error_kurd(enable_kurd)) {
        (void)kspace_vm_table->remove(interval.vbase());
        fail.reason = FAIL_REASONS::REASON_CODE_ENABLE_VM_ENTRY_FAIL;
        return fail;
    }
    return success;
}

extern "C" vaddr_t Kspace_pinterval_alloc_and_map(vm_interval interval, KURD_t* kurd)
{
    using namespace MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::DIRECT_MAP_PADDR_RESULTS;
    KURD_t success=KURD_t(result_code::SUCCESS,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_MAP_PADDR,
            level_code::INFO,
            err_domain::CORE_MODULE
        );
    KURD_t fail=KURD_t(result_code::FAIL,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_MAP_PADDR,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );

    if (kurd == nullptr) return 0;
    if (interval.vpn != 0 || interval.ppn == 0 || interval.npages == 0) {
        fail.reason = FAIL_REASONS::REASON_CODE_BAD_INTERVAL;
        *kurd = fail;
        return 0;
    }

    interrupt_guard g;
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    vaddr_t vbase = kspace_vm_table->alloc_available_space(
        interval.byte_cnt(), interval.pbase() % 0x40000000);
    if (vbase == 0) {
        fail.reason = FAIL_REASONS::REASON_CODE_NO_AVALIABLE_VADDR_SPACE;
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
        fail.reason = FAIL_REASONS::REASON_CODE_INSERT_VM_DESC_FAIL;
        *kurd = fail;
        return 0;
    }
    KURD_t enable_kurd = KspacePageTable::enable_VMentry(mapped);
    if (error_kurd(enable_kurd)) {
        (void)kspace_vm_table->remove(vbase);
        fail.reason = FAIL_REASONS::REASON_CODE_ENABLE_VM_ENTRY_FAIL;
        *kurd = fail;
        return 0;
    }
    *kurd = success;
    return vbase;
}

extern "C" KURD_t Kspace_phyaddr_direct_unmap(vm_interval interval)
{
    using namespace MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::DIRECT_UNMAP_PADDR_RESULTS;
    interrupt_guard g;
    KURD_t success=KURD_t(result_code::SUCCESS,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_UNMAP_PADDR,
            level_code::INFO,
            err_domain::CORE_MODULE
        );
    KURD_t fail=KURD_t(result_code::FAIL,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_UNMAP_PADDR,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );

    if (!interval.is_kernel_address()) {
        fail.reason = FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    KURD_t status; 
    seg_to_pages_info_pakage_t pak;
    {
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    int result = kspace_vm_table->remove(interval.vbase());
    if (result != 0) {
        fail.reason = FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    pak= KspacePageTable::disable_VMentry(interval,status);
    }
    
    status=broadcast_invalidate_tlb(&pak);
    if (error_kurd(status)) return status;
    return success;
}
KURD_t broadcast_invalidate_tlb(seg_to_pages_info_pakage_t *pak)
{
    KURD_t success = KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_UNMAP_PADDR,
        level_code::INFO,
        err_domain::CORE_MODULE);
    KURD_t fatal = KURD_t(result_code::FATAL, 0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_UNMAP_PADDR,
        level_code::FATAL,
        err_domain::CORE_MODULE);

    if (!pak)
        return success;

    uint32_t self = fast_get_processor_id();
    uint32_t nproc = logical_processor_count;

    // ── 本核直接执行 ──
    remote_invalidate_seg(pak);

    if (nproc <= 1)
        return success;

    // ── 跟踪已完成的远端核 ──
    // MAX_PROCESSORS_COUNT = 4096, 栈上 512B bitmap
    uint8_t done_bitmap[512];
    ksetmem_8(done_bitmap, 0, sizeof(done_bitmap));

    // 本核视为已完成
    done_bitmap[self / 8] |= (1 << (self % 8));
    uint32_t confirmed = 1;

    // ── 多核 TLB shootdown ────────────────────────────────────
    uint64_t deadline = ktime::get_microsecond_stamp() + 50000;  // 50ms

    while (confirmed < nproc) {
        if (ktime::get_microsecond_stamp() >= deadline) {
            fatal.reason = 1;
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
            // 跳过已完成核
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

            __uint128_t result = ret_ipi_send(&ipi);
            uint64_t ipi_status = (uint64_t)result;  // lo64 = IPI 状态

            if (ipi_status == 1) {
                done_bitmap[byte_idx] |= bit_mask;
                confirmed++;
                made_progress = true;
            }
            // BUSY(2) / TIMEOUT(3) → 下轮重试
        }

        // 无进展 → pause 短暂避让
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
                MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
                MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VALLOC,
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
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VALLOC,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
        return nullptr;
    }

    *kurd_out = KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VALLOC,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
    return p;
#else
    interrupt_guard g;
    phyaddr_t result=FreePagesAllocator::alloc(
        _4kbpgscount*0x1000,
        buddy_alloc_params{
            .numa=0,
            .try_lock_always_try=0,
            .align_log2=alignment_log2
        },
        TYPE,
        *kurd_out
    );
    if(result==FreePagesAllocator::INVALID_ALLOC_BASE||error_kurd(*kurd_out)){
        //尝试用phymemspace_mgr::pages_linear_scan_and_alloc
            return nullptr;
    }
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    vaddr_t vbase=kspace_vm_table->alloc_available_space(_4kbpgscount*0x1000,result%0x40000000);
    if(vbase==0){
        //回滚FreePagesAllocator::alloc
        return nullptr;
    }
    VM_DESC new_desc={
        .start=vbase,
        .end=vbase+_4kbpgscount*0x1000,
        .map_type=VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start=result,
        .access=KspacePageTable::PG_RW,
        .committed_full=true,
        .is_vaddr_alloced=true,
    };
    int res=kspace_vm_table->insert(new_desc);
    if(res!=OS_SUCCESS){
        return nullptr;
    }
    vm_interval interval={
        .vpn=vbase>>12,
        .ppn=result>>12,
        .npages=_4kbpgscount,
        .access=KspacePageTable::PG_RW
    };
    *kurd_out=KspacePageTable::enable_VMentry(interval);

    return (void*)vbase;
#endif
}
// 栈分配：_4kbpgscount = 总页数（含 guard page）
// 返回 priv_stack_base（栈顶），4K对齐
// 布局（高位→低位）：
//   priv_stack_base                                              — 栈顶
//   [priv_stack_base - 4K, priv_stack_base)                        — guard page（未映射）
//   [priv_stack_base - 4K * _4kbpgscount, priv_stack_base - 4K)   — 可用栈空间
//   初始RSP = priv_stack_base - 64B
vaddr_t stack_alloc(KURD_t *kurd_out, uint64_t _4kbpgscount)
{
#ifdef USER_MODE
    if (kurd_out == nullptr || _4kbpgscount < 2) {
        if (kurd_out) {
            *kurd_out = KURD_t(result_code::FAIL,
                OS_INVALID_PARAMETER,
                module_code::MEMORY,
                MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
                MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_ALLOC,
                level_code::ERROR,
                err_domain::CORE_MODULE
            );
        }
        return 0;
    }

    const uint64_t total_size = _4kbpgscount * 0x1000;
    void* base = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        *kurd_out = KURD_t(result_code::FAIL,
            (errno == ENOMEM) ? OS_OUT_OF_MEMORY : OS_FAIL_PAGE_ALLOC,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_ALLOC,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
        return 0;
    }

    // guard page：最高页，mprotect PROT_NONE
    void* guard_addr = reinterpret_cast<void*>(reinterpret_cast<vaddr_t>(base) + (_4kbpgscount - 1) * 0x1000);
    mmap(guard_addr, 0x1000, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    *kurd_out = KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_ALLOC,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
    return reinterpret_cast<vaddr_t>(base) + _4kbpgscount * 0x1000;
#else
    interrupt_guard g;
    if (_4kbpgscount < 2) return 0;
    uint64_t usable_pages = _4kbpgscount - 1;

    phyaddr_t phys_base = FreePagesAllocator::alloc(
        _4kbpgscount * 0x1000,
        buddy_alloc_params{
            .numa=0,
            .try_lock_always_try=0,
            .align_log2=12
        },
        page_state_t::kernel_pinned,
        *kurd_out
    );
    if (phys_base == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(*kurd_out)) {
        return phys_base;
    }

    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    // 虚拟空间：_4kbpgscount 页
    vaddr_t vbase = kspace_vm_table->alloc_available_space(
        _4kbpgscount * 0x1000, phys_base % 0x40000000);
    if (vbase == 0) {
        // TODO: rollback FreePagesAllocator::alloc
        return 0;
    }

    // 只映射低 usable_pages 页（guard page 不映射）
    VM_DESC new_desc = {
        .start = vbase,
        .end   = vbase + usable_pages * 0x1000,
        .map_type = VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start = phys_base,
        .access = KspacePageTable::PG_RW,
        .committed_full = true,
        .is_vaddr_alloced = true,
    };
    int res = kspace_vm_table->insert(new_desc);
    if (res != OS_SUCCESS) {
        return 0;
    }

    vm_interval interval = {
        .vpn    = vbase >> 12,
        .ppn    = phys_base >> 12,
        .npages = usable_pages,
        .access = KspacePageTable::PG_RW,
    };
    *kurd_out = KspacePageTable::enable_VMentry(interval);
    // priv_stack_base = vbase + _4kbpgscount * 0x1000（即 guard page 顶端）
    return vbase + _4kbpgscount * 0x1000;
#endif
}

KURD_t __wrapped_pgs_vfree(void*vbase,uint64_t _4kbpgscount){
#ifdef USER_MODE
    if (vbase == nullptr || _4kbpgscount == 0) {
        return KURD_t(result_code::FAIL,
            OS_INVALID_PARAMETER,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
    }

    const uint64_t size = _4kbpgscount * 0x1000;
    if (munmap(vbase, size) != 0) {
        return KURD_t(result_code::FAIL,
            OS_MEMORY_FREE_FAULT,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
    }

    return KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
#else
    interrupt_guard g;
    phyaddr_t pbase=0;
    KURD_t status=KspacePageTable::v_to_phyaddrtraslation((vaddr_t)vbase,pbase);
    if(status.result!=result_code::SUCCESS){
        return status;
    }
    status=FreePagesAllocator::free(pbase,_4kbpgscount*0x1000);
    vm_interval interval={
        .vpn=reinterpret_cast<vaddr_t>(vbase)>>12,
        .ppn=pbase>>12,
        .npages=_4kbpgscount,
        .access=KspacePageTable::PG_RW
    };
    seg_to_pages_info_pakage_t pak;
    {
        spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
        int result = kspace_vm_table->remove((vaddr_t)vbase);
        if (result != 0) {
            return KURD_t(result_code::FAIL,
                MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::PAGES_VFREE_RESULTS::FAIL_REASONS::REMOVE_VMENTRY_FAIL,
                module_code::MEMORY,
                MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
                MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
                level_code::ERROR,
                err_domain::CORE_MODULE
            );
        }
        KURD_t inner_kurd;
        pak = KspacePageTable::disable_VMentry(interval, inner_kurd);
        if (error_kurd(inner_kurd))
            return inner_kurd;
    }  // kspace_pagetable_modify_lock 已释放

    // ── TLB shootdown（无锁）──
    KURD_t tlb_status = broadcast_invalidate_tlb(&pak);
    if (error_kurd(tlb_status))
        return tlb_status;

    return KURD_t(result_code::SUCCESS, 0,
        module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
        MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
        level_code::INFO,
        err_domain::CORE_MODULE
    );
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
