#include "phygpsmemmgr.h"
#include "kpoolmemmgr.h"
#include "Memory.h"
#include "VideoDriver.h"
#include "os_error_definitions.h"
#include "OS_utils.h"
/*
此函数在phymemSegsSubMgr_t子系统中通过扫描allocatable_mem_seg数组
（按照物理基址从小到大排序，有效性由allocatable_mem_seg_count决定，引索小于的就是有效条目）
每个成员都有一个下级表指针，下级表的最大数目与实际数目由成员max_num_of_subtb_entries与
决定num_of_subtb_entries，存储的是被占用的物理内存段，表项按照物理基址从小到大排列
对于足够大的物理内存段，从物理地址从小到大顺序扫描，找到第一个满足条件的物理内存段，并返回该物理内存段的物理基址
显然，初分配内存段的重映射数会为0
若失败返回nullptr
*/
void *KernelSpacePgsMemMgr::phymemSegsSubMgr_t::alloc(uint64_t num_of_4kbpgs, uint8_t align_require)
{
    // 计算对齐要求（2的幂次）
    phyaddr_t alignment = 1ULL << align_require;
    phyaddr_t alignment_mask = alignment - 1;

    for(int i=0; i < allocatable_mem_seg_count; i++)
    {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        if(seg.size_in_numof4kbpgs >= num_of_4kbpgs){
            if(seg.num_of_subtb_entries == seg.max_num_of_subtb_entries){
                continue;
            }
            
            // 检查第一个空隙（在第一个已分配段之前）
            if(seg.num_of_subtb_entries > 0) {
                phyaddr_t available_size = seg.subtb[0].base - seg.base;
                if(available_size >= num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) {
                    // 在第一个段之前分配，但需要满足对齐要求
                    phyaddr_t candidate_addr = seg.base;
                    // 如果需要对齐，则调整地址
                    if (candidate_addr & alignment_mask) {
                        candidate_addr = (candidate_addr + alignment_mask) & ~alignment_mask;
                    }
                    
                    // 检查调整后的地址是否仍然在空隙内
                    if ((candidate_addr + num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) <= seg.subtb[0].base) {
                        minimal_phymem_seg_t new_entry;
                        new_entry.base = candidate_addr;
                        new_entry.num_of_4kbpgs = num_of_4kbpgs;
                        new_entry.remapped_count = 0;
                        linearTBSerialInsert(&seg.num_of_subtb_entries,
                            0, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                        return (void*)new_entry.base;
                    }
                }
            } else {
                // 如果没有已分配段，直接在开始位置分配，但需要满足对齐要求
                phyaddr_t candidate_addr = seg.base;
                // 如果需要对齐，则调整地址
                if (candidate_addr & alignment_mask) {
                    candidate_addr = (candidate_addr + alignment_mask) & ~alignment_mask;
                }
                
                // 检查调整后的地址是否仍然在段内
                if ((candidate_addr + num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) <= 
                    (seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0])) {
                    minimal_phymem_seg_t new_entry;
                    new_entry.base = candidate_addr;
                    new_entry.num_of_4kbpgs = num_of_4kbpgs;
                    new_entry.remapped_count = 0;
                    linearTBSerialInsert(&seg.num_of_subtb_entries,
                        0, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                    return (void*)new_entry.base;
                }
            }
            
            // 检查中间的空隙
            for(int j = 0; j < seg.num_of_subtb_entries - 1; j++){
                phyaddr_t gap_start = seg.subtb[j].base + seg.subtb[j].num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
                phyaddr_t gap_end = seg.subtb[j+1].base;
                phyaddr_t gap_size = gap_end - gap_start;
                
                if(gap_size >= num_of_4kbpgs * PAGE_SIZE_IN_LV[0]){
                    // 在空隙中寻找满足对齐要求的地址
                    phyaddr_t candidate_addr = gap_start;
                    // 如果需要对齐，则调整地址
                    if (candidate_addr & alignment_mask) {
                        candidate_addr = (candidate_addr + alignment_mask) & ~alignment_mask;
                    }
                    
                    // 检查调整后的地址是否仍然在空隙内
                    if ((candidate_addr + num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) <= gap_end) {
                        minimal_phymem_seg_t new_entry;
                        new_entry.base = candidate_addr;
                        new_entry.num_of_4kbpgs = num_of_4kbpgs;
                        new_entry.remapped_count = 0;
                        linearTBSerialInsert(&seg.num_of_subtb_entries,
                            j + 1, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                        return (void*)new_entry.base;
                    }
                }
            }
            
            // 检查最后一个段之后的空隙
            minimal_phymem_seg_t& last_seg = seg.subtb[seg.num_of_subtb_entries - 1];
            phyaddr_t last_seg_end = last_seg.base + last_seg.num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
            phyaddr_t seg_end = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
            
            if(seg_end - last_seg_end >= num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) {
                // 在最后一个段之后分配，但需要满足对齐要求
                phyaddr_t candidate_addr = last_seg_end;
                // 如果需要对齐，则调整地址
                if (candidate_addr & alignment_mask) {
                    candidate_addr = (candidate_addr + alignment_mask) & ~alignment_mask;
                }
                
                // 检查调整后的地址是否仍然在段内
                if ((candidate_addr + num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) <= seg_end) {
                    minimal_phymem_seg_t new_entry;
                    new_entry.base = candidate_addr;
                    new_entry.num_of_4kbpgs = num_of_4kbpgs;
                    new_entry.remapped_count = 0;
                    linearTBSerialInsert(&seg.num_of_subtb_entries,
                        seg.num_of_subtb_entries, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                    return (void*)new_entry.base;
                }
            }
        }
    }
    return nullptr;
}
minimal_phymem_seg_t* KernelSpacePgsMemMgr::phymemSegsSubMgr_t::memsegs_usage_query(phyaddr_t base,uint64_t offset,uint64_t&result_entry_count){
    phyaddr_t query_start = base;
    phyaddr_t query_end = base + offset;
    result_entry_count = 0;
    
    // 首先计算需要的段数量上限
    // 最坏情况下，每个可分配段及其所有子段都可能产生多个结果段
    uint64_t max_segments = 1; // 至少一个段
    for(int i = 0; i < allocatable_mem_seg_count; i++) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        // 每个可分配段最多可能产生: 1(段本身) + 子段数 + 1(最后一个子段后的空隙)
        max_segments += 2 + seg.num_of_subtb_entries;
    }
    
    // 创建结果数组
    minimal_phymem_seg_t* result = new minimal_phymem_seg_t[max_segments];
    if(result == nullptr) {
        return nullptr;
    }
    
    uint64_t result_index = 0;
    
    // 遍历所有可分配内存段
    for(int i = 0; i < allocatable_mem_seg_count; i++) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        phyaddr_t seg_start = seg.base;
        phyaddr_t seg_end = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
        
        // 检查段是否与查询范围有交集
        if(seg_start >= query_end || seg_end <= query_start) {
            // 段在查询范围外，跳过
            continue;
        }
        
        // 调整段的起始和结束位置到查询范围内
        phyaddr_t current_start = seg_start < query_start ? query_start : seg_start;
        phyaddr_t current_end = seg_end > query_end ? query_end : seg_end;
        
        // 如果该段没有任何子段，整个段都是FREE
        if(seg.num_of_subtb_entries == 0) {
            result[result_index].base = current_start;
            result[result_index].num_of_4kbpgs = (current_end - current_start) / PAGE_SIZE_IN_LV[0];
            result[result_index].type = FREE;
            result[result_index].remapped_count = 0;
            result_index++;
            continue;
        }
        
        // 处理有子段的情况
        phyaddr_t scan_pos = current_start;
        
        // 检查第一个子段之前是否有空闲区域
        if(seg.subtb[0].base > scan_pos) {
            phyaddr_t free_end = seg.subtb[0].base > current_end ? current_end : seg.subtb[0].base;
            result[result_index].base = scan_pos;
            result[result_index].num_of_4kbpgs = (free_end - scan_pos) / PAGE_SIZE_IN_LV[0];
            result[result_index].type = FREE;
            result[result_index].remapped_count = 0;
            result_index++;
            scan_pos = free_end;
        }
        
        // 遍历子段，处理占用和空闲区域
        for(uint64_t j = 0; j < seg.num_of_subtb_entries; j++) {
            minimal_phymem_seg_t& sub_seg = seg.subtb[j];
            phyaddr_t sub_seg_start = sub_seg.base;
            phyaddr_t sub_seg_end = sub_seg.base + sub_seg.num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
            
            // 检查子段是否与当前扫描区域有交集
            if(sub_seg_start >= current_end || sub_seg_end <= scan_pos) {
                continue;
            }
            
            // 调整子段范围到当前扫描区域
            phyaddr_t actual_sub_start = sub_seg_start < scan_pos ? scan_pos : sub_seg_start;
            phyaddr_t actual_sub_end = sub_seg_end > current_end ? current_end : sub_seg_end;
            
            // 如果在子段之前有空闲区域
            if(actual_sub_start > scan_pos) {
                result[result_index].base = scan_pos;
                result[result_index].num_of_4kbpgs = (actual_sub_start - scan_pos) / PAGE_SIZE_IN_LV[0];
                result[result_index].type = FREE;
                result[result_index].remapped_count = 0;
                result_index++;
            }
            
            // 添加占用段
            result[result_index].base = actual_sub_start;
            result[result_index].num_of_4kbpgs = (actual_sub_end - actual_sub_start) / PAGE_SIZE_IN_LV[0];
            result[result_index].type = OCCUPYIED;
            result[result_index].remapped_count = sub_seg.remapped_count;
            result_index++;
            
            scan_pos = actual_sub_end;
        }
        
        // 检查最后一个子段后是否有空闲区域
        if(scan_pos < current_end) {
            result[result_index].base = scan_pos;
            result[result_index].num_of_4kbpgs = (current_end - scan_pos) / PAGE_SIZE_IN_LV[0];
            result[result_index].type = FREE;
            result[result_index].remapped_count = 0;
            result_index++;
        }
    }
    
    // 如果没有任何匹配的可分配段，则整个区域为RESERVED
    if(result_index == 0) {
        result[result_index].base = query_start;
        result[result_index].num_of_4kbpgs = offset / PAGE_SIZE_IN_LV[0];
        result[result_index].type = RESERVED;
        result[result_index].remapped_count = 0;
        result_index++;
    }
    
    result_entry_count = result_index;
    return result;
}
bool KernelSpacePgsMemMgr::phymemSegsSubMgr_t::is_addrrange_have_intersect(IN phyaddr_t base, IN size_t numof_4kbpgs, IN minimal_phymem_seg_t subtb)
{
    // 计算给定地址范围的结束地址
    phyaddr_t end_addr = base + numof_4kbpgs * PAGE_SIZE_IN_LV[0];
    // 计算subtb描述的内存段的结束地址
    phyaddr_t subtb_end_addr = subtb.base + subtb.num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
    
    // 检查两个地址范围是否有交集
    // 如果给定范围的起始地址小于subtb段的结束地址 且 给定范围的结束地址大于subtb段的起始地址，则有交集
    if (base < subtb_end_addr && end_addr > subtb.base) {
        return true;
    }
    return false;
}
/*
此函数在phymemSegsSubMgr_t子系统中通过扫描allocatable_mem_seg数组
（按照物理基址从小到大排序，有效性由allocatable_mem_seg_count决定，引索小于的就是有效条目）
每个成员都有一个下级表指针，下级表的最大数目与实际数目由成员max_num_of_subtb_entries与
决定num_of_subtb_entries，存储的是被占用的物理内存段，表项按照物理基址从小到大排列
由于数据结构的特性，应该使用二分查找来查询描述的物理内存段是否在空隙中
若确实在空隙中，则分配，返回成功，若不是，则返回失败
当然如果地址为查询到，应该返回OS_INVALID_ADDRESS的错误码
*/
int KernelSpacePgsMemMgr::phymemSegsSubMgr_t::fixedaddr_allocate(IN phyaddr_t base, IN size_t num_of_4kbpgs)
{
    // 检查参数有效性
    if (num_of_4kbpgs == 0) {
        return OS_BAD_FUNCTION;
    }

    for(int i = 0; i < allocatable_mem_seg_count; i++)
    {
        auto& seg = allocatable_mem_seg[i];
        // 计算当前内存段的结束物理地址
        phyaddr_t seg_end_addr = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
        // 计算请求的内存区域的结束物理地址
        phyaddr_t req_end_addr = base + num_of_4kbpgs * PAGE_SIZE_IN_LV[0];

        // 检查请求的地址范围是否完全包含在当前内存段内
        if (seg.base <= base && req_end_addr <= seg_end_addr)
        {
            // 检查子表项是否已满
            if (seg.num_of_subtb_entries == seg.max_num_of_subtb_entries)
            {
                kputsSecure("fixedaddr_allocate: subtable full\n");
                return OS_BAD_FUNCTION;
            }
            
            minimal_phymem_seg_t* subtb = seg.subtb;
            
            // 使用二分查找确定插入位置并检查冲突
            int left = 0;
            int right = seg.num_of_subtb_entries - 1;
            int insert_pos = seg.num_of_subtb_entries; // 默认插入到末尾
            
            // 二分查找第一个基地址大于base的项
            while (left <= right) {
                int mid = (left + right) / 2;
                // 检查是否与当前项有交集
                if (is_addrrange_have_intersect(base, num_of_4kbpgs, subtb[mid])) {
                    return OS_BAD_FUNCTION;
                }
                
                if (subtb[mid].base > base) {
                    insert_pos = mid;
                    right = mid - 1;
                } else {
                    left = mid + 1;
                }
            }
            
            // 检查insert_pos之前的项是否与请求范围有交集
            if (insert_pos > 0 && is_addrrange_have_intersect(base, num_of_4kbpgs, subtb[insert_pos - 1])) {
                return OS_BAD_FUNCTION;
            }
            
            // 创建新的内存段描述符
            minimal_phymem_seg_t new_entry;
            new_entry.base = base;
            new_entry.num_of_4kbpgs = num_of_4kbpgs;
            
            // 将新项插入到子表的指定位置
            linearTBSerialInsert(
                &seg.num_of_subtb_entries,
                insert_pos,
                &new_entry,
                subtb,
                sizeof(minimal_phymem_seg_t)
            );
            
            return OS_SUCCESS;
        }
    }
    return OS_INVALID_ADDRESS;
}
/*
此函数在phymemSegsSubMgr_t子系统中通过扫描allocatable_mem_seg数组
（按照物理基址从小到大排序，有效性由allocatable_mem_seg_count决定，引索小于的就是有效条目）
每个成员都有一个下级表指针，下级表的最大数目与实际数目由成员max_num_of_subtb_entries与
决定num_of_subtb_entries，存储的是被占用的物理内存段，表项按照物理基址从小到大排列
查询基址的内存段，在只有重映射数目为0的时候并删除该内存段
非零则返回错误码OS_TARTET_BUSY
若不存在该内存段，则返回OS_INVALID_ADDRESS的错误码
内存循环应该使用二分查找优化
*/
int KernelSpacePgsMemMgr::phymemSegsSubMgr_t::free(phyaddr_t base)
{
    // 使用二分查找优化外层循环
    int left = 0, right = allocatable_mem_seg_count - 1;
    int i = -1;
    
    // 二分查找找到可能包含base的内存段
    while (left <= right) {
        int mid = (left + right) / 2;
        allocatable_mem_seg_t& seg = allocatable_mem_seg[mid];
        phyaddr_t seg_end = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
        
        if (base < seg.base) {
            right = mid - 1;
        } else if (base >= seg_end) {
            left = mid + 1;
        } else {
            i = mid;
            break;
        }
    }
    
    // 如果找到了包含base的内存段
    if (i != -1) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        
        // 使用二分查找在subtb中查找base
        int sub_left = 0, sub_right = seg.num_of_subtb_entries - 1;
        while (sub_left <= sub_right) {
            int mid = (sub_left + sub_right) / 2;
            if (seg.subtb[mid].base == base) {
                // 检查remapped_count，只有为0时才能释放
                if (seg.subtb[mid].remapped_count == 0) {
                    linearTBSerialDelete(
                        &seg.num_of_subtb_entries,
                        mid, mid,
                        seg.subtb,
                        sizeof(minimal_phymem_seg_t)
                    );
                    return OS_SUCCESS;
                } else {
                    // remapped_count不为0，返回错误码
                    return OS_TARTET_BUSY;
                }
            } else if (seg.subtb[mid].base < base) {
                sub_left = mid + 1;
            } else {
                sub_right = mid - 1;
            }
        }
    }
    
    return OS_INVALID_ADDRESS;
}

/*
 * 碎片分配函数，从低地址0x100000开始扫描，分配每一个内存碎片，如果最后分配到了足够的空间就交返回数组地址
 * 反之返回空指针
 */
vaddr_seg_subtb_t* KernelSpacePgsMemMgr::phymemSegsSubMgr_t::fragments_alloc
(uint64_t numof_4kbpgs,OUT uint16_t& allocated_count)
{
    // 如果请求数量为0，直接返回空指针
    if (numof_4kbpgs == 0) {
        return nullptr;
    }

    // 创建结果数组，使用固定大小（最多支持4096个碎片）
    const uint64_t MAX_FRAGMENTS = 4096;
    vaddr_seg_subtb_t* result = new vaddr_seg_subtb_t[MAX_FRAGMENTS];
    if (result == nullptr) {
        return nullptr;
    }
    uint64_t remaining_pages = numof_4kbpgs;

    // 遍历所有可分配内存段
    for (int i = 0; i < allocatable_mem_seg_count && remaining_pages > 0; i++) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        
        // 如果该段已满，跳过
        if (seg.num_of_subtb_entries == seg.max_num_of_subtb_entries) {
            continue;
        }

        // 检查第一个空隙（在第一个已分配段之前）
        if (seg.num_of_subtb_entries > 0) {
            phyaddr_t available_size = seg.subtb[0].base - seg.base;
            uint64_t pages_in_gap = available_size >> 12; // 除以PAGE_SIZE_IN_LV[0](4096)
            
            if (pages_in_gap > 0) {
                // 检查是否超出固定数组容量
                if (allocated_count >= MAX_FRAGMENTS) {
                    delete[] result;
                    return nullptr; // 超出最大碎片数限制
                }
                
                // 如果空隙大小大于等于剩余需分配的页数，则只分配剩余页数
                // 否则分配整个空隙
                uint64_t pages_to_alloc = (pages_in_gap < remaining_pages) ? 
                                          pages_in_gap : remaining_pages;
                
                // 在物理内存管理器中记录分配
                minimal_phymem_seg_t new_entry;
                new_entry.base = seg.base;
                new_entry.num_of_4kbpgs = pages_to_alloc;
                new_entry.remapped_count = 0;
                linearTBSerialInsert(&seg.num_of_subtb_entries,
                    0, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                
                // 记录到结果数组
                result[allocated_count].phybase = seg.base;
                result[allocated_count].num_of_4kbpgs = pages_to_alloc;
                allocated_count++;
                
                // 更新剩余需分配页数
                remaining_pages -= pages_to_alloc;
            }
        } else {
            // 如果没有已分配段，检查整个段的大小
            uint64_t pages_in_seg = seg.size_in_numof4kbpgs;
            
            if (pages_in_seg > 0) {
                // 检查是否超出固定数组容量
                if (allocated_count >= MAX_FRAGMENTS) {
                    delete[] result;
                    return nullptr; // 超出最大碎片数限制
                }
                
                // 如果段大小大于等于剩余需分配的页数，则只分配剩余页数
                // 否则分配整个段
                uint64_t pages_to_alloc = (pages_in_seg < remaining_pages) ? 
                                          pages_in_seg : remaining_pages;
                
                // 在物理内存管理器中记录分配
                minimal_phymem_seg_t new_entry;
                new_entry.base = seg.base;
                new_entry.num_of_4kbpgs = pages_to_alloc;
                new_entry.remapped_count = 0;
                linearTBSerialInsert(&seg.num_of_subtb_entries,
                    0, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                
                // 记录到结果数组
                result[allocated_count].phybase = seg.base;
                result[allocated_count].num_of_4kbpgs = pages_to_alloc;
                allocated_count++;
                
                // 更新剩余需分配页数
                remaining_pages -= pages_to_alloc;
            }
        }

        // 如果还没分配完所需页数，检查中间的空隙
        if (remaining_pages > 0 && seg.num_of_subtb_entries > 0) {
            for (int j = 0; j < seg.num_of_subtb_entries - 1 && remaining_pages > 0; j++) {
                phyaddr_t gap_start = seg.subtb[j].base + (seg.subtb[j].num_of_4kbpgs << 12);
                phyaddr_t gap_end = seg.subtb[j + 1].base;
                uint64_t gap_size = gap_end - gap_start;
                uint64_t pages_in_gap = gap_size >> 12;
                
                if (pages_in_gap > 0) {
                    // 检查是否超出固定数组容量
                    if (allocated_count >= MAX_FRAGMENTS) {
                        delete[] result;
                        return nullptr; // 超出最大碎片数限制
                    }
                    
                    // 如果空隙大小大于等于剩余需分配的页数，则只分配剩余页数
                    // 否则分配整个空隙
                    uint64_t pages_to_alloc = (pages_in_gap < remaining_pages) ? 
                                              pages_in_gap : remaining_pages;
                    
                    // 在物理内存管理器中记录分配
                    minimal_phymem_seg_t new_entry;
                    new_entry.base = gap_start;
                    new_entry.num_of_4kbpgs = pages_to_alloc;
                    new_entry.remapped_count = 0;
                    linearTBSerialInsert(&seg.num_of_subtb_entries,
                        j + 1, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                    
                    // 记录到结果数组
                    result[allocated_count].phybase = gap_start;
                    result[allocated_count].num_of_4kbpgs = pages_to_alloc;
                    allocated_count++;
                    
                    // 更新剩余需分配页数
                    remaining_pages -= pages_to_alloc;
                }
            }
        }

        // 检查最后一个段之后的空隙
        if (remaining_pages > 0 && seg.num_of_subtb_entries > 0) {
            minimal_phymem_seg_t& last_seg = seg.subtb[seg.num_of_subtb_entries - 1];
            phyaddr_t last_seg_end = last_seg.base + (last_seg.num_of_4kbpgs << 12);
            phyaddr_t seg_end = seg.base + (seg.size_in_numof4kbpgs << 12);
            uint64_t pages_after_last = (seg_end - last_seg_end) >> 12;
            
            if (pages_after_last > 0) {
                // 检查是否超出固定数组容量
                if (allocated_count >= MAX_FRAGMENTS) {
                    delete[] result;
                    return nullptr; // 超出最大碎片数限制
                }
                
                // 如果空隙大小大于等于剩余需分配的页数，则只分配剩余页数
                // 否则分配整个空隙
                uint64_t pages_to_alloc = (pages_after_last < remaining_pages) ? 
                                          pages_after_last : remaining_pages;
                
                // 在物理内存管理器中记录分配
                minimal_phymem_seg_t new_entry;
                new_entry.base = last_seg_end;
                new_entry.num_of_4kbpgs = pages_to_alloc;
                new_entry.remapped_count = 0;
                linearTBSerialInsert(&seg.num_of_subtb_entries,
                    seg.num_of_subtb_entries, &new_entry, seg.subtb, sizeof(minimal_phymem_seg_t));
                
                // 记录到结果数组
                result[allocated_count].phybase = last_seg_end;
                result[allocated_count].num_of_4kbpgs = pages_to_alloc;
                allocated_count++;
                
                // 更新剩余需分配页数
                remaining_pages -= pages_to_alloc;
            }
        }
    }

    // 如果成功分配了所有请求的页，返回结果数组
    if (remaining_pages == 0) {
        result=(vaddr_seg_subtb_t*)gKpoolmemmgr.realloc(result,allocated_count*sizeof(vaddr_seg_subtb_t));
        if(result==nullptr)kputsSecure("KernelSpacePgsMemMgr::phy_pgs_allocate_remapped: Out of memory");
        return result;
    } else {
        // 分配失败，释放已分配的内存
        delete[] result;
        return nullptr;
    }
}

/*
查询是否有对应基址的占用内存段并在重映射计数上加一
自然不能超过0xffff
*/
int KernelSpacePgsMemMgr::phymemSegsSubMgr_t::remap_inc(phyaddr_t base)
{
    // 使用二分查找优化外层循环
    int left = 0, right = allocatable_mem_seg_count - 1;
    int i = -1;
    
    // 二分查找找到可能包含base的内存段
    while (left <= right) {
        int mid = (left + right) / 2;
        allocatable_mem_seg_t& seg = allocatable_mem_seg[mid];
        phyaddr_t seg_end = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
        
        if (base < seg.base) {
            right = mid - 1;
        } else if (base >= seg_end) {
            left = mid + 1;
        } else {
            i = mid;
            break;
        }
    }
    
    // 如果找到了包含base的内存段
    if (i != -1) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        
        // 使用二分查找在subtb中查找base
        int sub_left = 0, sub_right = seg.num_of_subtb_entries - 1;
        while (sub_left <= sub_right) {
            int mid = (sub_left + sub_right) / 2;
            if (seg.subtb[mid].base == base) {
                // 检查remapped_count是否已经达到最大值
                if (seg.subtb[mid].remapped_count == 0xFFFF) {
                    return OS_TARTET_BUSY; // 达到最大值，返回错误码
                }
                // 增加重映射计数
                seg.subtb[mid].remapped_count++;
                return OS_SUCCESS;
            } else if (seg.subtb[mid].base < base) {
                sub_left = mid + 1;
            } else {
                sub_right = mid - 1;
            }
        }
    }
    
    return OS_INVALID_ADDRESS;
}

/*
查询是否有对应基址的占用内存段并在重映射计数上减一
自然不能小于0
*/
int KernelSpacePgsMemMgr::phymemSegsSubMgr_t::remap_dec(phyaddr_t base)
{
    // 使用二分查找优化外层循环
    int left = 0, right = allocatable_mem_seg_count - 1;
    int i = -1;
    
    // 二分查找找到可能包含base的内存段
    while (left <= right) {
        int mid = (left + right) / 2;
        allocatable_mem_seg_t& seg = allocatable_mem_seg[mid];
        phyaddr_t seg_end = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
        
        if (base < seg.base) {
            right = mid - 1;
        } else if (base >= seg_end) {
            left = mid + 1;
        } else {
            i = mid;
            break;
        }
    }
    
    // 如果找到了包含base的内存段
    if (i != -1) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        
        // 使用二分查找在subtb中查找base
        int sub_left = 0, sub_right = seg.num_of_subtb_entries - 1;
        while (sub_left <= sub_right) {
            int mid = (sub_left + sub_right) / 2;
            if (seg.subtb[mid].base == base) {
                // 检查remapped_count是否已经为0
                if (seg.subtb[mid].remapped_count == 0) {
                    return OS_INVALID_PARAMETER; // 已经为0，返回错误码
                }
                // 减少重映射计数
                seg.subtb[mid].remapped_count--;
                return OS_SUCCESS;
            } else if (seg.subtb[mid].base < base) {
                sub_left = mid + 1;
            } else {
                sub_right = mid - 1;
            }
        }
    }
    
    return OS_INVALID_ADDRESS;
}

minimal_phymem_seg_t KernelSpacePgsMemMgr::phymemSegsSubMgr_t::addr_query(phyaddr_t phyaddr)
{
    // 检查地址是否高于最大物理地址
    if (phyaddr > gBaseMemMgr.getMaxPhyaddr()) {
        minimal_phymem_seg_t result;
        result.base = phyaddr;
        result.num_of_4kbpgs = 1;
        result.type = NOT_EXIST;
        result.remapped_count = 0;
        return result;
    }
    
    // 遍历所有可分配内存段
    for (int i = 0; i < allocatable_mem_seg_count; i++) {
        allocatable_mem_seg_t& seg = allocatable_mem_seg[i];
        phyaddr_t seg_start = seg.base;
        phyaddr_t seg_end = seg.base + seg.size_in_numof4kbpgs * PAGE_SIZE_IN_LV[0];
        
        // 检查查询的地址是否在当前段范围内
        if (phyaddr >= seg_start && phyaddr < seg_end) {
            // 如果该段没有任何子段，整个段都是FREE
            if (seg.num_of_subtb_entries == 0) {
                minimal_phymem_seg_t result;
                result.base = phyaddr;
                result.num_of_4kbpgs = seg.size_in_numof4kbpgs;
                result.type = FREE;
                result.remapped_count = 0;
                return result;
            }
            
            // 使用二分查找在子段中查找
            int left = 0;
            int right = seg.num_of_subtb_entries - 1;
            
            while (left <= right) {
                int mid = (left + right) / 2;
                minimal_phymem_seg_t& sub_seg = seg.subtb[mid];
                phyaddr_t sub_seg_start = sub_seg.base;
                phyaddr_t sub_seg_end = sub_seg.base + sub_seg.num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
                
                // 检查地址是否在当前子段中
                if (phyaddr >= sub_seg_start && phyaddr < sub_seg_end) {
                    // 地址在占用段中
                    minimal_phymem_seg_t result;
                    result.base = phyaddr;
                    result.num_of_4kbpgs = sub_seg.num_of_4kbpgs;
                    result.type = OCCUPYIED;
                    result.remapped_count = sub_seg.remapped_count;
                    return result;
                } else if (phyaddr < sub_seg_start) {
                    // 地址在当前子段之前，检查是否在空隙中
                    if (mid == 0 || phyaddr >= seg.subtb[mid-1].base + seg.subtb[mid-1].num_of_4kbpgs * PAGE_SIZE_IN_LV[0]) {
                        // 地址在空隙中
                        phyaddr_t pre_seg_end = seg.subtb[mid-1].base + seg.subtb[mid-1].num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
                        minimal_phymem_seg_t result;
                        result.base = phyaddr;
                        result.num_of_4kbpgs = (phyaddr - pre_seg_end)>>12;
                        result.type = FREE;
                        result.remapped_count = 0;
                        return result;
                    }
                    right = mid - 1;
                } else {
                    // 地址在当前子段之后，检查是否在空隙中
                    if (mid == seg.num_of_subtb_entries - 1 || phyaddr < seg.subtb[mid+1].base) {
                        // 地址在空隙中
                        phyaddr_t end_addr = sub_seg.base + sub_seg.num_of_4kbpgs * PAGE_SIZE_IN_LV[0];
                        phyaddr_t next_seg_start = seg.subtb[mid+1].base;
                        minimal_phymem_seg_t result;
                        result.base = phyaddr;
                        result.num_of_4kbpgs = (next_seg_start - end_addr)>>12;
                        result.type = FREE;
                        result.remapped_count = 0;
                        return result;
                    }
                    left = mid + 1;
                }
            }
            
            // 如果没找到确切的子段，说明在某个空隙中
            minimal_phymem_seg_t result;
            result.base = phyaddr;
            result.num_of_4kbpgs = 1;
            result.type = FREE;
            result.remapped_count = 0;
            return result;
        }
    }
    
    // 如果地址不在任何可分配段中，则为RESERVED
    minimal_phymem_seg_t result;
    result.base = phyaddr;
    result.num_of_4kbpgs = 1;
    result.type = RESERVED;
    result.remapped_count = 0;
    return result;
}
