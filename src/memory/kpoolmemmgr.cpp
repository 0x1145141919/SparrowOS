#include "memory/memory_base.h"
#include "util/OS_utils.h"
#include "abi/os_error_definitions.h"
#include "util/kout.h"
#include "panic.h"
#include "memory/kpoolmemmgr.h"
#include "memory/AddresSpace.h"
#include "firmware/ACPI_APIC.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/kptrace.h"
#include "util/OS_utils.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#ifdef USER_MODE
#include "stdlib.h"
#include "kpoolmemmgr.h"
#endif  


kpoolmemmgr_t::HCB_v2**kpoolmemmgr_t::HCB_ARRAY;
// 定义类中的静态成员变量
bool kpoolmemmgr_t::is_muli_heap_enabled = false;
kpoolmemmgr_t::HCB_v2 kpoolmemmgr_t::first_linekd_heap;
spinrwlock_cpp_t kpoolmemmgr_t::HCB_ARRAY_lock;
VM_DESC heap_area={.start=0,
    .end=0,
    .map_type=VM_DESC::MAP_NONE,
    .phys_start=0,
    .access=KspacePageTable::PG_RW,
    .committed_full=0,
    .is_vaddr_alloced=0,
    .is_out_bound_protective=0
};
KURD_t kpoolmemmgr_t::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR,0,0,err_domain::CORE_MODULE);
}
KURD_t kpoolmemmgr_t::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result = result_code::SUCCESS;
    kurd.level = level_code::INFO;
    return  kurd;
}
KURD_t kpoolmemmgr_t::default_fail()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return  kurd;
}
KURD_t kpoolmemmgr_t::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return  kurd;
}
KURD_t kpoolmemmgr_t::Init(loaded_VM_interval *first_static_heap, loaded_VM_interval *first_static_heap_bitmap)
{
    first_linekd_heap.first_linekd_heap_Init(first_static_heap,first_static_heap_bitmap);
    KURD_t success=default_success();
    return success;
}
KURD_t kpoolmemmgr_t::multi_heap_enable() 
//只能由BSP在SCHDULE阶段之前调用
{
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    if(is_muli_heap_enabled){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_ALREADY_ENABLED;
        return fail;
    }
    uint64_t processor_count=gAnalyzer->processor_x64_list->size();
    if(processor_count==0){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_BAD_PROCESSOR_COUNT;
        return fail;
    }
    uint64_t hcb_count=processor_count*(1<<PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);
    HCB_ARRAY_lock.write_lock();
    HCB_ARRAY=new kpoolmemmgr_t::HCB_v2*[hcb_count];
    ksetmem_8(HCB_ARRAY, 0, hcb_count*sizeof(HCB_ARRAY[0]));
    HCB_ARRAY_lock.write_unlock();
    uint64_t heap_area_size=HCB_DEFAULT_SIZE*hcb_count;
    heap_area.start=kspace_vm_table->alloc_available_space(heap_area_size,0);
    if(heap_area.start==0){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_NO_VADDR_SPACE;
        return fail;
    }
    heap_area.end=heap_area.start+heap_area_size;
    heap_area.is_vaddr_alloced=1;
    if(kspace_vm_table->insert(heap_area)!=OS_SUCCESS){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_VM_ADD_FAIL;
        return fail;
    }
    is_muli_heap_enabled=true;
    return success;
}
KURD_t kpoolmemmgr_t::alloc_heap(uint32_t idx)
{
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    uint64_t processor_count=gAnalyzer->processor_x64_list->size();
    uint64_t hcb_count=processor_count*(1<<PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);
    if(idx>=hcb_count){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_IDX_OUT_OF_RANGE;
        return fail;
    }
    HCB_ARRAY_lock.read_lock();
    bool exists=(HCB_ARRAY[idx]!=nullptr);
    HCB_ARRAY_lock.read_unlock();
    if(exists){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_HEAP_ALREADY_EXISTS;
        return fail;
    }
    alloc_flags_t flags=default_flags;
    void* ptr=nullptr;
    KURD_t contain=first_linekd_heap.in_heap_alloc(
        ptr,sizeof(HCB_v2),flags
    );
    if(!success_all_kurd(contain))return contain;
    HCB_ARRAY_lock.write_lock();
    if(HCB_ARRAY[idx]){
        HCB_ARRAY_lock.write_unlock();
        first_linekd_heap.free(ptr);
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_HEAP_ALREADY_EXISTS;
        return fail;
    }
    HCB_ARRAY[idx]=(HCB_v2*)ptr;
    new(HCB_ARRAY[idx]) HCB_v2(HCB_DEFAULT_SIZE,heap_area.start+HCB_DEFAULT_SIZE*idx);
    contain=HCB_ARRAY[idx]->second_stage_Init();
    if(!success_all_kurd(contain)){
        HCB_ARRAY[idx]=nullptr;
        HCB_ARRAY_lock.write_unlock();
        first_linekd_heap.free(ptr);
        return contain;
    }
    HCB_ARRAY_lock.write_unlock();
    return success;
}
KURD_t kpoolmemmgr_t::free_heap(uint32_t idx)
{
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_PER_PROCESSOR_HEAP_INIT;
    uint64_t processor_count=gAnalyzer->processor_x64_list->size();
    uint64_t hcb_count=processor_count*(1<<PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);
    if(idx>=hcb_count){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_IDX_OUT_OF_RANGE;
        return fail;
    }
    HCB_ARRAY_lock.read_lock();
    bool exists=(HCB_ARRAY[idx]!=nullptr);
    HCB_ARRAY_lock.read_unlock();
    if(!exists){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS::REASON_CODE_HEAP_NOT_EXIST;
        return fail;
    }
    HCB_ARRAY_lock.write_lock();
    HCB_v2* hcb=HCB_ARRAY[idx];
    HCB_ARRAY[idx]=nullptr;
    HCB_ARRAY_lock.write_unlock();
    hcb->~HCB_v2();
    KURD_t contain=first_linekd_heap.free(hcb);
    if(!success_all_kurd(contain))return contain;
    return success;
}
kpoolmemmgr_t::HCB_v2 *kpoolmemmgr_t::find_hcb_by_address(void *ptr)
{

    if(first_linekd_heap.is_addr_belong_to_this_hcb(ptr))return &first_linekd_heap;
    if((uint64_t)ptr>=heap_area.end||(uint64_t)ptr<heap_area.start)return nullptr;
    uint64_t offset=(uint64_t)ptr-(uint64_t)heap_area.start;
    uint64_t idx=offset/HCB_DEFAULT_SIZE;
    HCB_ARRAY_lock.read_lock();
    HCB_v2* hcb=HCB_ARRAY[idx];
    HCB_ARRAY_lock.read_unlock();
    return hcb;
}



// 辅助函数：处理堆对象被销毁的错误
static void handle_heap_obj_destroyed(const char* operation, void* ptr) {
    bsp_kout<< now << "kpoolmemmgr_t::" << operation 
                  << ": heap object destroyed at address 0x" << (void*)ptr << kendl;
    self_trace();
    panic_info_inshort inshort={
        .is_bug=false,
        .is_policy=true,
        .is_hw_fault=false,
        .is_mem_corruption=true,
        .is_escalated=false
    };
    Panic::panic(default_panic_behaviors_flags,"Heap object has been destroyed",nullptr,&inshort,KURD_t());
}

// 辅助函数：获取当前处理器的堆复合结构


void* kpoolmemmgr_t::kalloc(uint64_t size,KURD_t&no_succes_report,alloc_flags_t flags)
{
    interrupt_guard g;
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_ALLOC;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_ALLOC;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_ALLOC;
    // 参数验证
    if (size == 0){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SIZE_IS_ZERO;
        no_succes_report=fail;
        return nullptr;
    }
    KURD_t contain=KURD_t();
    void* ptr = nullptr;
    if((!flags.force_first_linekd_heap)&&is_muli_heap_enabled) {
        // 尝试在当前处理器的堆中分配
        x64_local_processor*local_processor=(x64_local_processor*)read_gs_u64(PROCESSOR_SELF_RESOURCES_COMPELX_GS_INDEX);
        uint32_t id=local_processor->get_processor_id();
        for(uint32_t i = 0; i < (1<<PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2); ++i){
            uint32_t idx=(id<<PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2)+i;
            HCB_ARRAY_lock.read_lock();
            HCB_v2* hcb=HCB_ARRAY[idx];
            HCB_ARRAY_lock.read_unlock();
            if(!hcb){
                contain=alloc_heap(idx);
                if(!success_all_kurd(contain)){
                    no_succes_report = contain;
                    return nullptr;
                }
                HCB_ARRAY_lock.read_lock();
                hcb=HCB_ARRAY[idx];
                HCB_ARRAY_lock.read_unlock();
            }
            if(!hcb){
                fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_MEM;
                no_succes_report = fail;
                return nullptr;
            }
            contain=hcb->in_heap_alloc(ptr,size,flags);
            if(success_all_kurd(contain)){
                no_succes_report = contain;
                return ptr;
            }
        }
        // 所有每CPU堆都失败，回退到第一个链接堆
        bsp_kout<< now << "kpoolmemmgr_t::kalloc: fallback to first_linekd_heap on processor " 
                      << query_x2apicid() << kendl;
    }
    
    // 使用第一个链接堆分配
    contain=first_linekd_heap.in_heap_alloc(ptr, size, flags);
    if (contain.result == result_code::SUCCESS) {
        no_succes_report = contain;
        return ptr;
    }
    
    fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_MEM;
    no_succes_report = fail;
    return nullptr;
}

void* kpoolmemmgr_t::realloc(void* ptr,KURD_t&no_succes_report, uint64_t size, alloc_flags_t flags)
{
    interrupt_guard g;
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_REALLOC;
    fail.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_REALLOC;
    fatal.event_code=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::EVENT_CODE_REALLOC;
    if (!ptr) return kalloc(size,no_succes_report, flags);
    if (size == 0) {
        kfree(ptr);
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::REALLOC_RESULTS::FAIL_RESONS::REASON_CODE_DEMAND_SIZE_IS_ZERO;
        no_succes_report = fail;
        return nullptr;
    }
    
    KURD_t subhcb_results_contianer = KURD_t();
    HCB_v2* located_private_heap=find_hcb_by_address(ptr);
    if(!located_private_heap)located_private_heap =(first_linekd_heap.is_addr_belong_to_this_hcb(ptr)?&first_linekd_heap:nullptr);
    if(!located_private_heap){
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::REALLOC_RESULTS::FAIL_RESONS::REASON_CODE_PTR_NOT_IN_ANY_HEAP;
        no_succes_report=fail;
        return nullptr;
    }
    if(!flags.is_when_realloc_force_new_addr)subhcb_results_contianer=located_private_heap->in_heap_realloc(ptr, size, flags);
    if (subhcb_results_contianer.result==result_code::FATAL&&subhcb_results_contianer.level==level_code::FATAL&&
        subhcb_results_contianer.in_module_location==MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB&&
        subhcb_results_contianer.event_code==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC&&
        subhcb_results_contianer.reason==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED) {
        handle_heap_obj_destroyed("realloc", ptr);
    }   
    if(subhcb_results_contianer.result == result_code::SUCCESS){
        no_succes_report=subhcb_results_contianer;
        return ptr;
    }
    if (is_muli_heap_enabled) {
        // 尝试分配新内存并复制数据
        KURD_t err_container = KURD_t();
        void* new_ptr = kalloc(size,err_container, flags);
        no_succes_report = err_container;
        if (new_ptr) {
            // 获取旧数据大小
            auto* old_meta = reinterpret_cast<HCB_v2::data_meta*>(
                reinterpret_cast<uint64_t>(ptr) - sizeof(HCB_v2::data_meta));
            ksystemramcpy(ptr, new_ptr, old_meta->data_size);
            kfree(ptr);
            return new_ptr;
        }
        if(!success_all_kurd(err_container)){
            no_succes_report=err_container;
        }else{
            fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::REALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_MEM;
            no_succes_report=fail;
        }
        return nullptr;
    } else {
        subhcb_results_contianer = first_linekd_heap.in_heap_realloc(ptr, size, flags);
        if (subhcb_results_contianer.result==result_code::FATAL&&subhcb_results_contianer.level==level_code::FATAL&&
            subhcb_results_contianer.in_module_location==MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB&&
        subhcb_results_contianer.event_code==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC&&
            subhcb_results_contianer.reason==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED) {
            handle_heap_obj_destroyed("realloc", ptr);
        }
    }
    
    if(subhcb_results_contianer.result == result_code::SUCCESS){
        no_succes_report=subhcb_results_contianer;
        return ptr;
    }
    if(!success_all_kurd(subhcb_results_contianer)){
        no_succes_report=subhcb_results_contianer;
    }else{
        fail.reason=MEMMODULE_LOCAIONS::KPOOLMEMMGR_EVENTS::REALLOC_RESULTS::FAIL_RESONS::REASON_CODE_NO_AVALIABLE_MEM;
        no_succes_report=fail;
    }
    return nullptr;
}

void kpoolmemmgr_t::clear(void* ptr)
{
    interrupt_guard g;
    if (!ptr) return;
    
    KURD_t subhcb_results_contianer = KURD_t();
    
    // 优先在每CPU堆中查找
    if (is_muli_heap_enabled) {
        HCB_v2* hcb = find_hcb_by_address(ptr);
        if (hcb) {
            subhcb_results_contianer = hcb->clear(ptr);
            if (subhcb_results_contianer.result==result_code::FATAL&&subhcb_results_contianer.level==level_code::FATAL&&
        subhcb_results_contianer.in_module_location==MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB&&
        subhcb_results_contianer.event_code==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC&&
        subhcb_results_contianer.reason==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED) {
                handle_heap_obj_destroyed("clear", ptr);
            }
            return;
        }
    }
    
    // 回退到第一个链接堆
    subhcb_results_contianer = first_linekd_heap.clear(ptr);
    if (subhcb_results_contianer.result==result_code::FATAL&&subhcb_results_contianer.level==level_code::FATAL&&
        subhcb_results_contianer.in_module_location==MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB&&
        subhcb_results_contianer.event_code==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC&&
        subhcb_results_contianer.reason==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED) {
        handle_heap_obj_destroyed("clear", ptr);
    }
}


void kpoolmemmgr_t::kfree(void* ptr)
{
    interrupt_guard g;
    if (!ptr) return;
    
    KURD_t subhcb_results_contianer = KURD_t();
    
    if (is_muli_heap_enabled) {
        auto* hcb = find_hcb_by_address(ptr);
        if (hcb) {
            subhcb_results_contianer = hcb->free(ptr);
            if (subhcb_results_contianer.result==result_code::FATAL&&subhcb_results_contianer.level==level_code::FATAL&&
        subhcb_results_contianer.in_module_location==MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB&&
        subhcb_results_contianer.event_code==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE&&
        subhcb_results_contianer.reason==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED) {
                handle_heap_obj_destroyed("kfree", ptr);
            }
            
            // 如果HCB为空，则销毁它
            if (hcb->get_used_bytes_count() == 0) {
                uint64_t offset=(uint64_t)ptr-(uint64_t)heap_area.start;
                subhcb_results_contianer=free_heap(offset/HCB_DEFAULT_SIZE);
                if(!success_all_kurd(subhcb_results_contianer)){
                    //panic,释放失败
                }
            }
            return;
        }
    }
    
    // 回退到第一个链接堆
    subhcb_results_contianer = first_linekd_heap.free(ptr);
    if (subhcb_results_contianer.result==result_code::FATAL&&subhcb_results_contianer.level==level_code::FATAL&&
        subhcb_results_contianer.in_module_location==MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB&&
        subhcb_results_contianer.event_code==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC&&
        subhcb_results_contianer.reason==MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED) {
        handle_heap_obj_destroyed("kfree", ptr);
    }
}

// 地址转换辅助函数
namespace {
    constexpr uint64_t get_min_kvaddr() {
        #ifdef PGLV_5
        return 0xff00000000000000;
        #elif defined(PGLV_4)
        return 0xffff800000000000;
        #else
        return 0;
        #endif
    }
    
    constexpr uint64_t get_max_phyaddr() {
        #ifdef PGLV_5
        return 1ULL << 56;
        #elif defined(PGLV_4)
        return 1ULL << 47;
        #else
        return 0;
        #endif
    }
}

phyaddr_t kpoolmemmgr_t::get_phy(vaddr_t addr)
{
    interrupt_guard g;
    constexpr uint64_t MIN_KVADDR = get_min_kvaddr();
    constexpr uint64_t MAX_PHYADDR = get_max_phyaddr();
    
    if (addr < MIN_KVADDR && addr >= MAX_PHYADDR) {
        return 0;
    }
    
    if (is_muli_heap_enabled) {
        auto* hcb = find_hcb_by_address(reinterpret_cast<void*>(addr));
        if (hcb) {
            return hcb->tran_to_phy(reinterpret_cast<void*>(addr));
        }
    }
    
    return first_linekd_heap.tran_to_phy(reinterpret_cast<void*>(addr));
}

vaddr_t kpoolmemmgr_t::get_virt(phyaddr_t addr)
{
    interrupt_guard g;
    constexpr uint64_t MIN_KVADDR = get_min_kvaddr();
    constexpr uint64_t MAX_PHYADDR = get_max_phyaddr();
    
    if (addr < MIN_KVADDR || addr >= MAX_PHYADDR) {
        return 0;
    }
    
    if (is_muli_heap_enabled) {
        auto* hcb = find_hcb_by_address(reinterpret_cast<void*>(addr));
        if (hcb) {
            return hcb->tran_to_virt(addr);
        }
    }
    
    return first_linekd_heap.tran_to_virt(addr);
}

kpoolmemmgr_t::kpoolmemmgr_t() = default;
kpoolmemmgr_t::~kpoolmemmgr_t() = default;
