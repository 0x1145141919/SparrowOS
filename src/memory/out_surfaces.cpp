#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/AddresSpace.h"
#include "memory/FreePagesAllocator.h"
#include "panic.h"
#include "util/OS_utils.h"
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
    }
}

void* __wrapped_pgs_valloc(KURD_t*kurd_out,uint64_t _4kbpgscount, page_state_t TYPE, uint8_t alignment_log2) {
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
}
vaddr_t stack_alloc(KURD_t *kurd_out, uint64_t _4kbpgscount)
{
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
}
#ifdef KERNEL_MODE
KURD_t __wrapped_pgs_vfree(void*vbase,uint64_t _4kbpgscount){
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
     

}

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
