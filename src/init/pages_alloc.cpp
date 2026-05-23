#include "init/pages_alloc.h"
#include "util/OS_utils.h"
#include "init/init_heap_v3.h"
#include "util/kout.h"
// 静态成员定义
Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>* basic_allocator::memory_map = nullptr;
phymem_segment* basic_allocator::pure_mem_view = nullptr;
uint64_t basic_allocator::pure_view_entry_count = 0;
uint64_t basic_allocator::privious_alloc_end = 0; 
/**
 * @brief 按物理地址升序排序 (冒泡排序)
 * 
 * 使用迭代器遍历双链表，通过交换元素值实现原地排序
 * 时间复杂度 O(n²)，适合 EFI 内存表 (通常 < 1000 项)
 */
void basic_allocator::sort_by_physical_address()
{
    if (!memory_map || memory_map->size() < 2) return;
    
    bool swapped;
    do {
        swapped = false;
        
        auto it = memory_map->begin();
        auto next_it = it;
        ++next_it;
        
        for (size_t i = 0; i < memory_map->size() - 1; ++i) {
            EFI_MEMORY_DESCRIPTORX64& desc_a = *it;
            EFI_MEMORY_DESCRIPTORX64& desc_b = *next_it;
            
            if (desc_a.PhysicalStart > desc_b.PhysicalStart) {
                EFI_MEMORY_DESCRIPTORX64 temp = desc_a;
                desc_a = desc_b;
                desc_b = temp;
                swapped = true;
            }
            ++it;
            ++next_it;
        }
    } while (swapped);
}

/**
 * @brief 检测并填充内存空洞
 * 
 * 遍历排序后的内存描述符表，检测相邻段之间的空隙，
 * 并插入 EFI_RESERVED_MEMORY_TYPE 类型的描述符进行填充。
 */
void basic_allocator::fill_memory_holes()
{
    if (!memory_map || memory_map->size() < 2) return;
    
    size_t count = memory_map->size();
    for (size_t i = 0; i < count - 1; ++i) {
        auto it = memory_map->begin();
        for (size_t j = 0; j < i; ++j) ++it;
        
        auto next_it = it;
        ++next_it;
        
        EFI_MEMORY_DESCRIPTORX64& curr = *it;
        EFI_MEMORY_DESCRIPTORX64& next_desc = *next_it;
        
        uint64_t current_end = curr.PhysicalStart + (curr.NumberOfPages * 0x1000);
        uint64_t next_start = next_desc.PhysicalStart;
        
        if (current_end < next_start) {
            uint64_t hole_pages = (next_start - current_end) / 0x1000;
            
            EFI_MEMORY_DESCRIPTORX64 hole_desc;
            ksetmem_8(&hole_desc, 0, sizeof(EFI_MEMORY_DESCRIPTORX64));
            hole_desc.Type = PHY_MEM_TYPE::OS_MEMSEG_HOLE;
            hole_desc.PhysicalStart = current_end;
            hole_desc.NumberOfPages = hole_pages;
            hole_desc.Attribute = 0;
            
            // 手动实现插入逻辑：创建新链表并复制所有元素
            Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>* new_list = 
                new Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>();
            
            size_t idx = 0;
            for (auto elem_it = memory_map->begin(); elem_it != memory_map->end(); ++elem_it) {
                if (idx == i + 1) {
                    new_list->push_back(hole_desc);
                }
                new_list->push_back(*elem_it);
                ++idx;
            }
            
            delete memory_map;
            memory_map = new_list;
            ++count;
        }
    }
}

/**
 * @brief 回收 Loader 和 Boot Services 内存
 * 
 * 遍历所有内存描述符，将 Loader 和 Boot Services 相关的
 * 代码段和数据段标记为 freeSystemRam，使其可被内核使用。
 * 回收完成后会合并相邻的空闲段以减少碎片。
 */
void basic_allocator::reclaim_loader_and_boot_services()
{
    if (!memory_map) return;
    
    // 第一阶段：标记所有可回收的段为 freeSystemRam
    for (auto it = memory_map->begin(); it != memory_map->end(); ++it) {
        EFI_MEMORY_DESCRIPTORX64& desc = *it;
        switch (desc.Type) {
            case EFI_LOADER_CODE:
            case EFI_LOADER_DATA:
            case EFI_BOOT_SERVICES_CODE:
            case EFI_BOOT_SERVICES_DATA:
            case EFI_RUNTIME_SERVICES_CODE:
            case EFI_RUNTIME_SERVICES_DATA:
                desc.Type = freeSystemRam;
                break;
            default:
                break;
        }
    }
    
    // 第二阶段：合并相邻的空闲段
    if (memory_map->size() < 2) return;
    
    Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>* new_list = 
        new Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>();
    
    auto it = memory_map->begin();
    EFI_MEMORY_DESCRIPTORX64 current_desc = *it;
    ++it;
    
    while (it != memory_map->end()) {
        EFI_MEMORY_DESCRIPTORX64& next_desc = *it;
        
        // 检查当前段和下一段是否都是空闲段且物理地址连续
        if (current_desc.Type == freeSystemRam && 
            next_desc.Type == freeSystemRam) {
            
            uint64_t current_end = current_desc.PhysicalStart + 
                                   (current_desc.NumberOfPages * 0x1000);
            
            // 如果物理地址连续，合并两段
            if (current_end == next_desc.PhysicalStart) {
                current_desc.NumberOfPages += next_desc.NumberOfPages;
                // 继承属性（理论上应该相同）
                current_desc.Attribute |= next_desc.Attribute;
            } else {
                // 不连续，将当前段加入新链表
                new_list->push_back(current_desc);
                current_desc = next_desc;
            }
        } else {
            // 类型不同或不都是空闲段，将当前段加入新链表
            new_list->push_back(current_desc);
            current_desc = next_desc;
        }
        ++it;
    }
    
    // 添加最后一个段
    new_list->push_back(current_desc);
    
    // 替换原链表
    delete memory_map;
    memory_map = new_list;
}

/**
 * @brief 设置指定内存区域的类型
 * 
 * 遍历旧链表，根据指定的内存区间构建新链表。
 * 将区间内的所有内存段设置为指定类型，支持跨越多个连续内存段的场景。
 * 
 * 算法思路:
 * 1. 遍历旧链表的每个描述符
 * 2. 计算描述符与目标区间的交集
 * 3. 将描述符分裂为：[保留部分] + [设置类型部分] + [保留部分]
 * 4. 依次添加到新链表中
 * 
 * 处理场景:
 * 1. 完全匹配：整个描述符都在区间内，直接修改类型
 * 2. 部分覆盖前部：分裂为 [新类型] + [后部原类型]
 * 3. 部分覆盖后部：分裂为 [前部原类型] + [新类型]
 * 4. 覆盖中间：分裂为 [前部原类型] + [新类型] + [后部原类型]
 * 5. 跨越多个段：依次处理每个受影响的段，统一设置为指定类型
 * 
 * 假设条件:
 * - 链表按物理地址单调递增排序
 * - 相邻描述符之间无交集
 * 
 * @param interval 内存区间 (起始地址和大小)
 * @param type 要设置的内存类型
 * @return int 成功返回 0，失败返回负值错误码
 *         - 0: 成功
 *         - -1: 参数错误或未找到匹配的内存区域
 */
int basic_allocator::pages_set(mem_interval interval, PHY_MEM_TYPE type)
{
    if (!memory_map || interval.size == 0) {
        return -1;
    }
    
    const uint64_t start = interval.start;
    const uint64_t end = start + interval.size;
    
    // 验证 4KB 对齐
    if (start % 0x1000 != 0 || interval.size % 0x1000 != 0) {
        return -1;
    }
    
    // 创建新链表用于存储结果
    Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>* new_list = 
        new Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>();
    
    bool has_match = false;
    
    // 遍历旧链表的每个描述符
    for (auto it = memory_map->begin(); it != memory_map->end(); ++it) {
        EFI_MEMORY_DESCRIPTORX64& desc = *it;
        
        const uint64_t desc_start = desc.PhysicalStart;
        const uint64_t desc_end = desc_start + (desc.NumberOfPages * 0x1000);
        
        // 保存原始属性用于新建段
        const uint64_t orig_attr = desc.Attribute;
        const PHY_MEM_TYPE orig_type = static_cast<PHY_MEM_TYPE>(desc.Type);
        
        // 检查是否与目标区间有交集
        if (start < desc_end && end > desc_start) {
            has_match = true;
            
            // 计算交集范围
            const uint64_t intersect_start = (start > desc_start) ? start : desc_start;
            const uint64_t intersect_end = (end < desc_end) ? end : desc_end;
            
            // 计算三个部分的边界
            const uint64_t front_start = desc_start;
            const uint64_t front_end = intersect_start;
            
            const uint64_t mid_start = intersect_start;
            const uint64_t mid_end = intersect_end;
            
            const uint64_t back_start = intersect_end;
            const uint64_t back_end = desc_end;
            
            // 添加前部保留段（如果有）
            if (front_start < front_end) {
                EFI_MEMORY_DESCRIPTORX64 front_desc;
                ksetmem_8(&front_desc, 0, sizeof(EFI_MEMORY_DESCRIPTORX64));
                front_desc.Type = orig_type;
                front_desc.PhysicalStart = front_start;
                front_desc.NumberOfPages = (front_end - front_start) / 0x1000;
                front_desc.Attribute = orig_attr;
                new_list->push_back(front_desc);
            }
            
            // 添加中间设置类型段（如果有）
            if (mid_start < mid_end) {
                EFI_MEMORY_DESCRIPTORX64 mid_desc;
                ksetmem_8(&mid_desc, 0, sizeof(EFI_MEMORY_DESCRIPTORX64));
                mid_desc.Type = type;
                mid_desc.PhysicalStart = mid_start;
                mid_desc.NumberOfPages = (mid_end - mid_start) / 0x1000;
                mid_desc.Attribute = orig_attr;
                new_list->push_back(mid_desc);
            }
            
            // 添加后部保留段（如果有）
            if (back_start < back_end) {
                EFI_MEMORY_DESCRIPTORX64 back_desc;
                ksetmem_8(&back_desc, 0, sizeof(EFI_MEMORY_DESCRIPTORX64));
                back_desc.Type = orig_type;
                back_desc.PhysicalStart = back_start;
                back_desc.NumberOfPages = (back_end - back_start) / 0x1000;
                back_desc.Attribute = orig_attr;
                new_list->push_back(back_desc);
            }
        } else {
            // 没有交集，直接复制原描述符
            new_list->push_back(desc);
        }
    }
    
    if (!has_match) {
        delete new_list;
        return -1;
    }
    delete memory_map;
    memory_map = new_list;
    return 0;
}

/**
 * @brief 分配指定大小的物理内存页
 * 
 * 使用线性扫描法从privious_alloc_end 开始扫描空闲内存段，
 * 找到第一个满足大小要求的空闲段并返回起始地址。
 * 
 * 注意：本函数只扫描不修改链表，调用者拿到地址后需要自行调用
 * pages_set 来标记已分配的内存区域。
 * 
 * 对齐要求:
 * - align_log2 < 12 时自动提升到 12 (4KB 对齐)
 * - align_log2 >= 12 时使用指定对齐
 * 
 * @param size 需要分配的字节数 (必须是 4KB 的倍数)
 * @param align_log2 对齐指数的对数 (例如 12 表示 4KB 对齐，16 表示 64KB 对齐)
 * @return phyaddr_t 成功返回可用的物理地址，失败返回 ~0ull
 */
phyaddr_t basic_allocator::pages_alloc(uint64_t size, uint8_t align_log2)
{
    if (!memory_map || size == 0) {
        return ~0ull;
    }
    
    // 确保 4KB 对齐 (最小页面大小)
    if (align_log2 < 12) {
        align_log2 = 12;
    }
    
    const uint64_t alignment = 1ull << align_log2;
    const uint64_t required_pages = (size + 0xFFF) / 0x1000; // 向上取整到 4KB
    
    // 从 privious_alloc_end 开始扫描
    uint64_t search_start = privious_alloc_end;
    
    // 如果从未分配过，从第一个空闲段开始
    if (search_start == 0) {
        for (auto it = memory_map->begin(); it != memory_map->end(); ++it) {
            EFI_MEMORY_DESCRIPTORX64& desc = *it;
            
            if (desc.Type != freeSystemRam) {
                continue;
            }
            
            const uint64_t seg_start = desc.PhysicalStart;
            const uint64_t seg_end = seg_start + (desc.NumberOfPages * 0x1000);
            const uint64_t seg_size = seg_end - seg_start;
            
            if (seg_size < size) {
                continue;
            }
            
            // 计算对齐后的起始地址
            uint64_t aligned_start = seg_start;
            if (alignment > 1) {
                aligned_start = (seg_start + alignment - 1) & ~(alignment - 1);
            }
            
            // 检查对齐后是否有足够的空间
            if (aligned_start >= seg_end || (seg_end - aligned_start) < size) {
                continue;
            }
            
            // 更新搜索起点
            privious_alloc_end = aligned_start + size;
            
            return aligned_start;
        }
    } else {
        // 从上次分配的位置继续扫描
        for (auto it = memory_map->begin(); it != memory_map->end(); ++it) {
            EFI_MEMORY_DESCRIPTORX64& desc = *it;
            
            if (desc.Type != freeSystemRam) {
                continue;
            }
            
            const uint64_t seg_start = desc.PhysicalStart;
            const uint64_t seg_end = seg_start + (desc.NumberOfPages * 0x1000);
            
            // 检查段是否与搜索范围有交集
            if (seg_end <= search_start) {
                continue;
            }
            
            // 计算实际可用的起始位置
            uint64_t actual_start = (seg_start > search_start) ? seg_start : search_start;
            
            // 计算对齐后的起始地址
            uint64_t aligned_start = actual_start;
            if (alignment > 1) {
                aligned_start = (actual_start + alignment - 1) & ~(alignment - 1);
            }
            
            // 检查是否在段范围内
            if (aligned_start >= seg_end) {
                continue;
            }
            
            // 检查是否有足够的空间
            uint64_t available_size = seg_end - aligned_start;
            if (available_size < size) {
                continue;
            }
            
            // 更新搜索起点
            privious_alloc_end = aligned_start + size;
            
            return aligned_start;
        }
    }
    
    // 未找到合适的空闲内存
    return ~0ull;
}

/**
 * @brief 初始化内存分配器
 * 
 * 处理流程:
 * 1. 复制 UEFI 内存描述符到双链表
 * 2. 按物理地址排序
 * 3. 填充内存空洞
 * 4. 回收 Loader/Boot Services 内存
 * 5. 从处理后的 memory_map 创建纯净内存视图
 * 
 * @param memory_map_ptr UEFI 提供的内存描述符表指针
 * @param entry_count 描述符条目数量
 * @return int 成功返回 0，失败返回负值错误码
 */
int basic_allocator::Init(EFI_MEMORY_DESCRIPTORX64* memory_map_ptr, uint16_t entry_count)
{
    if (!memory_map_ptr || entry_count == 0) {
        return -1;
    }
    
    memory_map = new Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>();
    if (!memory_map) {
        return -2;
    }
    
    // 复制所有 EFI 内存描述符到双链表
    for (uint16_t i = 0; i < entry_count; ++i) {
        memory_map->push_back(memory_map_ptr[i]);
    }
    
    // 1. 按物理地址排序
    sort_by_physical_address();
    
    // 2. 填充内存空洞
    fill_memory_holes();
    
    // 3. 回收 Loader 和 Boot Services 内存为可用 RAM
    reclaim_loader_and_boot_services();
    
    // 4. 创建纯净内存视图（在堆上）
    pure_view_entry_count = memory_map->size();
    pure_mem_view = new phymem_segment[pure_view_entry_count];
    if (!pure_mem_view) {
        delete memory_map;
        return -3;
    }
    
    // 5. 从 memory_map 复制到纯净视图
    uint64_t idx = 0;
    for (auto it = memory_map->begin(); it != memory_map->end(); ++it) {
        EFI_MEMORY_DESCRIPTORX64& desc = *it;
        pure_mem_view[idx].start = desc.PhysicalStart;
        pure_mem_view[idx].size = desc.NumberOfPages * 0x1000;
        pure_mem_view[idx].type = static_cast<PHY_MEM_TYPE>(desc.Type);
        ++idx;
    }
    for(int64_t trim = (int64_t)pure_view_entry_count - 1; trim >= 0; trim--) {
        if(pure_mem_view[trim].start == 0x100000000ULL &&
           pure_mem_view[trim].type == freeSystemRam) {
            pure_view_entry_count = (uint64_t)(trim + 1);
            break;
        }
    }
    privious_alloc_end=0x100000;
    return 0;
}

/**
 * @brief 获取纯净内存视图数组
 * 
 * 返回经过初始化处理后的内存描述符数组视图，包含所有可用 RAM 段。
 * 该视图已按物理地址排序，填充了内存空洞，并回收了 Loader/Boot Services 内存。
 * 
 * @param entry_count_ptr 输出参数，返回视图条目数量
 * @return phymem_segment* 纯净内存视图数组指针，失败返回 nullptr
 */
phymem_segment *basic_allocator::get_pure_memory_view(uint64_t *entry_count_ptr) 
{
    if (!pure_mem_view || !entry_count_ptr) {
        return nullptr;
    }
    
    *entry_count_ptr = pure_view_entry_count;
    return pure_mem_view;
}
void basic_allocator::print_now_segs()
{
   if (!memory_map) {
       bsp_kout<< "[WARN] memory_map is nullptr, nothing to print" << kendl;
        return;
    }
    
   bsp_kout<< kendl;
   bsp_kout<< "========================================" << kendl;
   bsp_kout<< "[INFO] Current Memory Map Segments (" << memory_map->size() << " entries)" << kendl;
   bsp_kout<< "========================================" << kendl;
    
   uint64_t index = 0;
    for (auto it = memory_map->begin(); it != memory_map->end(); ++it) {
       EFI_MEMORY_DESCRIPTORX64& desc = *it;
        
        const char* type_str = "UNKNOWN";
        switch (desc.Type) {
            case EFI_RESERVED_MEMORY_TYPE:
                type_str = "EFI_RESERVED";
                break;
            case EFI_LOADER_CODE:
                type_str = "LOADER_CODE";
                break;
            case EFI_LOADER_DATA:
                type_str = "LOADER_DATA";
                break;
            case EFI_BOOT_SERVICES_CODE:
                type_str = "BOOT_SERVICES_CODE";
                break;
            case EFI_BOOT_SERVICES_DATA:
                type_str = "BOOT_SERVICES_DATA";
                break;
            case EFI_RUNTIME_SERVICES_CODE:
                type_str = "RUNTIME_SERVICES_CODE";
                break;
            case EFI_RUNTIME_SERVICES_DATA:
                type_str = "RUNTIME_SERVICES_DATA";
                break;
            case freeSystemRam:
                type_str = "FREE_SYSTEM_RAM";
                break;
            case EFI_UNUSABLE_MEMORY:
                type_str = "UNUSABLE_MEMORY";
                break;
            case EFI_ACPI_RECLAIM_MEMORY:
                type_str = "ACPI_RECLAIM_MEMORY";
                break;
            case EFI_ACPI_MEMORY_NVS:
                type_str = "ACPI_MEMORY_NVS";
                break;
            case EFI_MEMORY_MAPPED_IO:
                type_str = "MEMORY_MAPPED_IO";
                break;
            case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
                type_str = "MMIO_PORT_SPACE";
                break;
            case EFI_PAL_CODE:
                type_str = "PAL_CODE";
                break;
            case EFI_PERSISTENT_MEMORY:
                type_str = "PERSISTENT_MEMORY";
                break;
            case EFI_UNACCEPTED_MEMORY_TYPE:
                type_str = "UNACCEPTED_MEMORY";
                break;
            case OS_KERNEL_DATA:
                type_str = "OS_KERNEL_DATA";
                break;
            case OS_KERNEL_CODE:
                type_str = "OS_KERNEL_CODE";
                break;
            case OS_KERNEL_STACK:
                type_str = "OS_KERNEL_STACK";
                break;
            case OS_HARDWARE_GRAPHIC_BUFFER:
                type_str = "HARDWARE_GRAPHIC_BUFFER";
                break;
            case ERROR_FAIL_TO_FIND:
                type_str = "ERROR_FAIL_TO_FIND";
                break;
            case OS_ALLOCATABLE_MEMORY:
                type_str = "OS_ALLOCATABLE_MEMORY";
                break;
            case OS_RESERVED_MEMORY:
                type_str = "OS_RESERVED_MEMORY";
                break;
            case OS_PGTB_SEGS:
                type_str = "OS_PGTB_SEGS";
                break;
            case OS_MEMSEG_HOLE:
                type_str = "OS_MEMSEG_HOLE";
                break;
            default:
                type_str = "UNKNOWN";
                break;
        }
        
       uint64_t size_bytes = desc.NumberOfPages * 0x1000;
       uint64_t physical_start = desc.PhysicalStart;
       uint64_t physical_end = physical_start + size_bytes;
       uint64_t attribute = desc.Attribute;
        
       bsp_kout.shift_hex();  // 切换到十六进制
       bsp_kout<< "  [" << index << "] " 
                     << type_str 
                     << " | PA: 0x" << physical_start 
                     << " - 0x" << (physical_end > 0 ? physical_end - 1 : 0)
                     << " | Size: 0x" << size_bytes 
                     << " (" << (size_bytes / 1024) << " KB)"
                     << " | Pages: " << desc.NumberOfPages
                     << " | Attr: 0x" << attribute
                     << kendl;
     bsp_kout.shift_dec();  // 恢复十进制
        
        ++index;
    }
    
   bsp_kout<< "========================================" << kendl;
   bsp_kout<< kendl;
}