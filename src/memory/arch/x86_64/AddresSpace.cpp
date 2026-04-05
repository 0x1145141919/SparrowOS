#include "memory/AddresSpace.h"
#include "memory/memory_base.h"
#include "memory/all_pages_arr.h"
#include "memory/kpoolmemmgr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/phyaddr_accessor.h"    
#include "linker_symbols.h"
#include "util/OS_utils.h"
#include "util/kout.h"
AddressSpace*gKernelSpace;
KURD_t AddressSpace::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_ADDRESSPACE,0,0,err_domain::CORE_MODULE);
}
KURD_t AddressSpace::default_success()
{
    KURD_t success=default_kurd();
    success.result=result_code::SUCCESS;
    success.level=level_code::INFO;
    return success;
}
KURD_t AddressSpace::default_fail()
{
    KURD_t fail=default_kurd();
    fail=set_result_fail_and_error_level(fail);
    return fail;
}
KURD_t AddressSpace::default_fatal()
{
    KURD_t fatal=default_kurd();
    fatal=set_fatal_result_level(fatal);
    return fatal;
}
KURD_t AddressSpace::invalidate_tlb_of_VM_desc(VM_DESC desc, tlb_invalidate_flags flags)
{
    KURD_t success=default_success();
   KURD_t fail=default_fail();
   KURD_t fatal=default_fatal();
   success.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_INVALIDATE_TLB;
   fail.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_INVALIDATE_TLB;
   fatal.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_INVALIDATE_TLB;
    seg_to_pages_info_pakage_t info_package;
    int stauts=vm_interval_to_pages_info(info_package,desc);
    if(stauts){
        fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
        return fail;
    }
    auto x64_invalidate_tlb=[flags](vaddr_t vaddr){
    if(flags.if_not_currunt_space){
        // 刷新其他地址空间的TLB（使用PCID和INVPCID指令）
        if(flags.if_hardware_addresspace_id_valid){
            // 使用INVPCID指令刷新指定PCID的TLB项
            // INVPCID type 0: 使单个PCID的单个地址无效
            struct {
                uint64_t pcid;
                uint64_t vaddr;
            } descriptor = {
                .pcid = flags.hardware_addresspace_id & 0xFFF,
                .vaddr = vaddr
            };
            
            asm volatile(
                "invpcid (%1), %0"
                : 
                : "r"(0ULL),  // type=0: INVPCID_TYPE_INDIV_ADDR
                  "r"(&descriptor)
                : "memory"
            );
        } else {
            // 如果没有指定有效的PCID，回退到普通的INVLPG
            asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
        }
    } else {
        // 刷新当前地址空间的TLB
        asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    }
};
    for(int i=0;i<5;i++){
        switch(info_package.entryies[i].page_size_in_byte){
            case _4KB_SIZE:
            case _2MB_SIZE:
            case _1GB_SIZE:
            {
                seg_to_pages_info_pakage_t::pages_info_t& entry=info_package.entryies[i];
                for(uint64_t i=0;i<entry.num_of_pages;i++){
                    vaddr_t vaddr=entry.vbase+i*entry.page_size_in_byte;
                    x64_invalidate_tlb(vaddr);
                }
            }
            default:
            {
                fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::INVALIDATE_TLB_RESULTS::FATAL_REASONS::REASON_CODE_INVALID_PAGE_SIZE;
                return fatal;
            }   
        }
    }
    return success;
}
AddressSpace::AddressSpace()
{
}

KURD_t AddressSpace::enable_VM_desc(VM_DESC desc)    
{
    constexpr uint16_t ILLEAGLE_PAGES_COUNT=0x1;
    constexpr uint16_t PAGES_COUNT_AND_BASE_OUT_OF_RANGE=0x2;
    constexpr uint16_t NOT_ALIGNED_INPUT_BASE=0x3;
    constexpr uint16_t TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY=0x4;
    
    /**
     * 先搞参数检验
     */
   KURD_t success=default_success();
   KURD_t fail=default_fail();
   KURD_t fatal=default_fatal();
   success.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_ENABLE_VMENTRY;
   fail.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_ENABLE_VMENTRY;
   fatal.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_ENABLE_VMENTRY;
   bool is_reach_va_bottom=false;
   if(desc.start<ADDR_VM_BOTTOM){
    uint32_t gap=ADDR_VM_BOTTOM-desc.start;
    desc.start=ADDR_VM_BOTTOM;
    desc.phys_start+=gap;
    is_reach_va_bottom=true;
   }
   if(
        desc.start>=desc.end||
        desc.end>(pglv_4_or_5?PAGE_LV4_USERSPACE_SIZE:PAGE_LV5_USERSPACE_SIZE)||
        desc.start%_4KB_SIZE||
        desc.end%_4KB_SIZE||
        desc.end%_4KB_SIZE
    ){
    fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
    return fail;}
    pgaccess desc_access=desc.access;
    cache_table_idx_struct_t atompages_cache_table_idx=cache_strategy_to_idx(desc_access.cache_strategy);
    phyaddr_t pml4tb_phyaddr_base=pml4_phybase;
    
    /**
     * 
     * int _4lv_pdpte_1GB_entries_set(phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count,pgaccess access);//这里要求的是不能跨pml4e边界
    int _4lv_pde_2MB_entries_set(phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count,pgaccess access);//这里要求的是不能跨页目录指针边界
    int _4lv_pte_4KB_entries_set(phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count,pgaccess access);//这里要求的是不能跨页
    三个工具匿名函数，用于设置4级页表项，返回值是错误码 
    */
    // 匿名函数替代原来的get_sub_tb函数
    KURD_t pages_alloc_event_kurd=KURD_t();
    auto get_sub_tb = [&pages_alloc_event_kurd](PageTableEntryUnion& entry, PageTableEntryType Clevel) -> phyaddr_t {
        KURD_t contain;
        if(Clevel==PageTableEntryType::PT) return 0;
        constexpr uint32_t _4KB_SIZE=0x1000;
        PageTableEntryUnion  copy=entry;
        if(copy.raw&PageTableEntry::P_MASK){
            phyaddr_t subtb_phybase=(copy.pte.page_addr*_4KB_SIZE) & PHYS_ADDR_MASK;
            if(Clevel==PageTableEntryType::PDPT||Clevel==PageTableEntryType::PD)
            {
                if(copy.raw&PDE::PS_MASK) return 0;
            }
            
            return subtb_phybase;
        }else{
            
            phyaddr_t entry_to_alloc_phybase=0;
            entry_to_alloc_phybase= FreePagesAllocator::alloc(_4KB_SIZE,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_pinned,contain);
            if(!entry_to_alloc_phybase||error_kurd(contain)) return 0;
            
            // 初始化新分配的页表内存为0
            for(uint16_t i=0; i<512; i++) {
                uint64_t offset = sizeof(PageTableEntryUnion) * i;
                PhyAddrAccessor::writeu64(entry_to_alloc_phybase + offset, 0);
            }
            entry.raw=0;
            entry.pte.KERNELbit=1;
            entry.pte.present=1;
            entry.pte.RWbit=1;
            entry.pte.page_addr=entry_to_alloc_phybase/_4KB_SIZE;
            return entry_to_alloc_phybase;
        }
    };

    auto _4lv_pte_4KB_entries_set=[ILLEAGLE_PAGES_COUNT,
        PAGES_COUNT_AND_BASE_OUT_OF_RANGE,
        NOT_ALIGNED_INPUT_BASE,
        TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY,
        atompages_cache_table_idx, get_sub_tb,desc_access,pml4tb_phyaddr_base]
    (phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count)->uint16_t{
        
        
        if(count==0||count>512){
            bsp_kout<<"AddressSpace::enable_VM_desc::_4lv_pte_4KB_entries_set:invalid count"<<kendl;
            return ILLEAGLE_PAGES_COUNT;
        }
        if(phybase%_4KB_SIZE||vaddr_base%_4KB_SIZE){
            return NOT_ALIGNED_INPUT_BASE;
        }
        uint16_t pml4_index=(vaddr_base>>39)&((1<<9)-1);
        uint16_t pdpte_index=(vaddr_base>>30)&((1<<9)-1);
        uint16_t pde_index=(vaddr_base>>21)&((1<<9)-1);
        uint16_t pte_index=(vaddr_base>>12)&((1<<9)-1);
        if(pte_index+count>512){
            bsp_kout<<"AddressSpace::enable_VM_desc::_4lv_pte_4KB_entries_set:cross page directory boundary not allowed"<<kendl;
            return PAGES_COUNT_AND_BASE_OUT_OF_RANGE;
        }//这里权限问题待解决
        
        // 使用pml4_phybase和PhyAddrAccessor访问PML4项
        uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_index;
        uint64_t pml4_addr = pml4tb_phyaddr_base + pml4_offset;
        uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
        PageTableEntryUnion pml4e = { .raw = pml4_raw };
        
        phyaddr_t pdpte_tb_phyaddr = get_sub_tb(pml4e,PageTableEntryType::PML4);
        if(!pdpte_tb_phyaddr)return OS_OUT_OF_MEMORY;
        
        // 检查并写回PML4项（如果需要）
        if (!(pml4_raw & PageTableEntry::P_MASK) && (pml4e.pml4.present)) {
            PhyAddrAccessor::writeu64(pml4_addr, pml4e.raw);
        }
        
        uint64_t pdpte_offset = sizeof(PageTableEntryUnion) * pdpte_index;
        uint64_t pdpte_addr = pdpte_tb_phyaddr + pdpte_offset;
        uint64_t pdpte_raw = PhyAddrAccessor::readu64(pdpte_addr);
        PageTableEntryUnion pdpte_entry = { .raw = pdpte_raw };
        
        phyaddr_t pde_tb_phyaddr = get_sub_tb(pdpte_entry, PageTableEntryType::PDPT);
        if(!pde_tb_phyaddr)
            {
            if((pdpte_raw&PDPTE::PS_MASK)&&(pdpte_raw&PageTableEntry::P_MASK))
                {
                    return TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY;
                }
            else 
                return OS_OUT_OF_MEMORY;
            }
        // 如果在get_sub_tb中修改了pdpte_entry（例如分配了新的页表），需要将修改写回
        if (!(pdpte_raw & PageTableEntry::P_MASK)&&(pdpte_entry.pdpte.present)) {
            PhyAddrAccessor::writeu64(pdpte_addr, pdpte_entry.raw);
        }
        
        uint64_t pde_offset = sizeof(PageTableEntryUnion) * pde_index;
        uint64_t pde_addr = pde_tb_phyaddr + pde_offset;
        uint64_t pde_raw = PhyAddrAccessor::readu64(pde_addr);
        PageTableEntryUnion pde_entry = { .raw = pde_raw };
        
        phyaddr_t pte_tb_phyaddr = get_sub_tb(pde_entry, PageTableEntryType::PD);
        if(!pte_tb_phyaddr)
            {
            if(pde_raw&PageTableEntry::P_MASK&&pde_raw&PDE::PS_MASK)
                {   
                return TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY;
                }
            else return OS_OUT_OF_MEMORY;
        }
            
        // 如果在get_sub_tb中修改了pde_entry（例如分配了新的页表），需要将修改写回
        if (!(pde_raw & PageTableEntry::P_MASK)&&(pde_entry.pde.present)) {
            PhyAddrAccessor::writeu64(pde_addr, pde_entry.raw);
        }
        
        //设置pte项
        for(uint16_t i=0;i<count;i++){
            uint64_t pte_offset = sizeof(PageTableEntryUnion) * (pte_index + i);
            uint64_t pte_addr = pte_tb_phyaddr + pte_offset;
            
            PageTableEntryUnion pte_entry;
            pte_entry.raw=0;
            pte_entry.pte.present=1;
            pte_entry.pte.RWbit=desc_access.is_writeable;
            pte_entry.pte.KERNELbit=!desc_access.is_kernel;
            pte_entry.pte.page_addr=(phybase/0x1000)+i;
            pte_entry.pte.PWT=atompages_cache_table_idx.PWT;
            pte_entry.pte.PCD=atompages_cache_table_idx.PCD;
            pte_entry.pte.PAT=atompages_cache_table_idx.PAT;
            pte_entry.pte.EXECUTE_DENY=!desc_access.is_executable;
            pte_entry.pte.global=desc_access.is_global;
            
            PhyAddrAccessor::writeu64(pte_addr, pte_entry.raw);
        }
        return 0;
    };
    // ==================== 2. 2MB 大页版 ====================
    auto _4lv_pde_2MB_entries_set = [
        ILLEAGLE_PAGES_COUNT,
        PAGES_COUNT_AND_BASE_OUT_OF_RANGE,
        NOT_ALIGNED_INPUT_BASE,
        TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY,
        desc_access,atompages_cache_table_idx, get_sub_tb,pml4tb_phyaddr_base
    ](phyaddr_t phybase, vaddr_t vaddr_base, uint16_t count) -> uint16_t {
        
        if (count == 0 || count > 512) {
            bsp_kout<< "AddressSpace::enable_VM_desc::_4lv_pde_2MB_entries_set:cross invalid count" << kendl;
            return ILLEAGLE_PAGES_COUNT;
        }
        constexpr uint32_t _2MB_SIZE = 0x200000;
        if (phybase % _2MB_SIZE || vaddr_base % _2MB_SIZE) {
            return NOT_ALIGNED_INPUT_BASE;
        }

        uint16_t pml4_index  = (vaddr_base >> 39) & 0x1FF;
        uint16_t pdpte_index = (vaddr_base >> 30) & 0x1FF;
        uint16_t pde_index   = (vaddr_base >> 21) & 0x1FF;

        if (pde_index + count > 512) {
            bsp_kout<< "AddressSpace::enable_VM_desc::_4lv_pde_2MB_entries_set:cross page directory boundary not allowed" << kendl;
            return PAGES_COUNT_AND_BASE_OUT_OF_RANGE;
        }

        // 使用pml4tb_phyaddr_base和PhyAddrAccessor访问PML4项
        uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_index;
        uint64_t pml4_addr = pml4tb_phyaddr_base + pml4_offset;
        uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
        PageTableEntryUnion pml4e = { .raw = pml4_raw };
        
        phyaddr_t pdpte_tb_phyaddr = get_sub_tb(pml4e, PageTableEntryType::PML4);
        if (!pdpte_tb_phyaddr) return OS_OUT_OF_MEMORY;
        
        // 检查并写回PML4项（如果需要）
        if (!(pml4_raw & PageTableEntry::P_MASK) && (pml4e.pml4.present)) {
            PhyAddrAccessor::writeu64(pml4_addr, pml4e.raw);
        }
        
        uint64_t pdpte_offset = sizeof(PageTableEntryUnion) * pdpte_index;
        uint64_t pdpte_addr = pdpte_tb_phyaddr + pdpte_offset;
        uint64_t pdpte_raw = PhyAddrAccessor::readu64(pdpte_addr);
        PageTableEntryUnion pdpte_entry = { .raw = pdpte_raw };
        
        phyaddr_t pde_tb_phyaddr = get_sub_tb(pdpte_entry, PageTableEntryType::PDPT);
        if(!pde_tb_phyaddr)
            {
                if((pdpte_raw&PDPTE::PS_MASK)&&(pdpte_raw&PageTableEntry::P_MASK))
                {
                    return TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY;
                }
            else 
                return OS_OUT_OF_MEMORY;
            }
            
        // 如果在get_sub_tb中修改了pdpte_entry（例如分配了新的页表），需要将修改写回
        if (!(pdpte_raw & PageTableEntry::P_MASK)&&(pdpte_entry.pdpte.present)) {
            PhyAddrAccessor::writeu64(pdpte_addr, pdpte_entry.raw);
        }
        
        // 设置pde项
        for(uint16_t i=0;i<count;i++){
            uint64_t pde_offset = sizeof(PageTableEntryUnion) * (pde_index + i);
            uint64_t pde_addr = pde_tb_phyaddr + pde_offset;
            
            PageTableEntryUnion pde_entry;
            pde_entry.raw = 0;
            pde_entry.raw |= PDE::PS_MASK;
            pde_entry.pde2MB.present = 1;
            pde_entry.pde2MB.RWbit = desc_access.is_writeable;
            pde_entry.pde2MB.KERNELbit = !desc_access.is_kernel;
            pde_entry.pde2MB._2mb_Addr = phybase / _2MB_SIZE + i;
            pde_entry.pde2MB.PWT = atompages_cache_table_idx.PWT;
            pde_entry.pde2MB.PCD = atompages_cache_table_idx.PCD;
            pde_entry.pde2MB.PAT = atompages_cache_table_idx.PAT;
            pde_entry.pde2MB.EXECUTE_DENY = !desc_access.is_executable;
            pde_entry.pde2MB.global = desc_access.is_global;
            
            PhyAddrAccessor::writeu64(pde_addr, pde_entry.raw);
        }
        return 0;
    };

    // ==================== 3. 1GB 大页版 ====================
    auto _4lv_pdpte_1GB_entries_set = [ILLEAGLE_PAGES_COUNT,
        PAGES_COUNT_AND_BASE_OUT_OF_RANGE,
        NOT_ALIGNED_INPUT_BASE,
        TRY_TO_GET_SUB_ENTRY_FOT_BIG_ATOM_PAGE_ENTRY,desc_access,atompages_cache_table_idx, get_sub_tb,pml4tb_phyaddr_base](
        phyaddr_t phybase, vaddr_t vaddr_base, uint64_t count) -> uint16_t {
        if (count == 0 || count > 512) {
            bsp_kout<< "AddressSpace::enable_VM_desc::_4lv_pdpte_1GB_entries_set:invalid pages count" << kendl;
            return ILLEAGLE_PAGES_COUNT;
        }
        constexpr uint64_t _1GB_SIZE = 0x40000000ULL;
        if (phybase % _1GB_SIZE || vaddr_base % _1GB_SIZE) {
            return NOT_ALIGNED_INPUT_BASE;
        }

        uint16_t pml4_index  = (vaddr_base >> 39) & 0x1FF;
        uint16_t pdpte_index = (vaddr_base >> 30) & 0x1FF;

        if (pdpte_index + count > 512) {
            bsp_kout<< "AddressSpace::enable_VM_desc::_4lv_pdpte_1GB_entries_set:cross page directory boundary not allowed" << kendl;
            return PAGES_COUNT_AND_BASE_OUT_OF_RANGE;
        }

        // 使用pml4tb_phyaddr_base和PhyAddrAccessor访问PML4项
        uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_index;
        uint64_t pml4_addr = pml4tb_phyaddr_base + pml4_offset;
        uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
        PageTableEntryUnion pml4e = { .raw = pml4_raw };
        
        phyaddr_t pdpte_tb_phyaddr = get_sub_tb(pml4e, PageTableEntryType::PML4);
        if (!pdpte_tb_phyaddr) return OS_OUT_OF_MEMORY;
        
        // 检查并写回PML4项（如果需要）
        if (!(pml4_raw & PageTableEntry::P_MASK) && (pml4e.pml4.present)) {
            PhyAddrAccessor::writeu64(pml4_addr, pml4e.raw);
        }
        // 设置pdpte项
        for(uint16_t i=0;i<count;i++){
            uint64_t pdpte_offset = sizeof(PageTableEntryUnion) * (pdpte_index + i);
            uint64_t pdpte_addr = pdpte_tb_phyaddr + pdpte_offset;
            
            PageTableEntryUnion pdpte_entry;
            pdpte_entry.raw = 0;
            pdpte_entry.raw |= PDPTE::PS_MASK;
            pdpte_entry.pdpte1GB.present = 1;
            pdpte_entry.pdpte1GB.RWbit = desc_access.is_writeable;
            pdpte_entry.pdpte1GB.KERNELbit = !desc_access.is_kernel;
            pdpte_entry.pdpte1GB._1GB_Addr = (phybase / _1GB_SIZE) + i;
            pdpte_entry.pdpte1GB.PWT = atompages_cache_table_idx.PWT;
            pdpte_entry.pdpte1GB.PCD = atompages_cache_table_idx.PCD;
            pdpte_entry.pdpte1GB.PAT = atompages_cache_table_idx.PAT;
            pdpte_entry.pdpte1GB.EXECUTE_DENY = !desc_access.is_executable;
            pdpte_entry.pdpte1GB.global = desc_access.is_global;
            
            PhyAddrAccessor::writeu64(pdpte_addr, pdpte_entry.raw);
        }
        return 0;
    };
    //这里才是正式逻辑
    seg_to_pages_info_pakage_t package;
    int status = vm_interval_to_pages_info(package, desc);
    if(status!=OS_SUCCESS){
        fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY_CANT_SPLIT;
        return fail;
    }
    uint16_t contain=0;

    if(pglv_4_or_5==PAGE_TBALE_LV::LV_4)
    {
        spinrwlock_interrupt_about_write_guard lock_guard(lock);
        for(int i=0;i<5;i++) {
            auto &entry = package.entryies[i];
            if(entry.num_of_pages==0) continue;
            uint64_t psize = entry.page_size_in_byte;
            if(psize==0) goto page_size_invalid;
            if((entry.vbase % psize) != 0 || (entry.phybase % psize) != 0){
                fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
                goto bad_vmentry_unlock;
            }
            switch(entry.page_size_in_byte){
            case _1GB_SIZE: {
                uint64_t count_to_assign_left = entry.num_of_pages;
                uint16_t pdpte_idx = (entry.vbase / _1GB_SIZE) & 0x1FF;
                uint64_t processed_pages = 0;
                auto min = [](uint64_t a, uint64_t b)->uint64_t { return a < b ? a : b; };
                while(count_to_assign_left > 0){
                    uint16_t this_count = static_cast<uint16_t>(min(count_to_assign_left, 512 - pdpte_idx));
                    phyaddr_t this_phybase = entry.phybase + processed_pages * _1GB_SIZE;
                    vaddr_t this_vbase = entry.vbase + processed_pages * _1GB_SIZE;
                    contain = _4lv_pdpte_1GB_entries_set(this_phybase, this_vbase, this_count);
                    if(contain==OS_OUT_OF_MEMORY)goto pages_runout_chech;
                    if(contain!=0)goto sub_step_invalid;
                    count_to_assign_left -= this_count;
                    processed_pages += this_count;
                    pdpte_idx = 0;
                }
                break;
            }
            case _2MB_SIZE: {
                uint64_t count_to_assign_left = entry.num_of_pages;
                uint16_t pde_idx = (entry.vbase / _2MB_SIZE) & 0x1FF;
                uint64_t processed_pages = 0;
                auto min = [](uint64_t a, uint64_t b)->uint64_t { return a < b ? a : b; };
                while(count_to_assign_left > 0){
                    uint16_t this_count = static_cast<uint16_t>(min(count_to_assign_left, 512 - pde_idx));
                    phyaddr_t this_phybase = entry.phybase + processed_pages * _2MB_SIZE;
                    vaddr_t this_vbase = entry.vbase + processed_pages * _2MB_SIZE;
                    contain = _4lv_pde_2MB_entries_set(this_phybase, this_vbase, this_count);
                    if(contain==OS_OUT_OF_MEMORY)goto pages_runout_chech;
                    if(contain!=0)goto sub_step_invalid;
                    count_to_assign_left -= this_count;
                    processed_pages += this_count;
                    pde_idx = 0;
                }
                break;
            }
            case _4KB_SIZE: {
                uint64_t count_to_assign_left = entry.num_of_pages;
                uint16_t pte_idx = (entry.vbase / _4KB_SIZE) & 0x1FF;
                uint64_t processed_pages = 0;
                auto min = [](uint64_t a, uint64_t b)->uint64_t { return a < b ? a : b; };
                while(count_to_assign_left > 0){
                    uint16_t this_count = static_cast<uint16_t>(min(count_to_assign_left, 512 - pte_idx));
                    phyaddr_t this_phybase = entry.phybase + processed_pages * _4KB_SIZE;
                    vaddr_t this_vbase = entry.vbase + processed_pages * _4KB_SIZE;
                    contain = _4lv_pte_4KB_entries_set(this_phybase, this_vbase, this_count);
                    if(contain==OS_OUT_OF_MEMORY)goto pages_runout_chech;
                    if(contain!=0)goto sub_step_invalid;
                    count_to_assign_left -= this_count;
                    processed_pages += this_count;
                    pte_idx = 0;
                }
                break;
            }
            default:
                goto page_size_invalid;
            }
        }
        occupyied_size+=(desc.end-desc.start);
        goto success;
    }else{
        fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_NOT_SUPPORT_LV5_PAGING;
    }

    
    success:

    if(is_reach_va_bottom){
        success.result=result_code::SUCCESS_BUT_SIDE_EFFECT;
        success.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::SUCCESS_BUT_SIDE_AFFECTS::REASON_CODE_MAP_LOW_16K;
    }
    return success;
    page_size_invalid:

    fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALID_PAGE_SIZE;
    return fatal;
    bad_vmentry_unlock:

    return fail;
    sub_step_invalid:

    fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::ENABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_TRY_TO_GET_SUB_PAGE_IN_ATOM_PAGE;
    pages_runout_chech:

    return pages_alloc_event_kurd;
}
/**
 * 现在外部接口内的匿名函数的错误值返回机制
 * 最简单的就是私有的约定的作用域在函数内的错误码
 * 再稍微复杂一点考虑位图编码，复杂度压缩到一个结构体，这样子匿名函数与外部接口的通信成本也降低
 * 再复杂就得考虑独立成一个EVENT,从用完即弃的匿名函数升格为一般函数
 * 我提倡对于uint64_t里面位图编码的方式，
 */
KURD_t AddressSpace::disable_VM_desc(VM_DESC desc)
{
    // 参数校验（与 enable_VM_desc 保持一致）
    KURD_t success=default_success();
    KURD_t fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_DISABLE_VMENTRY;
    fail.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_DISABLE_VMENTRY;
    fatal.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_DISABLE_VMENTRY;
    if(
        desc.start>=desc.end||
        desc.end>(pglv_4_or_5?PAGE_LV4_USERSPACE_SIZE:PAGE_LV5_USERSPACE_SIZE)||
        desc.start%_4KB_SIZE||
        desc.end%_4KB_SIZE||
        desc.phys_start<16*_4KB_SIZE||
        desc.end%_4KB_SIZE
    ){fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
    return fail;}
    uint64_t currunt_roottb_phyaddr=0;
    asm volatile("mov %%cr3, %0" : "=r"(currunt_roottb_phyaddr));
    bool will_invalidate_soon=align_down(currunt_roottb_phyaddr, _4KB_SIZE)==this->pml4_phybase;
    phyaddr_t pml4tb_phyaddr_base=pml4_phybase;
    struct pages_clear_result_bitmap{
        uint64_t success:1;
        uint64_t con:1;
    };
    enum pages_clear_error_status:uint8_t{
        SUCCESS,
        COUNT_OUT_OF_RANGE,
        ADDRESS_NOT_ALIGNED,
        TRY_TO_GET_SUB_PAGE_OF_HUGE_PAGE,
        TRY_TO_GET_SUB_PAGE_OF_NOT_PRESENT_PAGE,
        TRY_TO_CLEAR_UNPRESENT_PAGE,
        CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY
    };
    //新增错误码OS_PGTB_FREE_VALIDATION_FAIL
    // 清 4KB PTE 范围（不会分配表，表不存在就认为已清空）
   // ==== 4KB PTE 清理（含物理地址校验 + invlpg） ====
    auto _4lv_pte_4KB_entries_clear = [pml4tb_phyaddr_base](
    phyaddr_t phybase, vaddr_t vaddr_base, uint16_t count) -> pages_clear_error_status
    {
    if (count == 0 || count > 512) {
        bsp_kout<<"OS_PGTB_FREE_VALIDATION_FAIL"<<kendl;
        return COUNT_OUT_OF_RANGE;
    }
    if (phybase % _4KB_SIZE ||vaddr_base % _4KB_SIZE) return ADDRESS_NOT_ALIGNED;

    uint16_t pml4_index  = (vaddr_base >> 39) & 0x1FF;
    uint16_t pdpte_index = (vaddr_base >> 30) & 0x1FF;
    uint16_t pde_index   = (vaddr_base >> 21) & 0x1FF;
    uint16_t pte_index   = (vaddr_base >> 12) & 0x1FF;

    if (pte_index + count > 512) return TRY_TO_GET_SUB_PAGE_OF_HUGE_PAGE;

    // 使用pml4tb_phyaddr_base和PhyAddrAccessor访问PML4项
    uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_index;
    uint64_t pml4_addr = pml4tb_phyaddr_base + pml4_offset;
    uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
    PageTableEntryUnion pml4e = { .raw = pml4_raw };
    
    phyaddr_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
    uint64_t pdpte_raw=PhyAddrAccessor::readu64(pdpt_base+pdpte_index*sizeof(PageTableEntryUnion));
    if(!(pdpte_raw&PageTableEntry::P_MASK))
    { 
        return TRY_TO_GET_SUB_PAGE_OF_NOT_PRESENT_PAGE;
    }else {
        if(pdpte_raw&PDPTE::PS_MASK)return TRY_TO_GET_SUB_PAGE_OF_HUGE_PAGE;
    }
    phyaddr_t pde_base=align_down(pdpte_raw,_4KB_SIZE)&PHYS_ADDR_MASK;
    uint64_t pde_raw=PhyAddrAccessor::readu64(pde_base+pde_index*sizeof(PageTableEntryUnion));
    if(!(pde_raw&PageTableEntry::P_MASK))
    { 
        return TRY_TO_GET_SUB_PAGE_OF_NOT_PRESENT_PAGE;
    }else {
        if(pde_raw&PDE::PS_MASK)return TRY_TO_GET_SUB_PAGE_OF_HUGE_PAGE;
    }
    phyaddr_t pte_base=align_down(pde_raw,_4KB_SIZE)&PHYS_ADDR_MASK;
    for(uint16_t i=pte_index;i<pte_index+count;i++)
    {
        uint64_t pte_raw=PhyAddrAccessor::readu64(pte_base+i*sizeof(PageTableEntryUnion));
        if(!(pte_raw&PageTableEntry::P_MASK))
        {
            return TRY_TO_CLEAR_UNPRESENT_PAGE;
        }
        phyaddr_t pte_phyaddr=align_down(pte_raw,_4KB_SIZE)&PHYS_ADDR_MASK;
        if(pte_phyaddr!=phybase+(i-pte_index)*_4KB_SIZE)
            return CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY;
;
        PhyAddrAccessor::writeu64(pte_base+i*sizeof(PageTableEntryUnion),0);
    }
    bool all_clear=true;
    for(uint16_t i=0;i<512;i++){
        if(PhyAddrAccessor::readu64(pte_base+i*sizeof(PageTableEntryUnion))){
            all_clear=false;
            break;
        }
    }
    if(all_clear)
     {
        FreePagesAllocator::free(pte_base,1<<12);
        PhyAddrAccessor::writeu64(pde_base+pde_index*sizeof(PageTableEntryUnion),0);
        
        // Check if the entire PD is now empty and can be recycled
        bool pd_all_clear = true;
        for (uint16_t i = 0; i < 512; i++) {
            if (PhyAddrAccessor::readu64(pde_base + i * sizeof(PageTableEntryUnion))) {
                pd_all_clear = false;
                break;
            }
        }
        
        if (pd_all_clear) {
            // Recycle the entire PD page
            FreePagesAllocator::free(pde_base, 1<<12);
            PhyAddrAccessor::writeu64(pdpt_base + pdpte_index * sizeof(PageTableEntryUnion), 0);
            
            // Check if the entire PDPT is now empty and can be recycled
            bool pdpt_all_clear = true;
            for (uint16_t i = 0; i < 512; i++) {
                if (PhyAddrAccessor::readu64(pdpt_base + i * sizeof(PageTableEntryUnion))) {
                    pdpt_all_clear = false;
                    break;
                }
            }
            
            if (pdpt_all_clear) {
                // Recycle the entire PDPT page
                FreePagesAllocator::free(pdpt_base, 1<<12);
                // 使用pml4tb_phyaddr_base和PhyAddrAccessor修改PML4项
                PhyAddrAccessor::writeu64(pml4tb_phyaddr_base + pml4_index * sizeof(PageTableEntryUnion), 0);
            }
        }
     }
    return SUCCESS;
    };



    // 清 2MB PDE 范围（若上级不存在则视为已清空）
    // ==== 2MB PDE 清理（含物理地址校验 + invlpg） ====
auto _4lv_pde_2MB_entries_clear = [pml4tb_phyaddr_base, will_invalidate_soon](
    phyaddr_t phybase, vaddr_t vaddr_base, uint16_t count) -> pages_clear_error_status
{
    if (count == 0 || count > 512) {
        bsp_kout<<"AddressSpace::disable_VM_desc: out of range"<<kendl;
        return COUNT_OUT_OF_RANGE;
    }
    constexpr uint32_t _2MB_SIZE = 0x200000;
    if (vaddr_base % _2MB_SIZE) return ADDRESS_NOT_ALIGNED;

    uint16_t pml4_index  = (vaddr_base >> 39) & 0x1FF;
    uint16_t pdpte_index = (vaddr_base >> 30) & 0x1FF;
    uint16_t pde_index   = (vaddr_base >> 21) & 0x1FF;

    if (pde_index + count > 512) return COUNT_OUT_OF_RANGE;

    // 使用pml4tb_phyaddr_base和PhyAddrAccessor访问PML4项
    uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_index;
    uint64_t pml4_addr = pml4tb_phyaddr_base + pml4_offset;
    uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
    PageTableEntryUnion pml4e = { .raw = pml4_raw };
    
    phyaddr_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
    uint64_t pdpte_raw = PhyAddrAccessor::readu64(pdpt_base + pdpte_index * sizeof(PageTableEntryUnion));
    
    // 检查PDPT条目是否存在且不是1GB大页
    if (!(pdpte_raw & PageTableEntry::P_MASK)) {
        return TRY_TO_GET_SUB_PAGE_OF_NOT_PRESENT_PAGE;
    } else {
        if (pdpte_raw & PDPTE::PS_MASK) return TRY_TO_GET_SUB_PAGE_OF_HUGE_PAGE;
    }
    
    phyaddr_t pde_base = align_down(pdpte_raw, _4KB_SIZE) & PHYS_ADDR_MASK;
    
    // 逐项校验：条目必须为 2MB（PS=1）并且物理地址对齐匹配
    uint64_t expected_2mb = phybase / _2MB_SIZE;
    for (int i = 0; i < count; i++) {
        uint64_t pde_raw = PhyAddrAccessor::readu64(pde_base + (pde_index + i) * sizeof(PageTableEntryUnion));
        
        // 检查是否为2MB大页
        if (!(pde_raw & PDE::PS_MASK)) {
            // 不是 2MB 大页，校验不通过
            return CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY;
        }
        
        // 解析2MB页的物理地址
        uint64_t pde_phyaddr = align_down(pde_raw, _2MB_SIZE) & PHYS_ADDR_MASK;
        if (pde_phyaddr != (expected_2mb + i) * _2MB_SIZE) {
            return CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY;
        }
        
        // 清零并立即 invlpg（按 2MB 大页的虚地址）
        PhyAddrAccessor::writeu64(pde_base + (pde_index + i) * sizeof(PageTableEntryUnion), 0);
        if (will_invalidate_soon) {
            vaddr_t va = vaddr_base + (vaddr_t)i * _2MB_SIZE;
        }
    }

    // 检查 PDE 表是否全空
    bool pde_all_empty = true;
    for (int i = 0; i < 512; i++) {
        if (PhyAddrAccessor::readu64(pde_base + i * sizeof(PageTableEntryUnion))) {
            pde_all_empty = false;
            break;
        }
    }
    if (!pde_all_empty) return SUCCESS;

    // 释放 PDE 表
    FreePagesAllocator::free(pde_base, 1<<12);
    PhyAddrAccessor::writeu64(pdpt_base + pdpte_index * sizeof(PageTableEntryUnion), 0);

    // 检查 PDPT 是否全空
    bool pdpt_all_empty = true;
    for (int i = 0; i < 512; i++) {
        if (PhyAddrAccessor::readu64(pdpt_base + i * sizeof(PageTableEntryUnion))) {
            pdpt_all_empty = false;
            break;
        }
    }
    if (!pdpt_all_empty) return SUCCESS;

    // 释放 PDPT
    FreePagesAllocator::free(pdpt_base, 1<<12);
    // 使用pml4tb_phyaddr_base和PhyAddrAccessor修改PML4项
    PhyAddrAccessor::writeu64(pml4tb_phyaddr_base + pml4_index * sizeof(PageTableEntryUnion), 0);
    return SUCCESS;
};


    // 清 1GB PDPTE 范围
   // ==== 1GB PDPT 清理（含物理地址校验 + invlpg） ====
auto _4lv_pdpte_1GB_entries_clear = [pml4tb_phyaddr_base, will_invalidate_soon](
    phyaddr_t phybase, vaddr_t vaddr_base, uint64_t count) -> pages_clear_error_status {
    if (count == 0 || count > 512) {
        bsp_kout<<"AddressSpace::disable_VM_desc::_4lv_pdpte_1GB_entries_clear: invalid count"<<kendl;
        return COUNT_OUT_OF_RANGE;
    }
    constexpr uint64_t _1GB_SIZE = 0x40000000ULL;
    if (vaddr_base % _1GB_SIZE) return ADDRESS_NOT_ALIGNED;

    uint16_t pml4_index  = (vaddr_base >> 39) & 0x1FF;
    uint16_t pdpte_index = (vaddr_base >> 30) & 0x1FF;

    if (pdpte_index + count > 512) return COUNT_OUT_OF_RANGE;

    // 使用pml4tb_phyaddr_base和PhyAddrAccessor访问PML4项
    uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_index;
    uint64_t pml4_addr = pml4tb_phyaddr_base + pml4_offset;
    uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
    PageTableEntryUnion pml4e = { .raw = pml4_raw };
    
    phyaddr_t pdpt_base = pml4e.pml4.pdpte_addr << 12;

    // 逐项校验并清除
    uint64_t expected_1gb = phybase / _1GB_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        uint64_t pdpte_raw = PhyAddrAccessor::readu64(pdpt_base + (pdpte_index + i) * sizeof(PageTableEntryUnion));
        
        // 必须是 1GB 大页（PS=1）且物理地址一致
        if (!(pdpte_raw & PDPTE::PS_MASK)) return CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY;
        
        // 解析1GB页的物理地址
        uint64_t pdpte_phyaddr = align_down(pdpte_raw, _1GB_SIZE) & PHYS_ADDR_MASK;
        if (pdpte_phyaddr != (expected_1gb + i) * _1GB_SIZE) return CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY;
        
        PhyAddrAccessor::writeu64(pdpt_base + (pdpte_index + i) * sizeof(PageTableEntryUnion), 0);
        if (will_invalidate_soon) {
            vaddr_t va = vaddr_base + (vaddr_t)i * _1GB_SIZE;
            asm volatile("invlpg %0" : : "m"(va));
        }
    }

    // 检查 PDPT 是否全空
    bool pdpt_all_empty = true;
    for (uint16_t i = 0; i < 512; i++) {
        if (PhyAddrAccessor::readu64(pdpt_base + i * sizeof(PageTableEntryUnion))) {
            pdpt_all_empty = false;
            break;
        }
    }
    if (!pdpt_all_empty) return SUCCESS;

    // 释放 PDPT
    FreePagesAllocator::free(pdpt_base, 1<<12);
    // 使用pml4tb_phyaddr_base和PhyAddrAccessor修改PML4项
    PhyAddrAccessor::writeu64(pml4tb_phyaddr_base + pml4_index * sizeof(PageTableEntryUnion), 0);

    return SUCCESS;
};


    // 主流程
    seg_to_pages_info_pakage_t package;
    int status = vm_interval_to_pages_info(package, desc);
    pages_clear_error_status clear_status;
    if (status != OS_SUCCESS) {
        fail.reason = MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
        return fail;
    }
    if (pglv_4_or_5 == PAGE_TBALE_LV::LV_4) {
        spinrwlock_interrupt_about_write_guard lock_guard(lock);
        switch (package.congruence_level) {
        case congruence_level_1gb: {
            for (int i = 0; i < 5; i++) {
                auto &entry = package.entryies[i];
                if (entry.num_of_pages == 0) continue;
                uint64_t psize = entry.page_size_in_byte;
                if (psize == 0) goto page_size_invalid;
                if ((entry.vbase % psize) != 0 || (entry.phybase % psize) != 0) {
                    fail.reason = MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
                    goto bad_vmentry_unlock;
                }
                switch (entry.page_size_in_byte) {
                case _1GB_SIZE: {
                    uint64_t count_to_clear_left = entry.num_of_pages;
                    uint16_t pdpte_idx = (entry.vbase / _1GB_SIZE) & 0x1FF;
                    uint64_t processed_pages = 0;
                    auto min = [](uint64_t a, uint64_t b)->uint64_t { return a < b ? a : b; };
                    while (count_to_clear_left > 0) {
                        uint16_t this_count = static_cast<uint16_t>(min(count_to_clear_left, 512 - pdpte_idx));
                        phyaddr_t this_phybase = entry.phybase + processed_pages * _1GB_SIZE;
                        vaddr_t this_vbase = entry.vbase + processed_pages * _1GB_SIZE;
                        clear_status = _4lv_pdpte_1GB_entries_clear(this_phybase, this_vbase, this_count);
                        if (clear_status != pages_clear_error_status::SUCCESS) goto sub_step_invalid;
                        count_to_clear_left -= this_count;
                        processed_pages += this_count;
                        pdpte_idx = 0;
                    }
                    break;
                }
                case _2MB_SIZE: {
                    clear_status = _4lv_pde_2MB_entries_clear(
                        entry.phybase, entry.vbase, static_cast<uint16_t>(entry.num_of_pages));
                    if (clear_status != pages_clear_error_status::SUCCESS) goto sub_step_invalid;
                    break;
                }
                case _4KB_SIZE: {
                    clear_status = _4lv_pte_4KB_entries_clear(
                        entry.phybase, entry.vbase, static_cast<uint16_t>(entry.num_of_pages));
                    if (clear_status != pages_clear_error_status::SUCCESS) goto sub_step_invalid;
                    break;
                }
                default:
                    goto page_size_invalid;
                }
            }
            break;
        }
        case congruence_level_2mb: {
            for (int i = 0; i < 5; i++) {
                auto &entry = package.entryies[i];
                if (entry.num_of_pages == 0) continue;
                uint64_t psize = entry.page_size_in_byte;
                if (psize == 0) goto page_size_invalid;
                if ((entry.vbase % psize) != 0 || (entry.phybase % psize) != 0) {
                    fail.reason = MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
                    goto bad_vmentry_unlock;
                }
                switch (entry.page_size_in_byte) {
                case _2MB_SIZE: {
                    for (uint64_t j = 0; j < entry.num_of_pages; j++) {
                        clear_status = _4lv_pde_2MB_entries_clear(
                            entry.phybase + j * _2MB_SIZE,
                            entry.vbase + j * _2MB_SIZE,
                            1);
                        if (clear_status != pages_clear_error_status::SUCCESS) goto sub_step_invalid;
                    }
                    break;
                }
                case _4KB_SIZE: {
                    clear_status = _4lv_pte_4KB_entries_clear(
                        entry.phybase, entry.vbase, static_cast<uint16_t>(entry.num_of_pages));
                    if (clear_status != pages_clear_error_status::SUCCESS) goto sub_step_invalid;
                    break;
                }
                default:
                    goto page_size_invalid;
                }
            }
            break;
        }
        case congruence_level_4kb: {
            for (int i = 0; i < 5; i++) {
                auto &entry = package.entryies[i];
                if (entry.page_size_in_byte != _4KB_SIZE) continue;
                for (uint64_t j = 0; j < entry.num_of_pages; j++) {
                    clear_status = _4lv_pte_4KB_entries_clear(
                        entry.phybase + j * _4KB_SIZE,
                        entry.vbase + j * _4KB_SIZE,
                        1);
                    if (clear_status != pages_clear_error_status::SUCCESS) goto sub_step_invalid;
                }
            }
            break;
        }
        default:
            goto page_size_invalid;
        }
        occupyied_size-=(desc.end-desc.start);
        //tod:广播所有核心重新
        goto success;
    } else {
        fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_NOT_SUPPORT_LV5_PAGING;
        return fail;
    }

success:
    return success;
page_size_invalid:
    fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALID_PAGE_SIZE;
    return fatal;
bad_vmentry_unlock:
    return fail;
sub_step_invalid:
    switch (clear_status)
    {
    case pages_clear_error_status::TRY_TO_CLEAR_UNPRESENT_PAGE:
        fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_TRY_TO_CLEAR_UNPRESENT_PAGE;
        break;
    case pages_clear_error_status::TRY_TO_GET_SUB_PAGE_OF_HUGE_PAGE:
        fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_TRY_TO_GET_SUB_PAGE_IN_ATOM_PAGE;
        break;
    case pages_clear_error_status::CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY:
        fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY;
        break;
    default:
    fatal.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_OTHER_PAGES_SET_FATAL;
    }
    return fatal;
}
phyaddr_t AddressSpace::vaddr_to_paddr(vaddr_t vaddr,KURD_t& kurd)
{
    uint16_t pml5_idx = (vaddr >> 48)&511;
    uint16_t pml4_idx = (vaddr >> 39)&511;
    uint16_t pdpte_idx = (vaddr >> 30)&511;
    uint16_t pde_idx = (vaddr >> 21)&511;
    uint16_t pte_idx = (vaddr >> 12)&511;
    spinrwlock_interrupt_about_read_guard lock_guard(lock);
    KURD_t success=default_success();
   KURD_t fail=default_fail();
   KURD_t fatal=default_fatal();
   success.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_TRAN_TO_PHY;
   fail.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_TRAN_TO_PHY;
   fatal.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_TRAN_TO_PHY;
    if(pglv_4_or_5 == PAGE_TBALE_LV::LV_4){
        if(pml4_idx>255)goto not_allowd;// 高一半是内核空间,这里无权限访问
        
        // 使用pml4_phybase和PhyAddrAccessor访问PML4表项
        uint64_t pml4_offset = sizeof(PageTableEntryUnion) * pml4_idx;
        uint64_t pml4_addr = pml4_phybase + pml4_offset;
        uint64_t pml4_raw = PhyAddrAccessor::readu64(pml4_addr);
        PageTableEntryUnion pml4e_union;
        pml4e_union.raw = pml4_raw;
        PML4Entry pml4_entry = pml4e_union.pml4;
        
        if(!pml4_entry.present)goto entry_not_presnt;
        phyaddr_t pdpte_base_phyaddr = pml4_entry.pdpte_addr << 12;
        uint64_t pdpte_raw = PhyAddrAccessor::readu64(pdpte_base_phyaddr + pdpte_idx * sizeof(PageTableEntryUnion));
        PageTableEntryUnion pdpte;
        pdpte.raw = pdpte_raw;
        if(!pdpte.pdpte.present)goto entry_not_presnt;
        else if(pdpte.pdpte.large)goto pdpte_end;
        
        phyaddr_t pde_base_phyaddr = pdpte.pdpte.PD_addr << 12;
        uint64_t pde_raw = PhyAddrAccessor::readu64(pde_base_phyaddr + pde_idx * sizeof(PageTableEntryUnion));
        PageTableEntryUnion pde;
        pde.raw = pde_raw;
        if(!pde.pde.present)goto entry_not_presnt;
        else if(pde.raw&PDE::PS_MASK)goto pde_end;  
        phyaddr_t pte_base_phyaddr = pde.pde.pt_addr << 12;
        uint64_t pte_raw = PhyAddrAccessor::readu64(pte_base_phyaddr + pte_idx * sizeof(PageTableEntryUnion));
        PageTableEntryUnion pte;
        pte.raw = pte_raw;
        goto pte_end;
    }else{
        fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::TRAN_TO_PHY_RESULTS_CODE::FAIL_REASONS::REASON_CODE_NOT_SUPPORT_LV5_PAGING;
    kurd=fail;
    return 0;
    }
    not_allowd:
    fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::TRAN_TO_PHY_RESULTS_CODE::FAIL_REASONS::REASON_CODE_NOT_ALLOW_KSPACE_VA;
    kurd=fail;
    return 0;
    entry_not_presnt:
    fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::TRAN_TO_PHY_RESULTS_CODE::FAIL_REASONS::REASON_CODE_NOT_PRESENT_ENTRY;
    kurd=fail;
    return 0;
    pdpte_end:
    return (uint64_t(pml5_idx)<<48)+(uint64_t(pml4_idx)<<39)+(uint64_t(pdpte_idx)<<30)+(vaddr&(_1GB_SIZE-1));
    pde_end:
    return (uint64_t(pml5_idx)<<48)+(uint64_t(pml4_idx)<<39)+(uint64_t(pde_idx)<<21)+(vaddr&(_2MB_SIZE-1));
    pte_end:
    return (uint64_t(pml5_idx)<<48)+(uint64_t(pml4_idx)<<39)+(uint64_t(pde_idx)<<21)+(uint64_t(pte_idx)<<12)+(vaddr&(_4KB_SIZE-1));
}
/**
 * 不对pcid内容进行校验直接位运算
 */
int VM_desc_cmp(const VM_DESC &a, const VM_DESC &b)
{
    if(a.start>=b.end)return 1;
    if(a.end<=b.start)return -1;
    return 0;
}
void AddressSpace::unsafe_load_pml4_to_cr3(uint16_t pcid)
{
    uint64_t cr3_value=pml4_phybase|pcid;
    asm volatile("mov %0, %%cr3"::"r"(cr3_value));
}

AddressSpace::~AddressSpace()
{
    KURD_t status=FreePagesAllocator::free(pml4_phybase,1<<12);
    if(status.result!=result_code::SUCCESS){
        bsp_kout<<"phymemspace_mgr::pages_recycle failed in result:"<<status<<kendl;
    }
}

KURD_t AddressSpace::second_stage_init()
{
    alloc_flags_t flags=default_flags;
    flags.force_first_linekd_heap=true;
    flags.align_log2=12;
    KURD_t contain=KURD_t();
    pml4_phybase=FreePagesAllocator::alloc(_4KB_SIZE,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_pinned,contain);
    if(pml4_phybase==0||error_kurd(contain))return contain;
    for(uint16_t i=0;i<256;i++){
        PhyAddrAccessor::writeu64(
            pml4_phybase+i*sizeof(PageTableEntryUnion),
            0
        );
    }
    phyaddr_t kspacUPpdpt_phybase=KspacePageTable::kspace_uppdpt_phyaddr;
    PageTableEntryUnion Up_pml4e_template=KspacePageTable::high_half_template;
    for(uint16_t i=0;i<256;i++)
    {
        uint64_t raw=Up_pml4e_template.raw;
        raw|=(kspacUPpdpt_phybase+i*4096)&PHYS_ADDR_MASK;
        PhyAddrAccessor::writeu64(
            pml4_phybase+(i+256)*sizeof(PageTableEntryUnion),
            raw
        );
    }
    occupyied_size=0;
    KURD_t success=default_success();
    success.event_code=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::EVENT_CODE_INIT;
    return success;
}
KURD_t AddressSpace::build_identity_map_ONLY_IN_gKERNELSPACE()
{
    KURD_t fail=default_fail();
    if(this!=gKernelSpace){
        fail.reason=MEMMODULE_LOCAIONS::ADDRESSPACE_EVENTS::BUILD_INDENTITY_MAP_ONLY_ON_gKERNELSPACE::FAIL_REASONS::REASON_CODE_NOT_gKERNELSPACE;
        return fail;
    }
    VM_DESC identity_desc={
        .start=0x4000,
        .end=0x1000000000,
        .map_type=VM_DESC::map_type_t::MAP_PHYSICAL,
        .phys_start=0x4000,
        .access={
            .is_kernel=1,
            .is_writeable=1,
            .is_readable=1,
            .is_executable=1,
            .is_global=0,
            .cache_strategy=WB,
        },
        .committed_full=0,
        .is_vaddr_alloced=0,
        .is_out_bound_protective=0,
        .SEG_SIZE_ONLY_UES_IN_BASIC_SEG=0,
    };
    return enable_VM_desc(identity_desc);
}
