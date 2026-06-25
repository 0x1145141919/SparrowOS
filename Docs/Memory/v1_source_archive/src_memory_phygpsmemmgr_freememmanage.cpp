#include "phygpsmemmgr.h"
#include "kpoolmemmgr.h"
#include "Memory.h"
#include "VideoDriver.h"
#include "os_error_definitions.h"
#include "OS_utils.h"
#include "DoubleLinkList.h"

/**
 * 这里的align_require是（1ULL<<align_require）字节对齐
 * 输入的size_in_byte是字节数，向上取整到4kb
 * alloc_or_free1表示alloc.,0表示free
 */

phyaddr_t KernelSpacePgsMemMgr::Inner_fixed_addr_manage(
uint64_t linear_base, 
phymem_pgs_queue queue, 
pgaccess access,
phyaddr_t mapped_phybase,
bool modify_pgtb
)
{
    phyaddr_t scan_addr=linear_base;
    pgflags flag_of_pg={0 };
    if(cpu_pglv==4)
    {
        if (linear_base>=0xffff800000000000)
        {
            flag_of_pg.physical_or_virtual_pg=VIR_ATOM_PAGE;
        }else{
            flag_of_pg.physical_or_virtual_pg=PHY_ATOM_PAGE;
        }
        
    }else{
        if(linear_base>=0xff00000000000000)
        {
            flag_of_pg.physical_or_virtual_pg=VIR_ATOM_PAGE;
        }else{
            flag_of_pg.physical_or_virtual_pg=PHY_ATOM_PAGE;
        }
        
    }
    flag_of_pg.cache_strateggy=access.cache_strategy;
    flag_of_pg.is_occupied=access.is_occupyied;
    flag_of_pg.is_atom=1;
    flag_of_pg.is_exist=1;
    flag_of_pg.is_kernel=access.is_kernel;
    flag_of_pg.is_readable=access.is_readable;
    flag_of_pg.is_writable=access.is_writeable;
    flag_of_pg.is_executable=access.is_executable;
    for(int i=0;i<queue.entry_count;i++)
    {
        uint8_t lv=queue.entry[i].pgs_lv;
        flag_of_pg.pg_lv=lv;
        for(int j=0;j<queue.entry[i].pgs_count;j++)
        {
            if((this->*PgCBtb_construct_func[lv])(scan_addr,flag_of_pg,mapped_phybase+(scan_addr-linear_base))!=OS_SUCCESS)
            return scan_addr;
            scan_addr+=PAGE_SIZE_IN_LV[lv];
        }
    }
    if (modify_pgtb)
    {
       switch (cpu_pglv)
       {
       case 4:
       modify_pgtb_in_4lv(linear_base,scan_addr);
        /* code */
        break;
       case 5:
       modify_pgtb_in_5lv(linear_base,scan_addr);
        break;
       default:
        return OS_INVALID_PARAMETER ;
       }
    }
    
    return scan_addr;
}
// 定义提前终止码
#define OS_EARLY_RETURN 100

/**
 * 处理PTE级别（最后一级）的页表项
 */
int KernelSpacePgsMemMgr::process_pte_level(
    uint64_t* pt_base, 
    lowerlv_PgCBtb* pt_PgCBtb, 
    uint64_t& scan_addr, 
    uint64_t endaddr,
    uint64_t pml4_index, 
    uint64_t pdpt_index, 
    uint64_t pd_index,
    uint64_t start_pt_index
) {
    for (int l = start_pt_index; l < 512; l++) {
        pgflags tmp_flags = pt_PgCBtb->entries[l].flags;
         int result; if (tmp_flags.is_exist) {
            if(tmp_flags.physical_or_virtual_pg==PHY_ATOM_PAGE){
            phyaddr_t page_base = (pml4_index << 39) + (pdpt_index << 30) + (pd_index << 21) + (l << 12);
           result= pgtb_entry_convert(pt_base[l], pt_PgCBtb->entries[l], page_base);
            
            }else{
               result = pgtb_entry_convert(pt_base[l], pt_PgCBtb->entries[l], pt_PgCBtb->entries[l].base.base_phyaddr);
            }if (result != OS_SUCCESS) {
                return result;
            }
            scan_addr += PAGE_SIZE_IN_LV[0];
            if (scan_addr >= endaddr) {
                return OS_EARLY_RETURN;
            }
        }
    }
    return OS_SUCCESS;
}

/**
 * 处理PDE级别（第三级）的页表项
 */
int KernelSpacePgsMemMgr::process_pde_level(
    uint64_t* pd_base, 
    lowerlv_PgCBtb* pd_PgCBtb, 
    uint64_t& scan_addr, 
    uint64_t endaddr,
    uint64_t pml4_index, 
    uint64_t pdpt_index,
    uint64_t start_pd_index
) {
    for (int k = start_pd_index; k < 512; k++) {
        pgflags tmp_flags = pd_PgCBtb->entries[k].flags;
        if (tmp_flags.is_exist) {
            if (tmp_flags.is_atom) {
                // 处理2MB大页
                 int result;
                if(tmp_flags.physical_or_virtual_pg==PHY_ATOM_PAGE){
                phyaddr_t huge_2mb_base = (pml4_index << 39) + (pdpt_index << 30) + (k << 21);
                result = pgtb_entry_convert(pd_base[k], pd_PgCBtb->entries[k], huge_2mb_base);
                
                }else{
                result = pgtb_entry_convert(pd_base[k], pd_PgCBtb->entries[k], pd_PgCBtb->entries[k].base.base_phyaddr);
                }
                if (result != OS_SUCCESS) {
                    return result;
                }
                scan_addr += PAGE_SIZE_IN_LV[1];
                if (scan_addr >= endaddr) {
                    return OS_EARLY_RETURN;
                }
            } else {
                // 处理指向PT的普通PDE
                uint64_t* pt_base;
                lowerlv_PgCBtb* pt_PgCBtb = pd_PgCBtb->entries[k].base.lowerlvPgCBtb;
                
                if ((pd_base[k] & PageTableEntry::P_MASK) == 0) {
                    pt_base = (uint64_t*)pgtb_heap_ptr->pgalloc();
                    if (pt_base == nullptr) {
                        kputsSecure("modify_pgtb_in_4lv:pgalloc failed for pt_base\n");
                        return OS_OUT_OF_MEMORY;
                    }
                    
                    int result = pgtb_entry_convert(pd_base[k], pd_PgCBtb->entries[k], (phyaddr_t)pt_base);
                    if (result != OS_SUCCESS) {
                        pgtb_heap_ptr->free((phyaddr_t)pt_base);
                        return result;
                    }
                } else {
                    pt_base = (uint64_t*)(pd_base[k] & PDE::ADDR_PT_MASK);
                }
                
                // 处理PTE级别
                uint64_t start_pt_index = (scan_addr & lineaddr_index_filters::PT_INDEX_MASK_lv0) >> 12;
                int result = process_pte_level(pt_base, pt_PgCBtb, scan_addr, endaddr, 
                                              pml4_index, pdpt_index, k, start_pt_index);
                
                if (result == OS_EARLY_RETURN) {
                    return OS_EARLY_RETURN;
                } else if (result != OS_SUCCESS) {
                    return result;
                }
            }
        }
    }
    return OS_SUCCESS;
}

/**
 * 处理PDPTE级别（第二级）的页表项
 */
int KernelSpacePgsMemMgr::process_pdpte_level(
    uint64_t* pdpt_base, 
    lowerlv_PgCBtb* pdpt_PgCBtb, 
    uint64_t& scan_addr, 
    uint64_t endaddr,
    uint64_t pml4_index,
    uint64_t start_pdpt_index
) {
    for (int j = start_pdpt_index; j < 512; j++) {
        pgflags tmp_flags = pdpt_PgCBtb->entries[j].flags;
        if (tmp_flags.is_exist) {
            if (tmp_flags.is_atom) {
                // 处理1GB大页
                int result;
                if(tmp_flags.physical_or_virtual_pg==PHY_ATOM_PAGE){
                phyaddr_t huge_1gb_base = (pml4_index << 39) + (j << 30);
                result = pgtb_entry_convert(pdpt_base[j], pdpt_PgCBtb->entries[j], huge_1gb_base);
                
                }else{
                    result = pgtb_entry_convert(pdpt_base[j], pdpt_PgCBtb->entries[j], pdpt_PgCBtb->entries[j].base.base_phyaddr);
                }
                if (result != OS_SUCCESS) {
                    return result;
                }
                scan_addr += PAGE_SIZE_IN_LV[2];
                if (scan_addr >= endaddr) {
                    return OS_EARLY_RETURN;
                }
            } else {
                // 处理指向PD的普通PDPTE
                uint64_t* pd_base;
                lowerlv_PgCBtb* pd_PgCBtb = pdpt_PgCBtb->entries[j].base.lowerlvPgCBtb;
                
                if ((pdpt_base[j] & PageTableEntry::P_MASK) == 0) {
                    pd_base = (uint64_t*)pgtb_heap_ptr->pgalloc();
                    if (pd_base == nullptr) {
                        kputsSecure("modify_pgtb_in_4lv:pgalloc failed for pd_base\n");
                        return OS_OUT_OF_MEMORY;
                    }
                    
                    int result = pgtb_entry_convert(pdpt_base[j], pdpt_PgCBtb->entries[j], (phyaddr_t)pd_base);
                    if (result != OS_SUCCESS) {
                        pgtb_heap_ptr->free((phyaddr_t)pd_base);
                        return result;
                    }
                } else {
                    pd_base = (uint64_t*)(pdpt_base[j] & PDPTE::ADDR_PD_MASK);
                }
                
                // 处理PDE级别
                uint64_t start_pd_index = (scan_addr & lineaddr_index_filters::PD_INDEX_MASK_lv1) >> 21;
                int result = process_pde_level(pd_base, pd_PgCBtb, scan_addr, endaddr, 
                                              pml4_index, j, start_pd_index);
                
                if (result == OS_EARLY_RETURN) {
                    return OS_EARLY_RETURN;
                } else if (result != OS_SUCCESS) {
                    return result;
                }
            }
        }
    }
    return OS_SUCCESS;
}

/**
 * 处理PML4E级别（第一级）的页表项
 */
int KernelSpacePgsMemMgr::process_pml4e_level(
    uint64_t* rootPgtb, 
    lowerlv_PgCBtb* root_PgCBtb, 
    uint64_t& scan_addr, 
    uint64_t endaddr,
    uint64_t start_pml4_index
) {
    for (uint16_t i = start_pml4_index; i < 512; i++) {
        pgflags tmp_flags = root_PgCBtb->entries[i].flags;
        if (tmp_flags.is_exist) {
            if (tmp_flags.is_atom) {
                kputsSecure("modify_pgtb_in_4lv:atom entry in Pml4 not support\n");
                return OS_BAD_FUNCTION;
            } else {
                uint64_t* pdpt_base;
                
                if ((rootPgtb[i] & PageTableEntry::P_MASK) == 0) {
                    pdpt_base = (uint64_t*)pgtb_heap_ptr->pgalloc();
                    if (pdpt_base == nullptr) {
                        kputsSecure("modify_pgtb_in_4lv:pgalloc failed for pdpt_base\n");
                        return OS_OUT_OF_MEMORY;
                    }
                    
                    int result = pgtb_entry_convert(rootPgtb[i], root_PgCBtb->entries[i], (phyaddr_t)pdpt_base);
                    if (result != OS_SUCCESS) {
                        pgtb_heap_ptr->free((phyaddr_t)pdpt_base);
                        return result;
                    }
                } else {
                    pdpt_base = (uint64_t*)(rootPgtb[i] & PML4E::ADDR_MASK);
                }
                
                lowerlv_PgCBtb* pdpt_PgCBtb = root_PgCBtb->entries[i].base.lowerlvPgCBtb;
                
                // 处理PDPTE级别
                uint64_t start_pdpt_index = (scan_addr & lineaddr_index_filters::PDPT_INDEX_MASK_lv2) >> 30;
                int result = process_pdpte_level(pdpt_base, pdpt_PgCBtb, scan_addr, endaddr, i, start_pdpt_index);
                
                if (result == OS_EARLY_RETURN) {
                    return OS_EARLY_RETURN;
                } else if (result != OS_SUCCESS) {
                    return result;
                }
            }
        }
    }
    return OS_SUCCESS;
}
int KernelSpacePgsMemMgr::process_pml5e_level(
    uint64_t* rootPgtb, 
    lowerlv_PgCBtb* root_PgCBtb, 
    uint64_t& scan_addr, 
    uint64_t endaddr,
    uint64_t start_pml5_index
) {
    for (uint16_t i = start_pml5_index; i < 512; i++) {
        pgflags tmp_flags = root_PgCBtb->entries[i].flags;
        if (tmp_flags.is_exist) {
            if (tmp_flags.is_atom) {
                kputsSecure("modify_pgtb_in_5lvpg:atom entry in Pml5 not support\n");
                return OS_BAD_FUNCTION;
            } else {
                uint64_t* pml4_base;
                
                if ((rootPgtb[i] & PageTableEntry::P_MASK) == 0) {
                    pml4_base = (uint64_t*)pgtb_heap_ptr->pgalloc();
                    if (pml4_base == nullptr) {
                        kputsSecure("modify_pgtb_in_5lvpg:pgalloc failed for pml4_base\n");
                        return OS_OUT_OF_MEMORY;
                    }
                    
                    int result = pgtb_entry_convert(rootPgtb[i], root_PgCBtb->entries[i], (phyaddr_t)pml4_base);
                    if (result != OS_SUCCESS) {
                        pgtb_heap_ptr->free((phyaddr_t)pml4_base);
                        return result;
                    }
                } else {
                    pml4_base = (uint64_t*)(rootPgtb[i] & PML5E::ADDR_MASK);
                }
                
                lowerlv_PgCBtb* pml4_PgCBtb = root_PgCBtb->entries[i].base.lowerlvPgCBtb;
                
                // 处理PML4E级别
                uint64_t start_pml4_index = (scan_addr & lineaddr_index_filters::PML4_INDEX_MASK_lv3) >> 39;
                int result = process_pml4e_level(pml4_base, pml4_PgCBtb, scan_addr, endaddr,start_pml4_index);
                
                if (result == OS_EARLY_RETURN) {
                    return OS_EARLY_RETURN;
                } else if (result != OS_SUCCESS) {
                    return result;
                }
            }
        }
    }
    return OS_SUCCESS;
}
/**
 * 修改四级页表中的页表项
 * 这个函数现在通过调用分层处理函数来实现
 * 返回值为OS_SUCCESS或OS_EARLY_RETURN表示成功，其他值表示失败
 * OS_OUT_OF_MEMORY表示特殊堆内存耗尽
 */
int KernelSpacePgsMemMgr::modify_pgtb_in_4lv(uint64_t base, uint64_t endaddr) {
    lowerlv_PgCBtb* root_PgCBtb = rootlv4PgCBtb->base.lowerlvPgCBtb;
    uint64_t* rootPgtb = pgtb_heap_ptr->pgtb_root_phyaddr;
    uint64_t scan_addr = base;
    
    // 计算起始PML4索引
    uint64_t start_pml4_index = (scan_addr & lineaddr_index_filters::PML4_INDEX_MASK_lv3) >> 39;
    
    // 从PML4级别开始处理
    return process_pml4e_level(rootPgtb, root_PgCBtb, scan_addr, endaddr, start_pml4_index);
}
int KernelSpacePgsMemMgr::modify_pgtb_in_5lv(phyaddr_t base, uint64_t endaddr)
{
    lowerlv_PgCBtb* rootlv4PgCBtb;
    uint64_t* rootPgtb = pgtb_heap_ptr->pgtb_root_phyaddr;
    uint64_t scan_addr = base;
    // 计算起始PML5索引
    uint64_t start_pml5_index = (scan_addr & lineaddr_index_filters::PML5_INDEX_MASK_lv4) >> 48;
    
    // 从PML5级别开始处理
    return process_pml5e_level(rootPgtb, rootlv4PgCBtb, scan_addr, endaddr, start_pml5_index);
}

void *KernelSpacePgsMemMgr::pgs_allocate(uint64_t size_in_byte, pgaccess access, uint8_t align_require)
{

    if(size_in_byte==0)return nullptr;
    phyaddr_t scan_addr = 0x100000;
    size_in_byte += PAGE_OFFSET_MASK[0];
    size_in_byte &= ~PAGE_OFFSET_MASK[0];
    uint64_t align_require_mask = (1ULL << align_require) - 1;
    phy_memDescriptor*usage_query_result=nullptr;
    while(true){
    usage_query_result = queryPhysicalMemoryUsage(scan_addr,size_in_byte);
    if(usage_query_result==(phy_memDescriptor*)OS_OUT_OF_MEMORY)
    return (void*)OS_OUT_OF_MEMORY;    
    if(usage_query_result==nullptr)return nullptr;
    if(usage_query_result[0].Type==OS_ALLOCATABLE_MEMORY&&
    (usage_query_result[0].Attribute&1)==0&&
    usage_query_result[0].NumberOfPages*PAGE_SIZE_IN_LV[0]>=size_in_byte)
    {
        delete usage_query_result;
        break;
    }

    else {
        scan_addr+=PAGE_SIZE_IN_LV[0]*usage_query_result[0].NumberOfPages;
        scan_addr+=align_require_mask;
    scan_addr&=~align_require_mask;
    delete usage_query_result;
    continue;
    }
    int i = 0;
        for (; usage_query_result[i].Type!=EfiReservedMemoryType; i++)
        {
        }
        scan_addr=usage_query_result[i-1].PhysicalStart;
    scan_addr+=align_require_mask;
    scan_addr&=~align_require_mask;

    delete usage_query_result;
    }
    
    phymem_pgs_queue*queue=seg_to_queue(scan_addr,size_in_byte);
    phyaddr_t alloced_addr= Inner_fixed_addr_manage(scan_addr,*queue,access,true);
    if (alloced_addr!=scan_addr+size_in_byte)
    {
        delete queue;
    return nullptr;
    }
    delete usage_query_result;
    delete queue;
    fixedaddr_phy_pgs_allocate(scan_addr,size_in_byte);
    return (void*)scan_addr;
}

void *KernelSpacePgsMemMgr::pgs_remapp(phyaddr_t addr, pgflags flags, vaddr_t vbase)
{
    // 根据物理基址addr使用phymemSubMgr子系统中物理内存段增减引用数
    int remap_result = phymemSubMgr.remap_inc(addr);uint64_t size_in_byte;
    uint64_t numof_4kbpgs;
    
    // 若是失败再尝试使用gBaseMemMgr的物理内存段增减引用数接口
    if (remap_result != 0) {
        // TODO: 尝试使用gBaseMemMgr的物理内存段增减引用数接口
        phy_memDescriptor* query_result = gBaseMemMgr.queryPhysicalMemoryUsage(addr);
        int status = gBaseMemMgr.descriptor_remapped_inc(addr);
        if (status != OS_SUCCESS) {
            return nullptr;
        }    
        numof_4kbpgs = query_result->NumberOfPages;
        size_in_byte = numof_4kbpgs << 12;
    }else{
         // 成功增加引用数之后用对应的查询接口得到相应物理内存段的信息
    minimal_phymem_seg_t phy_info = phymemSubMgr.addr_query(addr);
    size_in_byte = phy_info.num_of_4kbpgs << 12;
    numof_4kbpgs = phy_info.num_of_4kbpgs;
    
    // 默认在末尾添加
    }
    int target_index = vaddrobj_count; 
   
    
    // 如果vbase参数非0先检查是不是有效的虚拟地址（4/5级分页下的高一般线性地址以及4kb对齐是否满足）
    if (vbase != 0) {
        // 检查是否是高一半内核空间地址
        if (vbase < 0xffff800000000000ULL) {
            phymemSubMgr.remap_dec(addr); // 回滚引用计数
            return nullptr;
        }
        
        // 检查是否4kb对齐
        if (vbase & 0xFFF) {
            phymemSubMgr.remap_dec(addr); // 回滚引用计数
            return nullptr;
        }
        
        // 检查是否与已分配的虚拟地址空间有重叠
        vaddr_t vaddr_end = vbase + size_in_byte;
        for (int i = 0; i < vaddrobj_count; i++) {
            vaddr_t existing_start = vaddr_objs[i].base;
            vaddr_t existing_end = vaddr_objs[i].base + (vaddr_objs[i].size_in_numof4kbpgs << 12);
            
            // 检查区间是否重叠: [vbase, vaddr_end) 与 [existing_start, existing_end)
            if (!(vaddr_end <= existing_start || vbase >= existing_end)) {
                phymemSubMgr.remap_dec(addr); // 回滚引用计数
                return nullptr; // 有重叠，返回nullptr
            }
        }
    } else {
        // vbase为0则自动分配
        // 参照pgs_allocate_remapped扫描一个空闲虚拟地址空间
        
        // 从前向后扫描，查找空闲空间
        for (int i = 0; i < vaddrobj_count; i++) {
            vaddr_t current_end = vaddr_objs[i].base + (vaddr_objs[i].size_in_numof4kbpgs << 12);
            bool found_space = true;
            
            // 检查下一个对象是否会与当前分配冲突
            if (i + 1 < vaddrobj_count) {
                vaddr_t next_start = vaddr_objs[i+1].base;
                if (current_end + size_in_byte > next_start) {
                    found_space = false;
                }
            }
            
            // 检查当前空隙是否足够大
            if (found_space) {
                vbase = current_end;
                target_index = i + 1;
                break;
            }
        }
        
        // 如果没有找到空闲空间，则在末尾分配
        if (vbase == 0 && vaddrobj_count > 0) {
            vbase = vaddr_objs[vaddrobj_count-1].base + (vaddr_objs[vaddrobj_count-1].size_in_numof4kbpgs << 12);
        }
        
        // 如果是第一次分配
        if (vaddrobj_count == 0) {
            // 使用高一半内核空间的起始地址
            vbase = 0xffff800000000000ULL;
        }
    }
    
    // 移动后续的vaddr_objs项为新项腾出空间
    for (int i = vaddrobj_count; i > target_index; i--) {
        vaddr_objs[i] = vaddr_objs[i-1];
    }
    
    // 在vaddr_objs里面新增虚拟内存对象
    vaddr_seg_t& orient_seg = vaddr_objs[target_index];
    
    // 设置子表
    orient_seg.max_num_of_subtb_entries = orient_seg.num_of_subtb_entries = 1;
    orient_seg.subtb = new vaddr_seg_subtb_t[1];
    orient_seg.subtb->phybase = addr;
    orient_seg.subtb->num_of_4kbpgs = numof_4kbpgs;
    
    // 创建访问权限
    pgaccess tmp_access = {0};
    tmp_access.is_kernel = flags.is_kernel;
    tmp_access.is_writeable = flags.is_writable;
    tmp_access.is_executable = flags.is_executable;
    tmp_access.cache_strategy = flags.cache_strateggy;
    tmp_access.is_global = flags.is_global;
    tmp_access.is_occupyied = 1;
    
    // 创建页队列并进行映射
    phymem_pgs_queue* pacage = seg_to_queue(vbase, size_in_byte);
    if(pacage == nullptr) {
        delete[] orient_seg.subtb;
        phymemSubMgr.remap_dec(addr); // 回滚引用计数
        return nullptr;
    }
    
    phyaddr_t result = Inner_fixed_addr_manage(vbase, *pacage, tmp_access, addr, true);
    delete pacage;
    
    if(result == vbase + size_in_byte) {
        // 映射成功，设置段信息
        orient_seg.base = vbase;
        orient_seg.size_in_numof4kbpgs = numof_4kbpgs;
        orient_seg.flags = flags;
        vaddrobj_count++;
        return (void*)vbase;
    } else {
        delete[] orient_seg.subtb;
        phymemSubMgr.remap_dec(addr); // 回滚引用计数
        return nullptr;
    }
}

void *KernelSpacePgsMemMgr::phy_pgs_allocate(uint64_t size_in_byte, uint8_t align_require)
{
    
    return phymemSubMgr.alloc((size_in_byte+4095)>>12,align_require);
}
int KernelSpacePgsMemMgr::fixedaddr_phy_pgs_allocate(phyaddr_t addr, uint64_t size_in_byte)
{
    return phymemSubMgr.fixedaddr_allocate(addr,(size_in_byte+4095)>>12);
}
int KernelSpacePgsMemMgr::free_phy_pgs(phyaddr_t addr)
{
    return phymemSubMgr.free(addr);
}
int KernelSpacePgsMemMgr::pgs_fixedaddr_allocate(IN phyaddr_t addr, IN size_t size_in_byte, pgaccess access)
{
    if(addr&PAGE_OFFSET_MASK[0])return OS_INVALID_ADDRESS;
    if(fixedaddr_phy_pgs_allocate(addr,size_in_byte)!=OS_SUCCESS)return OS_MEMRY_ALLOCATE_FALT;
    size_in_byte += PAGE_OFFSET_MASK[0];
    size_in_byte &= ~PAGE_OFFSET_MASK[0];
    phy_memDescriptor*usage_query_result = queryPhysicalMemoryUsage(addr,size_in_byte);
    if(usage_query_result[1].Type!=EfiReservedMemoryType)
    {
            delete usage_query_result;
            return OS_MEMRY_ALLOCATE_FALT;
     }
    //这个判定是因为如果确认是空闲内存，那么返回的结果必然只有一项，第二项必然是空不是一项可以返回不可分配
    if(usage_query_result[0].Type!=OS_ALLOCATABLE_MEMORY&&
    usage_query_result[0].Attribute&1!=0)
    {
        delete usage_query_result;
        return OS_MEMRY_ALLOCATE_FALT;
    }
    //确定这一项是100%可分配内存，完全空闲
    delete usage_query_result;
    phymem_pgs_queue*queue=seg_to_queue(addr,size_in_byte);
    phyaddr_t alloced_addr= Inner_fixed_addr_manage(addr,*queue,access);
    if (alloced_addr!=addr+size_in_byte)
    {
        return OS_MEMRY_ALLOCATE_FALT;
    }
    delete queue;
    return OS_SUCCESS;
}

int KernelSpacePgsMemMgr::pgs_free(phyaddr_t addr, size_t size_in_byte)
{   
    
        if(addr&PAGE_OFFSET_MASK[0])return OS_INVALID_ADDRESS;
    size_in_byte += PAGE_OFFSET_MASK[0];
    size_in_byte &= ~PAGE_OFFSET_MASK[0];
    phy_memDescriptor*usage_query_result = queryPhysicalMemoryUsage(addr,size_in_byte);
    for(;usage_query_result->Type!=EfiReservedMemoryType;usage_query_result++)
    {
        if(usage_query_result->Type!=OS_ALLOCATABLE_MEMORY)
        {
            kputsSecure("PgsMemMgr::pgs_free:invalid memtype when analyse result from queryPhysicalMemoryUsage\n");    
            return OS_MEMRY_ALLOCATE_FALT;
        }
        if(usage_query_result->Attribute&1!=1)
        {
            kputsSecure("PgsMemMgr::pgs_free:try to free freed memory\n");
        }
    }
    delete usage_query_result;
    phymem_pgs_queue*queue=seg_to_queue(addr,size_in_byte);
    pgaccess access={0};
    access.is_kernel=true;
    access.is_writeable=true;
    access.is_readable=true;
    access.is_executable=false;
    phyaddr_t alloced_addr= Inner_fixed_addr_manage(addr,*queue,access);
    if (alloced_addr!=addr+size_in_byte)return OS_MEMRY_ALLOCATE_FALT;
    phymemSubMgr.free(addr);
    delete queue;
    return OS_SUCCESS;
 }


 KernelSpacePgsMemMgr::phymem_pgs_queue *KernelSpacePgsMemMgr::seg_to_queue(phyaddr_t base,uint64_t size_in_bytes){
    uint64_t end_addr = base + size_in_bytes;
    end_addr+=PAGE_SIZE_IN_LV[0]-1;
    end_addr&=~PAGE_OFFSET_MASK[0];
    phymem_pgs_queue* queue = new phymem_pgs_queue;
    if (queue==nullptr)
    {
        return nullptr;
    }
        phyaddr_t scan_addr = base;
    int queue_index = 0;
    
    // 遍历内存区域，确定最佳的页大小组合
    while (scan_addr < end_addr)
    {
        // 从最大支持的页大小开始尝试
        int pglv_index = Max_huge_pg_index;
        
        // 找到适合当前地址的最大页大小
        while (pglv_index >= 0)
        {
            // 检查地址对齐和是否超出内存区域
            if ((scan_addr & PAGE_OFFSET_MASK[pglv_index]) == 0 && // 地址对齐
                scan_addr + PAGE_SIZE_IN_LV[pglv_index] - 1 <= end_addr) // 不超出内存区域
            {
                break;
            }
            pglv_index--;
        }
        
        if (pglv_index < 0)
        {
            delete queue;
            return nullptr;
        }
        
        // 添加到队列
        if (queue->entry_count == 0 || 
            queue->entry[queue->entry_count - 1].pgs_lv != pglv_index)
        {
            // 新类型的页
            if (queue->entry_count >= 10)
            {
                delete queue;
                return nullptr; // 队列已满
            }
            queue->entry[queue->entry_count].pgs_lv = pglv_index;
            queue->entry[queue->entry_count].pgs_count = 1;
            queue->entry_count++;
        }
        else
        {
            // 相同类型的页，增加计数
            queue->entry[queue->entry_count - 1].pgs_count++;
        }
        
        scan_addr += PAGE_SIZE_IN_LV[pglv_index];
    }
    return queue;
}

