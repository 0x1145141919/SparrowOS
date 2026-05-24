#pragma once
#include  <stdint.h>
#include <abi/os_error_definitions.h>
#include <util/lock.h>
enum class page_state_t : uint8_t { 
    free = 0, 
    kernel_persisit = 1, 
    kernel_anonymous = 2, 
    user_file = 3, 
    user_anonymous = 4, 
    dma = 5,
    kernel_pinned = 10,
    reserved = 63
};

/**
 * @brief page struct
 *此数据结构必须进入mem_map才有语义，
 *透明大页支持：若某个页框的page.is_skipped为真则属于透明页，其实际语义被mem_map[ptr]所代表
 */
struct page{
    page_state_t state;
};
static_assert(sizeof(page)==1,"struct page size must be 1 bytes");
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
constexpr pgaccess KSPACE_RWX_ACCESS={
    .is_kernel=1,
    .is_writeable=1,
    .is_readable=1,
    .is_executable=1,
    .is_global=1,
    .cache_strategy=WB
};
constexpr pgaccess KSPACE_RW_UC_ACCESS={
    .is_kernel=1,
    .is_writeable=1,
    .is_readable=1,
    .is_executable=0,
    .is_global=1,
    .cache_strategy=UC
};

// 内核可读可执行（代码段）
constexpr pgaccess KSPACE_RX_ACCESS={
    .is_kernel=1,
    .is_writeable=0,
    .is_readable=1,
    .is_executable=1,
    .is_global=1,
    .cache_strategy=WB
};

// 内核只读（rodata）
constexpr pgaccess KSPACE_R_ACCESS={
    .is_kernel=1,
    .is_writeable=0,
    .is_readable=1,
    .is_executable=0,
    .is_global=1,
    .cache_strategy=WB
};

// 内核读写执行（身份映射），非全局 — boot 过渡期使用
constexpr pgaccess KSPACE_RWX_NG_ACCESS={
    .is_kernel=1,
    .is_writeable=1,
    .is_readable=1,
    .is_executable=1,
    .is_global=0,
    .cache_strategy=WB
};

// 内核读写写结合（帧缓冲）
constexpr pgaccess KSPACE_RW_WC_ACCESS={
    .is_kernel=1,
    .is_writeable=1,
    .is_readable=1,
    .is_executable=0,
    .is_global=1,
    .cache_strategy=WC
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
};
// v3: 砍掉 is_longtime, is_crucial_variable, vaddraquire, align_log2
// 仅保留 force_first_linekd_heap 和 is_when_realloc_force_new_addr
struct alloc_flags_t{
    uint8_t force_first_linekd_heap:1;    // 强制使用 first_linekd_heap
    uint8_t is_when_realloc_force_new_addr:1;// realloc 强制新分配
};
constexpr alloc_flags_t default_flags={
    .force_first_linekd_heap=false,
    .is_when_realloc_force_new_addr=false,
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
    uint64_t must_down_4gb:1;
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
    .must_down_4gb = 0,
    .align_log2 = 12
};
constexpr buddy_alloc_params BUDDY_ALLOC_DOWN_4GB{
    .numa = 0,
    .try_lock_always_try = 0,
    .must_down_4gb = 1,
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
struct vm_interval{
    uint64_t vpn;   // Virtual Page Number: (vaddr >> 12)，低 52bit 为页框号，高 12bit 可做 tag
    uint64_t ppn;   // Physical Page Number: (paddr >> 12)，低 52bit 为页框号，高 12bit 可做 tag
    uint64_t npages;// Number of 4KB pages in this interval
    pgaccess access;

    // 右对齐提取低 52bit 后左移还原为字节地址
    vaddr_t   vbase()    const { return static_cast<vaddr_t>((vpn & 0x000FFFFFFFFFFFFFULL) << 12); }
    phyaddr_t pbase()    const { return static_cast<phyaddr_t>((ppn & 0x000FFFFFFFFFFFFFULL) << 12); }
    uint64_t  byte_cnt() const { return npages << 12; }

    bool is_kernel_address() const {
        extern bool is_addr_kernel_address(void* addr);
        vaddr_t s = vbase();
        vaddr_t e = s + byte_cnt();
        return e > s &&
               is_addr_kernel_address(reinterpret_cast<void*>(s)) &&
               is_addr_kernel_address(reinterpret_cast<void*>(e - 1));
    }

    bool vaddr_belong(vaddr_t addr) const {
        vaddr_t s = vbase();
        return addr >= s && addr < s + byte_cnt();
    }

    bool paddr_belong(phyaddr_t addr) const {
        phyaddr_t s = pbase();
        return addr >= s && addr < s + byte_cnt();
    }

    /**
     * @brief 根据 vaddr()+byte_cnt() 虚拟区间与 paddr()+byte_cnt() 物理区间的同余等级，
     *        拆分为 seg_to_pages_info_pakage_t（含 1GB/2MB/4KB 分块），
     *        供 enable_VMentry/disable_VMentry 内部 TLB 自适应拆分使用。
     */
    seg_to_pages_info_pakage_t to_pages_info() const;
};
struct vm_interval_payload{
    vm_interval interval;
    uint64_t is_fixed_property:1;
};
int vm_interval_to_pages_info(seg_to_pages_info_pakage_t &result, VM_DESC vmentry);
int vm_interval_to_pages_info(seg_to_pages_info_pakage_t &result, vm_interval interval);
extern loaded_VM_interval* VM_intervals;
extern uint64_t VM_intervals_count;
extern phymem_segment *phymem_segments;
extern uint64_t phymem_segments_count; 