#include "memory/AddresSpace.h"
#include "memory/memory_base.h"
#include "memory/all_pages_arr.h"
#include "memory/kpoolmemmgr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/phyaddr_accessor.h"
#include "abi/os_error_definitions.h"
#include "linker_symbols.h"
#include "util/OS_utils.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "panic.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "util/kptrace.h"
#include "util/kout.h"
#include "memory/init_memory_info.h"
#include "firmware/ACPI_APIC.h"
#ifdef USER_MODE
#include <elf.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
KspacePageTableStatisitcs*kspace_pagetable_statistics;
namespace {
static inline KspacePageTableStatisitcs* kpt_stat_for_current_cpu()
{
    if (kspace_pagetable_statistics == nullptr || logical_processor_count == 0) {
        return nullptr;
    }
    uint32_t pid = fast_get_processor_id();
    if (pid >= logical_processor_count) {
        pid = pid % logical_processor_count;
    }
    return &kspace_pagetable_statistics[pid];
}
}
void invalidate_tlb_by_vaddr(vaddr_t vaddr){
    asm volatile("invlpg (%0)" ::"r"(vaddr) : "memory");
}
PageTableEntryUnion*KspacePageTable::kspaceUPpdpt;
shared_inval_VMentry_info_t shared_inval_kspace_VMentry_info;
KURD_t KspacePageTable::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_KSPACE_MAP_MGR,0,0,err_domain::CORE_MODULE);
}
KURD_t KspacePageTable::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=result_code::SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}
KURD_t KspacePageTable::default_failure()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}
KURD_t KspacePageTable::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}
KURD_t KspacePageTable::Init(loaded_VM_interval* kspace_up_layer)
{
    KURD_t success=default_success();
    KURD_t fail=default_failure();
  success.event_code=MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INIT;
    fail.event_code=MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INIT;
    kspace_uppdpt_phyaddr=kspace_up_layer->pbase;
    kspaceUPpdpt=(PageTableEntryUnion*)kspace_up_layer->vbase;
    kspace_vm_table=new kspace_vm_table_t();
    KURD_t status=KURD_t();
    uint64_t basic_seg_size=PhyAddrAccessor::BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG;
    uint64_t intervals_count=VM_intervals_count;
    loaded_VM_interval*interval_arr=VM_intervals;
    kspace_pagetable_statistics=new KspacePageTableStatisitcs[logical_processor_count];
    for(uint64_t i=0;i<intervals_count;i++){
        if(interval_arr[i].vbase>=PAGELV4_KSPACE_BASE){
            vm_interval interval{
                .vbase = interval_arr[i].vbase,
                .pbase = interval_arr[i].pbase,
                .size = interval_arr[i].size,
                .access = interval_arr[i].access
            };
            status = enable_VMentry(interval);
            if(error_kurd(status)){
                return status;
            }
            VM_DESC desc{
                .start = interval.vbase,
                .end = interval.vbase + interval.size,
                .map_type = VM_DESC::MAP_PHYSICAL,
                .phys_start = interval.pbase,
                .access = interval.access,
                .committed_full = 1,
                .is_vaddr_alloced = 0,
                .is_out_bound_protective = 0,
                .SEG_SIZE_ONLY_UES_IN_BASIC_SEG = 0
            };
            kspace_vm_table->insert(desc);
        }
    }
    kspace_vm_table->all_node_print();

    vaddr_t basic_vaddr = kspace_vm_table->alloc_available_space(basic_seg_size, 0);
    if (basic_vaddr == 0) {
        return fail;
    }
    vm_interval basic_interval{
        .vbase = basic_vaddr,
        .pbase = 0,
        .size = basic_seg_size,
        .access = PG_RW
    };
    status = enable_VMentry(basic_interval);
    if (error_kurd(status)) {
        return status;
    }
    PhyAddrAccessor::BASIC_DESC = VM_DESC{
        .start = basic_vaddr,
        .end = basic_vaddr + basic_seg_size,
        .map_type = VM_DESC::MAP_PHYSICAL,
        .phys_start = 0,
        .access = PG_RW,
        .committed_full = 1,
        .is_vaddr_alloced = 1,
        .is_out_bound_protective = 0,
        .SEG_SIZE_ONLY_UES_IN_BASIC_SEG = basic_seg_size
    };
    kspace_vm_table->insert(PhyAddrAccessor::BASIC_DESC);

    return success;

}
void nonleaf_pgtbentry_flagsset(PageTableEntryUnion&entry){//内核态的是全局的，
    entry.raw=0;
    entry.pte.present=1;
    entry.pte.RWbit=1;
    entry.pte.KERNELbit=0;
}
/**
 * 锁在外部接口中持有
 */
KURD_t KspacePageTable::_4lv_pte_4KB_entries_set(
    phyaddr_t phybase,
    vaddr_t vaddr_base,
    uint16_t count,
    pgaccess access
    )
{//暂时只有(is_phypgsmgr_enabled==false）的逻辑
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->pages_set.no_leaf_entry_set += count;
        stat->pages_set.specific.x86_64.PTE_set_count += count;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;

    // 检查count参数有效性
    if (count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BAD_COUNT;
        return fail;
    }

    uint64_t highoffset = vaddr_base - PAGELV4_KSPACE_BASE;
    
    // 使用常量替代魔法数字，提高可读性
    const uint32_t PDPT_INDEX_BITS = 17;
    const uint32_t PD_INDEX_BITS = 9;
    const uint32_t PT_INDEX_BITS = 9;

    uint32_t pdpte_index = (highoffset >> 30) & ((1 << PDPT_INDEX_BITS) - 1);
    uint16_t pde_index = (highoffset >> 21) & ((1 << PD_INDEX_BITS) - 1);
    uint16_t pte_index = (highoffset >> 12) & ((1 << PT_INDEX_BITS) - 1);

    PageTableEntryUnion& pdpte = kspaceUPpdpt[pdpte_index];
    if(pdpte.raw & PDPTE::PS_MASK){
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_WHEN_GET_SUB;
        return fail;
    }
    // 检查页目录指针表项是否存在且不是1GB页面
    if (!(pdpte.raw & PageTableEntry::P_MASK) ){
        nonleaf_pgtbentry_flagsset(pdpte);
        KURD_t kurd;
        phyaddr_t pd_phyaddr = FreePagesAllocator::alloc(_4KB_SIZE,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_pinned,kurd);
        if (pd_phyaddr == 0||kurd.result!=result_code::SUCCESS) return kurd;
        pdpte.pdpte.PD_addr = pd_phyaddr >> 12;
        // 初始化新分配的页目录中的所有页表项为0
        for(uint16_t i=0;i<512;i++)
        {
            PhyAddrAccessor::writeu64((pd_phyaddr & PHYS_ADDR_MASK) + sizeof(PageTableEntryUnion) * i, 0);
        }
    }
    phyaddr_t pde_loacte_phyaddr = (pdpte.pdpte.PD_addr << 12) +sizeof(PageTableEntryUnion) * pde_index;
    uint64_t pderaw = PhyAddrAccessor::readu64(pde_loacte_phyaddr);
    PageTableEntryUnion pde = {
        .raw = pderaw
    };
    if(pderaw & PDE::PS_MASK){
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDE_WHEN_GET_SUB;
        return fail;
    }
    // 检查页目录项是否存在且不是2MB页面
    if (!(pderaw & PageTableEntry::P_MASK))
        {
            nonleaf_pgtbentry_flagsset(pde);
            KURD_t kurd;
            phyaddr_t pt_phyaddr = FreePagesAllocator::alloc(_4KB_SIZE,BUDDY_ALLOC_ALWAYS_TRY,page_state_t::kernel_pinned,kurd);
            if (pt_phyaddr == 0||kurd.result!=result_code::SUCCESS) return kurd;
            pde.pde.pt_addr = pt_phyaddr >> 12;
            PhyAddrAccessor::writeu64(pde_loacte_phyaddr, pde.raw);
            // 初始化新分配的页表中的所有页表项为0
            for(uint16_t i=0;i<512;i++)
            {
                PhyAddrAccessor::writeu64((pt_phyaddr & PHYS_ADDR_MASK) + sizeof(PageTableEntryUnion) * i, 0);
            }
        }

    phyaddr_t pte_phybase = (pde.pde.pt_addr << 12) & PHYS_ADDR_MASK;
    cache_table_idx_struct_t idx = cache_strategy_to_idx(access.cache_strategy);

    PageTableEntryUnion template_entry = {
        .raw = 0
    };

    template_entry.pte.EXECUTE_DENY = !access.is_executable;
    template_entry.pte.KERNELbit = !access.is_kernel;
    template_entry.pte.PAT = idx.PAT;
    template_entry.pte.RWbit = access.is_writeable;
    template_entry.pte.global = 1;
    template_entry.pte.present = 1;
    template_entry.pte.PCD = idx.PCD;
    template_entry.pte.PWT = idx.PWT;
    for (uint16_t i = 0; i < count; i++) {
        uint64_t pte_offset = sizeof(PageTableEntryUnion) * (i + pte_index);
        uint64_t pte_value = template_entry.raw + phybase + i * _4KB_SIZE;
        PhyAddrAccessor::writeu64(pte_phybase + pte_offset, pte_value);
    }

    return success;
}
// ====================================================================
void KspacePageTable::invalidate_seg()
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->invalidate_tlb_count++;
    }
    interrupt_guard g;
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INVALIDATE_TLB;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INVALIDATE_TLB;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_INVALIDATE_TLB;
    constexpr uint32_t _4KB_SIZE = 0x1000;
    constexpr uint32_t _2MB_SIZE = 1ULL << 21;
    constexpr uint32_t _1GB_SIZE = 1ULL << 30;
    panic_info_inshort in_short{
        .is_bug =true,
        .is_policy =true,
        .is_hw_fault=false,
        .is_mem_corruption=false,
        .is_escalated=false,
    };
    if (shared_inval_kspace_VMentry_info.is_package_valid == false) {
        bsp_kout<<"[KERNEL] invalid_kspace_VMentry_handler: stared_inval_kspace_VMentry_info is invalid"<<kendl;
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::INVALIDATE_TLB_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VM_ENTRY;
        Panic::panic(default_panic_behaviors_flags,
            "invalid_k space_VMentry_handler: stared_inval_kspace_VMentry_info is invalid",
        nullptr,&in_short,fail);
        return;
    }
    
    for (uint8_t i = 0; i < 5; i++) {
        seg_to_pages_info_pakage_t::pages_info_t& entry = 
            shared_inval_kspace_VMentry_info.info_package.entryies[i];
            
        if (entry.page_size_in_byte == 0 || entry.num_of_pages == 0) 
            continue;
            
        switch (entry.page_size_in_byte) {
            case _4KB_SIZE:
                for (uint32_t j = 0; j < entry.num_of_pages; j++) {
                    invalidate_tlb_by_vaddr(entry.vbase + j * _4KB_SIZE);
                }
                break;
                
            case _2MB_SIZE: 
                for (uint32_t j =0; j < entry.num_of_pages; j++) {
                    invalidate_tlb_by_vaddr(entry.vbase + j * _2MB_SIZE);
                }
                break;
                
            case _1GB_SIZE:
                for (uint32_t j = 0; j < entry.num_of_pages; j++) {
                    invalidate_tlb_by_vaddr(entry.vbase + j * _1GB_SIZE);
                }
                break;
                
            default:
                bsp_kout<< "[KERNEL] invalid_kspace_VMentry_handler: invalid page size in kspace_VMentry_info" << kendl;
                asm volatile("sti");
                fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::INVALIDATE_TLB_RESULTS::FATAL_REASONS::REASON_CODE_INVALID_PAGE_SIZE;
                Panic::panic(default_panic_behaviors_flags,
            "invalid_k space_VMentry_handler: stared_inval_kspace_VMentry_info is invalid",
        nullptr,&in_short,fatal);
                return;  // 添加 return 避免继续执行
        }
    }
    
    // 正确的原子递增
    shared_inval_kspace_VMentry_info.completed_processors_count++;

}
// 2MB 大页映射（PDE 级别）
// 要求：不能跨 PDPT 边界（即一次最多映射 512 个 2MB 页，覆盖 1GB）
// ====================================================================
KURD_t KspacePageTable::_4lv_pde_2MB_entries_set(
                                                phyaddr_t phybase,
                                                  vaddr_t vaddr_base,
                                                  uint16_t count,
                                                  pgaccess access
                                                  )
{//暂时只有(is_phypgsmgr_enabled==false）的逻辑
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->pages_set.no_leaf_entry_set += count;
        stat->pages_set.specific.x86_64.PDE_HUGE_set_count += count;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;

    // 检查count参数有效性
    if (count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BAD_COUNT;
        return fail;
    }
    
    if (count > 512) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE;
        return fail;
    }
    
    // 检查物理地址和虚拟地址是否按照2MB对齐
    if ((phybase & (_2MB_SIZE - 1)) || (vaddr_base & (_2MB_SIZE - 1))) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BASE_NOT_ALIGNED;
        return fail;
    }

    uint64_t highoffset = vaddr_base - PAGELV4_KSPACE_BASE;
    
    // 使用常量替代魔法数字，提高可读性
    const uint32_t PDPT_INDEX_BITS = 17;
    const uint32_t PD_INDEX_BITS = 9;

    uint32_t pdpte_index = (highoffset >> 30) & ((1ULL << PDPT_INDEX_BITS) - 1);
    uint16_t pde_index = (highoffset >> 21) & ((1 << PD_INDEX_BITS) - 1);

    // 检查是否跨越PDPT边界
    if (pde_index + count > 512) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE;
        return fail;
    }

    PageTableEntryUnion& pdpte = kspaceUPpdpt[pdpte_index];
    
    // 检查页目录指针表项是否存在且不是1GB页面
    if (pdpte.raw & PDPTE::PS_MASK) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_WHEN_GET_SUB;
        return fail;
    }
    
    // 如果页目录指针表项不存在，则创建新的页目录
    if (!(pdpte.raw & PageTableEntry::P_MASK)) {
        nonleaf_pgtbentry_flagsset(pdpte);
        KURD_t kurd;
        phyaddr_t pd_phyaddr = FreePagesAllocator::alloc(_4KB_SIZE,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_pinned,kurd);
        if (pd_phyaddr == 0||kurd.result!=result_code::SUCCESS) return kurd;
        pdpte.pdpte.PD_addr = pd_phyaddr >> 12;
        // 初始化新分配的页目录中的所有页表项为0
        for(uint16_t i=0;i<512;i++)
        {
            PhyAddrAccessor::writeu64((pd_phyaddr & PHYS_ADDR_MASK) + sizeof(PageTableEntryUnion) * i, 0);
        }
    }

    // 修复位运算优先级问题：使用括号确保正确的运算顺序
    phyaddr_t pd_phyaddr = pdpte.pdpte.PD_addr << 12;
    cache_table_idx_struct_t idx = cache_strategy_to_idx(access.cache_strategy);

    PageTableEntryUnion template_entry = {
        .raw = 0
    };

    template_entry.pde2MB.present      = 1;
    template_entry.pde2MB.RWbit        = access.is_writeable;
    template_entry.pde2MB.KERNELbit    = !access.is_kernel;
    template_entry.pde2MB.large        = 1;        // 必须为 1
    template_entry.pde2MB.global       = access.is_global;
    template_entry.pde2MB.PAT          = idx.PAT;
    template_entry.pde2MB.PWT          = idx.PWT;
    template_entry.pde2MB.PCD          = idx.PCD;
    template_entry.pde2MB.EXECUTE_DENY = !access.is_executable;

    for (uint16_t i = 0; i < count; i++) {
        uint64_t pde_offset = sizeof(PageTableEntryUnion) * (pde_index + i);
        template_entry.pde2MB._2mb_Addr = ((phybase >> 21) + i) & ((1ULL << 27) - 1);
        PhyAddrAccessor::writeu64(pd_phyaddr + pde_offset, template_entry.raw);
    }

    return success;
}
// ====================================================================
// 1GB 大页映射（PDPTE 级别）
// 要求：不能跨 PML4E 边界（即一次最多映射 512 个 1GB 页，覆盖 512GB）
// ====================================================================
KURD_t KspacePageTable::_4lv_pdpte_1GB_entries_set(phyaddr_t phybase,
                                                    vaddr_t vaddr_base,
                                                    uint16_t count,
                                                    pgaccess access)
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->pages_set.no_leaf_entry_set += count;
        stat->pages_set.specific.x86_64.PDPTE_HUGE_set_count += count;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_SET;

    if (count == 0){
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BAD_COUNT;
        return fail;
    }
    if ((phybase & (_1GB_SIZE - 1)) || (vaddr_base & (_1GB_SIZE - 1))) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BASE_NOT_ALIGNED;
        return fail; // 1GB 对齐
    }

    uint64_t highoffset = vaddr_base - PAGELV4_KSPACE_BASE;
    uint32_t pdpt_index = (highoffset >> 30) & ((1ULL << 17) - 1);  // 256×512 合并表索引
    uint16_t start_idx  = (highoffset >> 30) & 0x1FF;               // 在该 PDPTE 表内的起始偏移
    cache_table_idx_struct_t idx = cache_strategy_to_idx(access.cache_strategy);

    for (uint16_t i = 0; i < count; i++) {
        PageTableEntryUnion& entry = kspaceUPpdpt[pdpt_index + i];
        if (entry.raw & PageTableEntry::P_MASK) [[unlikely]] {
            fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_SET_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_WHEN_GET_SUB;
            return fail;
        }
        PDPTEEntry1GB& huge = entry.pdpte1GB;
        huge.present       = 1;
        huge.RWbit         = access.is_writeable;
        huge.KERNELbit     = !access.is_kernel;
        huge.large         = 1;        // 必须为 1
        huge.global        = access.is_global;
        huge.PAT           = idx.PAT;
        huge.PWT           = idx.PWT;
        huge.PCD           = idx.PCD;
        huge.EXECUTE_DENY  = !access.is_executable;
        huge._1GB_Addr     = ((phybase >> 30) + i) & ((1ULL << 18) - 1); // 低 18 位是地址高部
    }
    return success;
}

KURD_t KspacePageTable::_4lv_pte_4KB_entries_clear(vaddr_t vaddr_base, uint16_t count)
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->pages_clear.no_leaf_entry_clear += count;
        stat->pages_clear.specific.x86_64.PTE_set_count += count;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;

    if (count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BAD_COUNT;
        return fail;
    }

    uint64_t highoffset = vaddr_base - PAGELV4_KSPACE_BASE;
    uint32_t pdpte_index = (highoffset >> 30) & ((1 << 17) - 1);
    uint16_t pde_index = (highoffset >> 21) & ((1 << 9) - 1);
    uint16_t pte_index = (highoffset >> 12) & ((1 << 9) - 1);

    if (pte_index + count > 512) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE;
        return fail;
    }
    if (vaddr_base % _4KB_SIZE) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BASE_NOT_ALIGNED;
        return fail;
    }

    PageTableEntryUnion& pdpte = kspaceUPpdpt[pdpte_index];
    if (!(pdpte.raw & PageTableEntry::P_MASK)) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_SUBTABLE_NOT_EXIST;
        return fail;
    }
    if (pdpte.raw & PDPTE::PS_MASK) {
        fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_UNTIMELY;
        return fatal;
    }

    // 修复位运算优先级问题：使用括号确保正确的运算顺序
    uint64_t pd_addr = pdpte.pdpte.PD_addr;
    uint64_t pde_offset = sizeof(PageTableEntryUnion) * pde_index;
    uint64_t pde_address = (pd_addr << 12) + pde_offset;
    uint64_t pderaw = PhyAddrAccessor::readu64(pde_address);
    
    if (!(pderaw & PageTableEntry::P_MASK)) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDE_SUBTABLE_NOT_EXIST;
        return fail;
    }
    
    if (pderaw & PDE::PS_MASK) {
        fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDE_UNTIMELY;
        return fatal;
    }
    
    PageTableEntryUnion pde = { .raw = pderaw };
    phyaddr_t pt_phyaddr = (pde.pde.pt_addr << 12) & PHYS_ADDR_MASK;

    // 清除指定范围 PTE
    for (uint16_t i = 0; i < count; i++) {
        uint64_t pte_offset = sizeof(PageTableEntryUnion) * (pte_index + i);
        PhyAddrAccessor::writeu64(pt_phyaddr + pte_offset, 0);
        invalidate_tlb_by_vaddr(vaddr_base + ((pte_index + i) << 12)); // 4KB 步长
    }

    // 检查整个 PT 是否全空（不限 may_full_del）
    bool is_full_del = true;
    for (uint16_t i = 0; i < 512; i++) {
        uint64_t pte_offset = sizeof(PageTableEntryUnion) * i;
        uint64_t pte_value = PhyAddrAccessor::readu64(pt_phyaddr + pte_offset);
        if (pte_value & PageTableEntry::P_MASK) {
            is_full_del = false;
            break;
        }
    }

    if (is_full_del) {
        FreePagesAllocator::free(pt_phyaddr, 1<<12);
        // 清除PDE项
        PhyAddrAccessor::writeu64(pde_address, 0);
    }

    return success;
}
// ====================================================================
// 1GB 大页清除（PDPTE 级别）
// 功能：直接清除 kspaceUPpdpt 中的 PDPTE 条目（无需下级页表）
// ====================================================================
KURD_t KspacePageTable::_4lv_pdpte_1GB_entries_clear(vaddr_t vaddr_base, uint16_t count)
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->pages_clear.no_leaf_entry_clear += count;
        stat->pages_clear.specific.x86_64.PDPTE_HUGE_set_count += count;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;

    if (count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BAD_COUNT;
        return fail;
    }
    if (vaddr_base & (_1GB_SIZE - 1)) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BASE_NOT_ALIGNED;
        return fail;
    }

    uint64_t highoffset = vaddr_base - PAGELV4_KSPACE_BASE;
    uint32_t pdpt_index = (highoffset >> 30) & ((1ULL << 17) - 1);  // 合并索引
    uint16_t start_idx  = (highoffset >> 30) & 0x1FF;               // 在 512 项中的起始位置

    if (start_idx + count > 512) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE;
        return fail;
    }

    for (uint16_t i = 0; i < count; i++) {
        PageTableEntryUnion& entry = kspaceUPpdpt[pdpt_index + i];
        if (!(entry.raw & PageTableEntry::P_MASK)) {
            continue;  // 已空
        }
        if (!(entry.raw & PDPTE::PS_MASK)) {
            fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_NOT_EXIST;
            return fatal;
        }
        entry.raw = 0;
        invalidate_tlb_by_vaddr(vaddr_base + ((pdpt_index + i) << 30)); // 1GB 步长
    }

    // 注意：1GB 页清除后不需要回收下级页表（因为 PS=1 时没有下级）
    // 也不回收整个 512GB 块（内核空间一般保留结构）

    return success;
}
// ====================================================================
// 2MB 大页清除（PDE 级别）
// 要求：不能跨 PDPT 边界（即一次最多清除 512 个 2MB 页，覆盖 1GB）
// 功能：清除 PDE 条目 + 检测整个 PD 是否全空 → 回收 PD 页表 + 清上级 PDPTE
// ====================================================================
KURD_t KspacePageTable::_4lv_pde_2MB_entries_clear(vaddr_t vaddr_base, uint16_t count)
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->pages_clear.no_leaf_entry_clear += count;
        stat->pages_clear.specific.x86_64.PDE_HUGE_set_count += count;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_PAGES_CLEAR;

    if (count == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BAD_COUNT;
        return fail;
    }
    if (count > 512) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE;
        return fail;
    }
    if (vaddr_base & (_2MB_SIZE - 1)) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_BASE_NOT_ALIGNED;
        return fail;
    }

    uint64_t highoffset = vaddr_base - PAGELV4_KSPACE_BASE;
    uint32_t pdpt_index = (highoffset >> 30) & ((1ULL << 17) - 1);  // 合并 PDPT 索引
    uint16_t pde_index  = (highoffset >> 21) & 0x1FF;               // 在 PD 内的起始索引

    if (pde_index + count > 512) [[unlikely]] {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FAIL_REASONS::REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE;
        return fail;
    }

    // 获取或验证 PD 页表
    PageTableEntryUnion& pdpte = kspaceUPpdpt[pdpt_index];
    if (!(pdpte.raw & PageTableEntry::P_MASK)) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_SUBTABLE_NOT_EXIST;
        return fail;
    }
    if (pdpte.raw & PDPTE::PS_MASK) {
        fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDPTE_UNTIMELY;
        return fatal;
    }

    phyaddr_t pd_phyaddr = (pdpte.pdpte.PD_addr << 12) & PHYS_ADDR_MASK;

    // 清除指定范围的 2MB PDE 条目
    for (uint16_t i = 0; i < count; i++) {
        uint64_t pde_offset = sizeof(PageTableEntryUnion) * (pde_index + i);
        uint64_t pde_value = PhyAddrAccessor::readu64(pd_phyaddr + pde_offset);
        
        if (!(pde_value & PageTableEntry::P_MASK)) {
            // 已经为空，继续（允许重复清除）
            continue;
        }
        if (!(pde_value & PDE::PS_MASK)) {
            fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::PAGES_CLEAR_RESULTS_CODE::FATAL_REASONS::REASON_CODE_HUGE_PDE_NOT_EXIST;
            return fatal;
        }
        PhyAddrAccessor::writeu64(pd_phyaddr + pde_offset, 0);
        invalidate_tlb_by_vaddr(vaddr_base + ((pde_index + i) << 21)); // 2MB 步长
    }

    // 检查整个 PD 是否完全为空 → 回收 PD 页表页
    bool pd_empty = true;
    for (uint16_t i = 0; i < 512; i++) {
        uint64_t pde_offset = sizeof(PageTableEntryUnion) * i;
        uint64_t pde_value = PhyAddrAccessor::readu64(pd_phyaddr + pde_offset);
        if (pde_value & PageTableEntry::P_MASK) {
            pd_empty = false;
            break;
        }
    }

    if (pd_empty) {
        FreePagesAllocator::free(pd_phyaddr, 1<<12);
        pdpte.raw = 0;  // 清上级 PDPTE
        // 注意：这里不需要递归检查 PDPTE 是否全空，因为内核空间一般不回收整个 512GB 块
    }

    return success;
}
KURD_t KspacePageTable::v_to_phyaddrtraslation_entry
(vaddr_t vaddr, 
    PageTableEntryUnion &result, 
    uint32_t &page_size)
{
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_TRAN_TO_PHY_ENTRY;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_TRAN_TO_PHY_ENTRY;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_TRAN_TO_PHY_ENTRY;

    if(pglv_4_or_5){
        uint64_t highoffset = vaddr - PAGELV4_KSPACE_BASE;
        uint32_t pdpte_index = (highoffset >> 30) & ((1 << 17) - 1);
        uint16_t pde_index = (highoffset >> 21) & ((1 << 9) - 1);
        uint16_t pte_index = (highoffset >> 12) & ((1 << 9) - 1);
        
        // 检查PDPTE
        PageTableEntryUnion& pdpte = kspaceUPpdpt[pdpte_index];
        if (!(pdpte.raw & PageTableEntry::P_MASK)) {
            fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::TRAN_TO_PHY_ENTRY_RESULTS_CODE::FAIL_REASONS::REASON_CODE_NOT_PRESENT_ENTRY;
            return fail;
        }
        
        // 1GB 页面
        if (pdpte.raw & PDPTE::PS_MASK) {
            result = pdpte;
            page_size = _1GB_SIZE;
            return success;
        }
        
        // 获取PD物理地址
        phyaddr_t pd_phyaddr = (pdpte.pdpte.PD_addr << 12) & PHYS_ADDR_MASK;
        uint64_t pde_offset = sizeof(PageTableEntryUnion) * pde_index;
        uint64_t pde_address = pd_phyaddr + pde_offset;
        uint64_t pde_raw = PhyAddrAccessor::readu64(pde_address);
        
        // 检查PDE
        if (!(pde_raw & PageTableEntry::P_MASK)) {
            fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::TRAN_TO_PHY_ENTRY_RESULTS_CODE::FAIL_REASONS::REASON_CODE_NOT_PRESENT_ENTRY;
            return fail;
        }
        
        PageTableEntryUnion pde = { .raw = pde_raw };
        
        // 2MB 页面
        if (pde_raw & PDE::PS_MASK) {
            result = pde;
            page_size = _2MB_SIZE;
            return success;
        }
        
        // 获取PT物理地址
        phyaddr_t pt_phyaddr = (pde.pde.pt_addr << 12) & PHYS_ADDR_MASK;
        uint64_t pte_offset = sizeof(PageTableEntryUnion) * pte_index;
        uint64_t pte_address = pt_phyaddr + pte_offset;
        uint64_t pte_raw = PhyAddrAccessor::readu64(pte_address);
        
        // 设置结果
        page_size = _4KB_SIZE;
        result.raw = pte_raw;
        return success;
    }else{
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::TRAN_TO_PHY_ENTRY_RESULTS_CODE::FAIL_REASONS::REASON_CODE_NOT_SUPPORT_LV5_PAGING;
        return fail;
    }
    fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::TRAN_TO_PHY_ENTRY_RESULTS_CODE::FATAL_REASONS::REASON_CODE_UNREACHABLE_CODE;
    return fatal;
}
