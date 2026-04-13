#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/AddresSpace.h"
#include "memory/FreePagesAllocator.h"
#include "panic.h"
#include "util/OS_utils.h"
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

vaddr_t phyaddr_direct_map(vm_interval*interval, KURD_t *kurd_out)
{
    using namespace MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::DIRECT_MAP_PADDR_RESULTS;
    KURD_t success,fail;
    success=KURD_t(result_code::SUCCESS,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_MAP_PADDR,
            level_code::INFO,
            err_domain::CORE_MODULE
        );
    fail=KURD_t(result_code::FAIL,
            0,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_DIRECT_MAP_PADDR,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );

#ifdef USER_MODE
    if(interval==nullptr||kurd_out==nullptr||
       interval->size==0||(interval->size&0xFFF)||
       (interval->pbase&0xFFF)){
        if (kurd_out) {
            fail.reason=FAIL_REASONS::REASON_CODE_BAD_INTERVAL;
            *kurd_out=fail;
        }
        return 0;
    }
    if (g_userspace_devmem_fd < 0) {
        fail.reason=FAIL_REASONS::REASON_CODE_BAD_INTERVAL;
        *kurd_out=fail;
        return 0;
    }

    int prot = 0;
    if (interval->access.is_readable) prot |= PROT_READ;
    if (interval->access.is_writeable) prot |= PROT_WRITE;

    static constexpr uint64_t kHugePageSize = 0x200000; // 2MB
    const uint64_t aligned_pbase = align_down(interval->pbase, kHugePageSize);
    const uint64_t in_page_offset = interval->pbase - aligned_pbase;
    const uint64_t mmap_len = align_up(in_page_offset + interval->size, kHugePageSize);

    void* mapped = mmap(nullptr,
                        mmap_len,
                        prot,
                        MAP_SHARED,
                        g_userspace_devmem_fd,
                        static_cast<off_t>(aligned_pbase));
    if (mapped == MAP_FAILED) {
        fail.reason=FAIL_REASONS::REASON_CODE_NO_AVALIABLE_VADDR_SPACE;
        *kurd_out=fail;
        return 0;
    }

    void* user_vaddr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(mapped) + in_page_offset);
    int add_status = userspace_direct_map_record_add(user_vaddr, mapped, mmap_len);
    if (add_status != OS_SUCCESS) {
        (void)munmap(mapped, mmap_len);
        fail.reason=FAIL_REASONS::REASON_CODE_NO_AVALIABLE_VADDR_SPACE;
        *kurd_out=fail;
        return 0;
    }
    interval->vbase = reinterpret_cast<vaddr_t>(user_vaddr);
    *kurd_out = success;
    return interval->vbase;
#elif defined(KERNEL_MODE)
    if(interval==nullptr||kurd_out==nullptr||
       interval->size==0||(interval->size&0xFFF)||
       (interval->pbase&0xFFF)){
        fail.reason=FAIL_REASONS::REASON_CODE_BAD_INTERVAL;
        if(kurd_out)*kurd_out=fail;
        return 0;
    }

    interrupt_guard g;
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);

    vaddr_t vbase=kspace_vm_table->alloc_available_space(interval->size,interval->pbase%0x40000000);
    if(vbase==0){
        fail.reason=FAIL_REASONS::REASON_CODE_NO_AVALIABLE_VADDR_SPACE;
        *kurd_out=fail;
        return 0;
    }
    interval->vbase=vbase;
    VM_DESC new_desc={
        .start=vbase,
        .end=vbase+interval->size,
        .map_type=VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start=interval->pbase,
        .access=interval->access,
        .committed_full=true,
        .is_vaddr_alloced=true,
    };
    int res=kspace_vm_table->insert(new_desc);
    if(res!=OS_SUCCESS){
        fail.reason=FAIL_REASONS::REASON_CODE_INSERT_VM_DESC_FAIL;
        *kurd_out=fail;
        return 0;
    }
    KURD_t enable_kurd=KspacePageTable::enable_VMentry(*interval);
    if(error_kurd(enable_kurd)){
        (void)kspace_vm_table->remove(vbase); // 回滚RB树插入
        fail.reason=FAIL_REASONS::REASON_CODE_ENABLE_VM_ENTRY_FAIL;
        *kurd_out=fail;
        return 0;
    }
    *kurd_out=success;
    return vbase;
#else
    (void)interval;
    if(kurd_out)*kurd_out=fail;
    return 0;
#endif
}

extern "C" KURD_t phyaddr_direct_unmap(vm_interval *interval,uint64_t size)
{
    using namespace MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::DIRECT_UNMAP_PADDR_RESULTS;
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
#ifdef USER_MODE
    (void)size;
    if (interval==nullptr || interval->vbase==0) {
        fail.reason=FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    userspace_direct_map_record_t* rec = userspace_direct_map_record_find(reinterpret_cast<void*>(interval->vbase));
    if (rec == nullptr || rec->mmap_base == nullptr || rec->mmap_len == 0) {
        fail.reason=FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    if (munmap(rec->mmap_base, rec->mmap_len) != 0) {
        fail.reason=FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    userspace_direct_map_record_clear(rec);
    interval->vbase = 0;
    return success;
#elif defined(KERNEL_MODE)
    if(interval==nullptr || interval->vbase==0){
        fail.reason=FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    const uint64_t unmap_size = (size==0)?interval->size:size;
    if(unmap_size==0 || (unmap_size&0xFFF)){
        fail.reason=FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }

    vm_interval unmap_interval={
        .vbase=interval->vbase,
        .pbase=interval->pbase,
        .size=unmap_size,
        .access=interval->access
    };
    interrupt_guard g;
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    int result= kspace_vm_table->remove(interval->vbase);
    if(result!=0){
        fail.reason=FAIL_REASONS::REASON_CODE_REMOVE_VM_ENTRY_FAIL;
        return fail;
    }
    KURD_t status = KspacePageTable::disable_VMentry(unmap_interval);
    if (error_kurd(status)) return status;
    interval->vbase = 0;
    return success;
#else
    (void)interval;
    (void)size;
    return fail;
#endif
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
    vaddr_t vbase=kspace_vm_table->alloc_available_space(_4kbpgscount*0x1000,result%0x400000000);
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
        .vbase=vbase,
        .pbase=result,
        .size=_4kbpgscount*0x1000,
        .access=KspacePageTable::PG_RW
    };
    *kurd_out=KspacePageTable::enable_VMentry(interval);

    return (void*)vbase;
#endif
}
vaddr_t stack_alloc(KURD_t *kurd_out, uint64_t _4kbpgscount)
{
#ifdef USER_MODE
    if (kurd_out == nullptr || _4kbpgscount == 0) {
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

    const uint64_t real_alloc_phypages = _4kbpgscount + 1;
    const uint64_t size = real_alloc_phypages * 0x1000;
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
    if(_4kbpgscount==0)return 0;
    uint64_t real_alloc_phypages=_4kbpgscount+1;
    phyaddr_t result=FreePagesAllocator::alloc(
        real_alloc_phypages*0x1000,
        buddy_alloc_params{
            .numa=0,
            .try_lock_always_try=0,
            .align_log2=12
        },
        page_state_t::kernel_pinned,
        *kurd_out
    );
    if(result==FreePagesAllocator::INVALID_ALLOC_BASE||error_kurd(*kurd_out)){
        //尝试用phymemspace_mgr::pages_linear_scan_and_alloc
            return result;
    }
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    vaddr_t vbase=kspace_vm_table->alloc_available_space((real_alloc_phypages+1)*0x1000,result%0x400000000);
    if(vbase==0){
        return 0;
    }
    vbase+=0x1000;
    VM_DESC new_desc={
        .start=vbase,
        .end=vbase+real_alloc_phypages*0x1000,
        .map_type=VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start=result,
        .access=KspacePageTable::PG_RW,
        .committed_full=true,
        .is_vaddr_alloced=true,
    };
    int res=kspace_vm_table->insert(new_desc);
    if(res!=OS_SUCCESS){
        return 0;
    }
    vm_interval interval={
        .vbase=vbase,
        .pbase=result,
        .size=real_alloc_phypages*0x1000,
        .access=KspacePageTable::PG_RW
    };
    *kurd_out=KspacePageTable::enable_VMentry(interval);
    return vbase+_4kbpgscount*0x1000;
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
        .vbase=(vaddr_t)vbase,
        .pbase=pbase,
        .size=_4kbpgscount*0x1000,
        .access=KspacePageTable::PG_RW
    };
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    int result= kspace_vm_table->remove((vaddr_t)vbase);
    if(result!=0){
        return KURD_t(result_code::FAIL,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::PAGES_VFREE_RESULTS::FAIL_REASONS::REMOVE_VMENTRY_FAIL,
            module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_OUT_SURFACES,
            MEMMODULE_LOCAIONS::OUT_SRFACES_EVENTS::EVENT_CODE_PAGES_VFREE,
            level_code::ERROR,
            err_domain::CORE_MODULE
        );
    }
    return KspacePageTable::disable_VMentry(interval);
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

void* operator new[](size_t size, bool vaddraquire, uint8_t alignment) {
    KURD_t kurd;
    void* result= kpoolmemmgr_t::kalloc(size,kurd, default_flags);
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
