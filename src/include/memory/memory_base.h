#pragma once
#include  <stdint.h>
#include <abi/os_error_definitions.h>
#include <util/lock.h>
typedef enum :uint32_t{
    EFI_RESERVED_MEMORY_TYPE,
    EFI_LOADER_CODE,
    EFI_LOADER_DATA,
    EFI_BOOT_SERVICES_CODE,
    EFI_BOOT_SERVICES_DATA,
    EFI_RUNTIME_SERVICES_CODE,
    EFI_RUNTIME_SERVICES_DATA,
    freeSystemRam,
    EFI_UNUSABLE_MEMORY,
    EFI_ACPI_RECLAIM_MEMORY,
    EFI_ACPI_MEMORY_NVS,
    EFI_MEMORY_MAPPED_IO,
    EFI_MEMORY_MAPPED_IO_PORT_SPACE,
    EFI_PAL_CODE,
    EFI_PERSISTENT_MEMORY,
    EFI_UNACCEPTED_MEMORY_TYPE,
    EFI_MAX_MEMORY_TYPE,
    OS_KERNEL_DATA,
    OS_KERNEL_CODE,
    OS_KERNEL_STACK,
    OS_HARDWARE_GRAPHIC_BUFFER,
    ERROR_FAIL_TO_FIND,
    OS_ALLOCATABLE_MEMORY,
    OS_RESERVED_MEMORY,
    OS_PGTB_SEGS,
    OS_MEMSEG_HOLE,
    MEMORY_TYPE_OEM_RESERVED_MIN = 0x70000000,
  MEMORY_TYPE_OEM_RESERVED_MAX = 0x7FFFFFFF,
  MEMORY_TYPE_OS_RESERVED_MIN  = 0x80000000,
  MEMORY_TYPE_OS_RESERVED_MAX  = 0xFFFFFFFF
} PHY_MEM_TYPE;
constexpr uint64_t MAX_PHYADDR_1GB_PGS_COUNT=1ull<<21;
typedef uint64_t phyaddr_t;
typedef uint64_t vaddr_t;
enum cache_strategy_t:uint8_t
{
    UC=0,
    WC=1,
    WT=4,
    WP=5,
    WB=6,
    UC_minus=7
};
struct cache_table_idx_struct_t
{
    uint8_t PWT:1;
    uint8_t PCD:1;
    uint8_t PAT:1;
};
enum vinterval_base_same_congruence_level{
    congruence_level_4kb,
    congruence_level_2mb,
    congruence_level_1gb
};
struct seg_to_pages_info_pakage_t{
   vinterval_base_same_congruence_level congruence_level;
        struct pages_info_t{
           vaddr_t vbase;
            phyaddr_t phybase;
           uint64_t page_size_in_byte;
           uint64_t num_of_pages;
    };
    pages_info_t entryies[5];//里面的地址顺序是无序的
    
    /**
     * 清空所有条目数据
     */
    void clear() {
        congruence_level = congruence_level_4kb;
        for (uint32_t i = 0; i < 5; i++) {
            entryies[i].vbase = 0;
            entryies[i].phybase = 0;
            entryies[i].page_size_in_byte = 0;
            entryies[i].num_of_pages = 0;
        }
    }
};
struct shared_inval_VMentry_info_t{
    seg_to_pages_info_pakage_t info_package;
    bool is_package_valid;
    u32ka completed_processors_count;
};
union ia32_pat_t
{
   uint64_t value;
   cache_strategy_t  mapped_entry[8];
};
constexpr ia32_pat_t DEFAULT_PAT_CONFIG={
    .value=0x0407050600070106
};
struct pgaccess
{
    uint8_t is_kernel:1;
    uint8_t is_writeable:1;
    uint8_t is_readable:1;
    uint8_t is_executable:1;
    uint8_t is_global:1;
    cache_strategy_t cache_strategy;
};
constexpr pgaccess KSPACE_RW_ACCESS={
    .is_kernel=1,
    .is_writeable=1,
    .is_readable=1,
    .is_executable=0,
    .is_global=1,
    .cache_strategy=WB
};
struct vphypair_t
{//三个参数至少4k对齐
    vaddr_t vaddr;
    phyaddr_t paddr;
    uint32_t size;
};

struct VM_DESC
{
    vaddr_t start;    // inclusive
    vaddr_t end;      // exclusive
                      // 区间长度 = end - start
    enum map_type_t : uint8_t {
        MAP_NONE = 0,     // 未分配物理页（仅占位）
        MAP_PHYSICAL,     // 连续物理页,只有内核因为立即要求而使用，用户空间不能用
        MAP_FILE,         // 文件映射
        MAP_ANON,          // 匿名映射（默认用户空间）
    } map_type;
    phyaddr_t phys_start;  // 当 map_type=MAP_PHYSICAL 时有效
                           // MAP_NONE 没有意义
    pgaccess access;       // 页权限/缓存策略
    uint8_t committed_full:1;   // 物理页是否完全已经分配（lazy allocation 用）
    uint8_t is_vaddr_alloced:1;    // 虚拟地址是否由地址空间管理器分配（否则为固定映射）
    uint8_t is_out_bound_protective:1; // 是否有越界保护区,只有is_vaddr_alloced为1的bit此位才有意义，
    uint64_t SEG_SIZE_ONLY_UES_IN_BASIC_SEG;
};
struct alloc_flags_t{
    uint8_t is_longtime:1;
    uint8_t is_crucial_variable:1;
    uint8_t vaddraquire:1;
    uint8_t force_first_linekd_heap:1;
    uint8_t is_when_realloc_force_new_addr:1;//在realloc中强制重新分配内存，非realloc接口忽视此位但是会忠实记录进入metadata,realloc中此位不设置会优先原地调整，原地调整解决则不会修改源地址和元数据flags
    uint8_t align_log2;
};
constexpr alloc_flags_t default_flags={
    .is_longtime=false,
    .is_crucial_variable=false,
    .vaddraquire=true,
    .force_first_linekd_heap=false,
    .is_when_realloc_force_new_addr=false,
    .align_log2=4
};

/**
 * @brief 
 * 对于分配参数的设计优先看位域控制，遵循以下的依赖
 * 1.force_first_bcb为1时强制只使用first_BCB，其它参数统统无效
 * 2.当force_first_bcb为0时，no_up_limit_bit，try_lock_always_try，no_addr_constrain_bit
 * 这三个位是三个独立的位
 * 2.1try_lock_always_try:为1只有确定所有所有BCB都不满足分配条件时才失败，在到达这个之前无限重试,为0时重试次数有上限
 * 2.2no_up_limit_bit:为0时等价于[0,up_phyaddr_limit)的地址限制
 * 2.3no_addr_constrain_bit:为0时等价于[constrain_base,constrain_base+constrain_interval_size)的地址限制]
 * 2.2和2.3若同时为0则区间取交集
 */
struct buddy_alloc_params{
    uint64_t numa;//不支持，暂时
    uint64_t try_lock_always_try:1;//多BCB的架构下，会尝试多次获取锁，失败次数过高会失败返回繁忙重试，这个标志位为1则永远尝试直到成功获取锁
    uint8_t align_log2;
};
constexpr buddy_alloc_params BUDDY_ALLOC_DEFAULT_FLAG{
    .numa = 0,
    .try_lock_always_try = 0,
    .align_log2 = 12
};
constexpr buddy_alloc_params BUDDY_ALLOC_ALWAYS_TRY{
    .numa = 0,
    .try_lock_always_try = 1,
    .align_log2 = 12
};
struct phymem_segment {
    phyaddr_t start;
    uint64_t size;
    PHY_MEM_TYPE type;
};
struct loaded_VM_interval {

    phyaddr_t pbase;
    vaddr_t vbase;
    uint64_t size;
    uint32_t VM_interval_specifyid;
    pgaccess access;
};
int vm_interval_to_pages_info(seg_to_pages_info_pakage_t &result, VM_DESC vmentry);
extern loaded_VM_interval* VM_intervals;
extern uint64_t VM_intervals_count;
extern phymem_segment *phymem_segments;
extern uint64_t phymem_segments_count; 