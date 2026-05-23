#pragma once
#include "util/Ktemplats.h"
#include "abi/boot.h"
#include <stdint.h>


struct mem_interval {
    uint64_t start;
    uint64_t size;
};
using phyaddr_t = uint64_t;
/**
 * @brief 基础内存分配器
 * 
 * 负责处理 UEFI 传递的内存描述符表，完成以下核心任务:
 * 1. 按物理地址排序所有内存段
 * 2. 检测并填充内存段之间的空隙
 * 3. 将 Loader 和 Boot Services 相关内存标记为可用 RAM
 */

class basic_allocator { 
private:
    static Ktemplats::list_doubly<EFI_MEMORY_DESCRIPTORX64>* memory_map;
    static phymem_segment*pure_mem_view;
    static uint64_t pure_view_entry_count;
    /**
     * @brief 按物理地址升序排序 (冒泡排序)
     */
    static void sort_by_physical_address();
    
    /**
     * @brief 检测并填充内存空洞
     * 
     * 在相邻内存段之间插入 EFI_RESERVED_MEMORY_TYPE 类型的描述符
     */
    static void fill_memory_holes();
    
    /**
     * @brief 回收 Loader 和 Boot Services 内存
     * 
     * 将以下类型转换为 freeSystemRam:
     * - EFI_LOADER_CODE
     * - EFI_LOADER_DATA
     * - EFI_BOOT_SERVICES_CODE
     * - EFI_BOOT_SERVICES_DATA
     * 
     * 并在转换后合并所有相邻的空闲内存段。
     */
    static void reclaim_loader_and_boot_services();
    
public:
    static uint64_t privious_alloc_end; 
    /**
     * @brief 初始化内存分配器
     * 
     * @param memory_map_ptr UEFI 提供的内存描述符表指针
     * @param entry_count 描述符条目数量
     * @return int 成功返回 0，失败返回负值错误码
     *         - 0: 成功
     *         - -1: 参数错误 (空指针或 entry_count=0)
     *         - -2: 内存分配失败
     */
    static int Init(EFI_MEMORY_DESCRIPTORX64* memory_map_ptr, uint16_t entry_count);
    static int pages_set(mem_interval interval, PHY_MEM_TYPE type);
    static phyaddr_t pages_alloc(uint64_t size,uint8_t align_log2=12);
    
    /**
     * @brief 获取纯净内存视图数组
     * 
     * 返回经过初始化处理后的内存描述符数组视图，包含所有可用 RAM 段。
     * 该视图已按物理地址排序，填充了内存空洞，并回收了 Loader/Boot Services 内存。
     * 
     * @param entry_count_ptr 输出参数，返回视图条目数量
     * @return phymem_segment* 纯净内存视图数组指针，失败返回 nullptr
     */
    static  void print_now_segs();
    static phymem_segment* get_pure_memory_view(uint64_t* entry_count_ptr);
};
