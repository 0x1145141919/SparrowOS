#include "phygpsmemmgr.h"
#include "kpoolmemmgr.h"
#include "Memory.h"
#include "VideoDriver.h"
#include "os_error_definitions.h"
#include "OS_utils.h"
#include "pgtable45.h"
/**
 * 根据权限要求以及对齐要求，大小要求先phymemSubMgr子系统中分配一片连续的虚拟地址空间（高一半内核空间）
 * （优先分配连续物理内存，连续失败就走phymemSubMgr的碎片分配函数）
 * 由于表项按照虚拟基址增大的顺序存放排列，不保存空闲虚拟内存对象，可能有空隙但不可能存在重复空间，
 * 要从前向后扫描一个空闲虚拟地址空间
 * 以及最后使用Inner_fixed_addr_manage分配
*/
void *KernelSpacePgsMemMgr::pgs_allocate_remapped(size_t size_in_byte, pgflags flags, uint8_t align_require)
{
    if(size_in_byte==0)return nullptr;
    size_in_byte+=PAGE_OFFSET_MASK[0];
    size_in_byte&=~PAGE_OFFSET_MASK[0];
    vaddr_t vend;
    uint64_t numof_4kbpgs=size_in_byte>>12;
    if(align_require<12)align_require=12;
    // 修改：从前向后扫描空闲虚拟地址空间，而不是直接在vaddrobj_count处分配
    // 查找第一个合适的空闲虚拟地址空间
    vaddr_t vbase = 0;
    int target_index = vaddrobj_count; // 默认在末尾添加
    
    // 从前向后扫描，查找空闲空间
    for (int i = 0; i < vaddrobj_count; i++) {
        vaddr_t current_end = vaddr_objs[i].base + (vaddr_objs[i].size_in_numof4kbpgs << 12);
        bool found_space = true;
        current_end=align_up(current_end,1ULL<< align_require);
        if(current_end==0){return nullptr;} 
        // 检查下一个对象是否会与当前分配冲突
        if (i + 1 < vaddrobj_count) {
            vaddr_t next_start = vaddr_objs[i+1].base;
            if (current_end + size_in_byte > next_start) {
                found_space = false;
            }
        }
        
        if (found_space) {
            vbase = current_end;
            target_index = i + 1;
            break;
        }
    }
    
    // 如果没有找到空闲空间，则在末尾分配
    if (vbase == 0 && vaddrobj_count > 0) {
        vbase = vaddr_objs[vaddrobj_count-1].base + (vaddr_objs[vaddrobj_count-1].size_in_numof4kbpgs << 12);
        vbase=align_up(vbase,1ULL<< align_require);
    }
    
    // 如果是第一次分配
    if (vaddrobj_count == 0) {
        // 使用高一半内核空间的起始地址
        vbase = 0xffff800000000000ULL;
    }
    
    vend = vbase + size_in_byte;
    
    phyaddr_t phybase=(phyaddr_t)phymemSubMgr.alloc(numof_4kbpgs,align_require);
    
    // 移动后续的vaddr_objs项为新项腾出空间
    for (int i = vaddrobj_count; i > target_index; i--) {
        vaddr_objs[i] = vaddr_objs[i-1];
    }
    
    vaddr_seg_t&orient_seg=vaddr_objs[target_index];
    
    pgaccess tmp_access={0};
    tmp_access.is_kernel=flags.is_kernel;
    tmp_access.is_writeable=flags.is_writable;
    tmp_access.is_executable=flags.is_executable;
    tmp_access.cache_strategy=flags.cache_strateggy;
    tmp_access.is_global    =flags.is_global;
    tmp_access.is_occupyied=1;
    orient_seg.subtb->num_of_4kbpgs=numof_4kbpgs;
    orient_seg.base=vbase;
    orient_seg.flags=flags;
    if((void*)phybase==nullptr){
        orient_seg.subtb=phymemSubMgr.fragments_alloc(numof_4kbpgs,orient_seg.max_num_of_subtb_entries);
        if(orient_seg.subtb==nullptr)return nullptr;
        orient_seg.num_of_subtb_entries=orient_seg.max_num_of_subtb_entries;
        vaddr_t scan_addr=vbase;
        for(uint64_t i=0;i<orient_seg.max_num_of_subtb_entries;i++)
        {
            phymem_pgs_queue*pacage=seg_to_queue(scan_addr,orient_seg.subtb[i].num_of_4kbpgs*PAGE_SIZE_IN_LV[0]);
            if(pacage==nullptr)
            {
                // 在失败时需要释放已分配的subtb
                delete[] orient_seg.subtb;
                return nullptr;
            }
            phyaddr_t result= Inner_fixed_addr_manage(scan_addr,*pacage,tmp_access,phybase,true);
            if(result!=orient_seg.subtb[i].phybase+orient_seg.subtb[i].num_of_4kbpgs*PAGE_SIZE_IN_LV[0])
            {
                delete pacage;
                delete[] orient_seg.subtb;
                kputsSecure("allocate remapped failed");
                return nullptr;
            }
            scan_addr += orient_seg.subtb[i].num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
            delete pacage; // 释放临时队列对象
        }
        orient_seg.base = vbase;
        orient_seg.flags = flags;
        orient_seg.size_in_numof4kbpgs = numof_4kbpgs;
        vaddrobj_count++; // 成功分配后增加计数
        return (void*)vbase;
    }else{
        orient_seg.max_num_of_subtb_entries=orient_seg.num_of_subtb_entries=1;
        orient_seg.subtb=new vaddr_seg_subtb_t[1];
        orient_seg.subtb->phybase=phybase;
        orient_seg.subtb->num_of_4kbpgs=numof_4kbpgs; // 缺失的赋值
        
        phymem_pgs_queue*pacage=seg_to_queue(vbase,size_in_byte);
        if (pacage==nullptr)
        {
           delete[] orient_seg.subtb; // 失败时释放subtb
           return nullptr;
        }
        phyaddr_t result= Inner_fixed_addr_manage(vbase,*pacage,tmp_access,phybase,true);
        delete pacage; // 释放临时队列对象
        if(result==vend){
            orient_seg.base = vbase;
            orient_seg.flags = flags;
            orient_seg.size_in_numof4kbpgs = numof_4kbpgs;
            vaddrobj_count++; // 成功分配后增加计数
            return (void*)vbase;
        } else {
            delete[] orient_seg.subtb; // 映射失败也要释放
            return nullptr;
        }
    }
     
    return nullptr;
}
/*
根据设计，一个虚拟地址只会映射到一个物理地址，但是一个物理地址可能映射到多个虚拟地址
这个地址转换函数负责虚拟地址转换，原理是查询vaddr_objs[4096]以及其子表的数据结构
*/
void* KernelSpacePgsMemMgr::v_to_phyaddrtraslation(vaddr_t vaddr) {
    // 遍历vaddr_objs数组查找包含该虚拟地址的段
    for (int i = 0; i < vaddrobj_count; i++) {
        vaddr_seg_t& seg = vaddr_objs[i];
        
        // 检查虚拟地址是否在当前段范围内
        if (vaddr >= seg.base && vaddr < seg.base + (seg.size_in_numof4kbpgs << 12)) {
            // 如果该段有子表（碎片分配）
            if (seg.num_of_subtb_entries > 0 && seg.subtb != nullptr) {
                // 遍历子表查找具体映射
                vaddr_t current_base = seg.base;
                for (int j = 0; j < seg.num_of_subtb_entries; j++) {
                    vaddr_seg_subtb_t& sub_seg = seg.subtb[j];
                    vaddr_t sub_seg_end = current_base + (sub_seg.num_of_4kbpgs << 12);
                    
                    // 检查虚拟地址是否在当前子段范围内
                    if (vaddr >= current_base && vaddr < sub_seg_end) {
                        // 计算在当前子段中的偏移量
                        uint64_t offset = vaddr - current_base;
                        return (void*)(sub_seg.phybase + offset);
                    }
                    
                    current_base = sub_seg_end;
                }
            } else {
                // 连续分配情况：直接计算偏移量
                uint64_t offset = vaddr - seg.base;
                if (seg.subtb != nullptr) {
                    return (void*)(seg.subtb[0].phybase + offset);
                }
            }
            // 找到对应段但未找到具体映射
            return nullptr;
        }
    }
    
    // 未找到包含该虚拟地址的段，尝试通过页表查询
    PgControlBlockHeader& lv1_entry = PgCBtb_lv1_entry_query(vaddr);
    if (lv1_entry.flags.is_exist && !lv1_entry.flags.is_atom) {
        uint16_t lv0_index = (vaddr & 0x1FF000) >> 12;  // PT_INDEX_MASK_lv0
        PgControlBlockHeader& lv0_entry = lv1_entry.base.lowerlvPgCBtb->entries[lv0_index];
        if (lv0_entry.flags.is_exist && lv0_entry.flags.is_atom) {
            phyaddr_t phy_addr = lv0_entry.base.base_phyaddr;
            phy_addr += (vaddr & 0xFFF);  // 加上页内偏移
            return (void*)phy_addr;
        }
    }
    
    // 未找到映射关系
    return nullptr;
}

/**
 * 确定物理地址尝试分配一个连续的物理内存空间返回一个分配的连续的虚拟地址空间
 * 根据权限要求，物理地址基址，大小要求先phymemSubMgr子系统中使用固定物理地址分配器尝试分配物理内存，再分配一片连续的虚拟地址空间（高一半内核空间）
 * 由于表项按照虚拟基址增大的顺序存放排列，不保存空闲虚拟内存对象，可能有空隙但不可能存在重复空间，
 *  要从前向后扫描一个空闲虚拟地址空间
 * 以及最后使用Inner_fixed_addr_manage分配
 * 同一个物理地址空间不可以被重复分配，但可以被多次重映射
 */
void* KernelSpacePgsMemMgr::pgs_fixedaddr_allocate_remapped(IN phyaddr_t addr, IN size_t size_in_byte, IN pgaccess access)
{
    if(size_in_byte == 0) return nullptr;
    
    // 对齐大小到页边界
    size_in_byte += PAGE_OFFSET_MASK[0];
    size_in_byte &= ~PAGE_OFFSET_MASK[0];
    uint64_t numof_4kbpgs = size_in_byte >> 12;
    
    // 尝试使用固定物理地址分配器分配物理内存
    int alloc_result = phymemSubMgr.fixedaddr_allocate(addr, size_in_byte);
    if(alloc_result != 0) {
        return nullptr; // 分配失败
    }
    
    // 修改：从前向后扫描空闲虚拟地址空间，而不是直接在vaddrobj_count处分配
    // 查找第一个合适的空闲虚拟地址空间
    vaddr_t vbase = 0;
    int target_index = vaddrobj_count; // 默认在末尾添加
    
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
    
    // 移动后续的vaddr_objs项为新项腾出空间
    for (int i = vaddrobj_count; i > target_index; i--) {
        vaddr_objs[i] = vaddr_objs[i-1];
    }
    
    // 在虚拟地址空间中分配段
    vaddr_seg_t& orient_seg = vaddr_objs[target_index];
    
    // 设置子表
    orient_seg.max_num_of_subtb_entries = orient_seg.num_of_subtb_entries = 1;
    orient_seg.subtb = new vaddr_seg_subtb_t[1];
    orient_seg.subtb->phybase = addr;
    orient_seg.subtb->num_of_4kbpgs = numof_4kbpgs;
    
    // 创建页队列并进行映射
    phymem_pgs_queue* pacage = seg_to_queue(vbase, size_in_byte);
    if(pacage == nullptr) {
        delete[] orient_seg.subtb;
        return nullptr;
    }
    
    phyaddr_t result = Inner_fixed_addr_manage(vbase, *pacage, access, addr, true);
    delete pacage;
    
    if(result == vbase + size_in_byte) {
        // 映射成功，设置段信息
        orient_seg.base = vbase;
        orient_seg.size_in_numof4kbpgs = numof_4kbpgs;
        orient_seg.flags.is_kernel = 1;
        orient_seg.flags.is_exist = 1;
        orient_seg.flags.is_occupied = 1;
        orient_seg.flags.is_remaped = 1;
        vaddrobj_count++;
        return (void*)vbase;
    } else {
        delete[] orient_seg.subtb;
        return nullptr;
    }
}
/**
 * 查询基址为addr的虚拟内存段
 * 若查询到：
 * 先释放虚拟地址空间，再尝试释放物理地址空间
 * 只有指向的物理段的引用数降为0时才释放物理地址空间
 * 其中重定位数目只由phymemSubMgr.allocatable_mem_seg下表项中子表的引用数决定
 * 其他数据结构的描述无效
 * 由此只能使用phymemSubMgr的接口处理物理内存相关事务
 * 虚拟地址空间的删除使用线性表删除函数
 */
int KernelSpacePgsMemMgr::pgs_remapped_free(vaddr_t addr)
{
    // 查找虚拟地址段
    for (uint16_t i = 0; i < vaddrobj_count; i++) {
        vaddr_seg_t& seg = vaddr_objs[i];
        
        // 检查地址是否在该段中
        if (addr == seg.base ) {
            // 找到对应的虚拟地址段，开始释放物理内存
            for (uint16_t j = 0; j < seg.num_of_subtb_entries; j++) {
                vaddr_seg_subtb_t& sub_seg = seg.subtb[j];
                // 减少物理内存段的引用计数
                int remap_result = phymemSubMgr.remap_dec(sub_seg.phybase);
                
                // 如果引用计数降到0，尝试释放物理内存
                if (remap_result == OS_SUCCESS) {
                    // 检查是否可以释放物理内存
                    int free_result = phymemSubMgr.free(sub_seg.phybase);
                    // free_result为OS_SUCCESS表示物理内存已释放
                    // free_result为OS_TARTET_BUSY表示还有其他引用
                    // free_result为OS_INVALID_ADDRESS表示未找到物理内存段
                }
                // 如果remap_result为OS_INVALID_ADDRESS或OS_INVALID_PARAMETER，继续处理其他段
            }
            
            // 释放虚拟地址段的子表
            if (seg.subtb) {
                delete[] seg.subtb;
                seg.subtb = nullptr;
            }
            
            // 使用线性表删除函数删除虚拟地址空间
            linearTBSerialDelete(
                (uint64_t*)&vaddrobj_count,
                i, i,
                vaddr_objs,
                sizeof(vaddr_seg_t)
            );
            
            return OS_SUCCESS;
        }
    }
    
    // 未找到对应的虚拟地址段
    return OS_INVALID_ADDRESS;
}
