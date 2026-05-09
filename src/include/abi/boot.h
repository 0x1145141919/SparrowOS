#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#define INIT_INCLUDE_PATH "../init/include/"
#define LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD 0x01
#define LOADED_FILE_ENTRY_TYPE_BINARY 0x02
#define LOADED_FILE_ENTRY_TYPE_ELF_NO_LOAD 0x03//不加载的elf文件，内核主体
constexpr uint16_t PASS_THROUGH_DEVICE_GRAPHICS_INFO =0x0001;
struct pass_through_device_info {
    uint16_t device_info;
    void* specify_data;
};
typedef struct {
    PHY_MEM_TYPE     Type;          // 4字节
    uint32_t ReservedA;
    
    uint64_t  PhysicalStart;  // 8字节
    uint64_t   VirtualStart;   // 8字节
    uint64_t                NumberOfPages;  // 8字节
    uint64_t                Attribute;      // 8字节
    uint64_t                ReservedB;      // 8字节
} EFI_MEMORY_DESCRIPTORX64;
struct loaded_file_entry {
    char file_name[256];//文件系统路径
    uint64_t file_size; //文件大小
    uint16_t file_type;//file_type文件类型，是elf可执行文件还是裸二进制普通文件
    void* raw_data; //文件内容的原始指针，loader负责加载到内存中，内核负责解析,LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD这里是nullptr(是按图索骥不是连续地址)
};

typedef struct {
    uint64_t signature;          // 标识符，例如 'BOOTINFO'
    uint32_t total_size;         // 整个数据结构的总大小
    uint32_t total_pages_count; // 包含内存映射和加载文件等信息所占用的总页数
    uint16_t version;            // 结构版本

    uint64_t loaded_file_count;     // 加载的文件数量
    struct loaded_file_entry* loaded_files; // 加载的文件列表指针
    // 各字段的大小
    uint16_t memory_map_entry_count;
    uint16_t memory_map_entry_size;
    uint32_t memory_map_size;
    uint64_t memory_map_version;
    EFI_MEMORY_DESCRIPTORX64* memory_map_ptr;

    uint64_t pass_through_device_info_count;
    struct pass_through_device_info* pass_through_devices; // 传递到内核的设备的数组
    
    void* gST_ptr;
    char parameter_area[512]; // 预留的参数区域，可以根据需要调整大小
    uint64_t flags;
    uint32_t logical_processor_count;
    uint64_t checksum;           // 可选的数据校验
} BootInfoHeader;
// ============================================
// 内存类型枚举与常量定义 (与 kernel.elf 保持一致)
// ============================================
constexpr uint32_t VM_ID_architecture_agnostic_base = 0x1000;
// BSP 初始栈：32KB, 对齐 4KB(2^12)
constexpr uint32_t VM_ID_BSP_INIT_STACK = 0x1001;
constexpr uint64_t BSP_INIT_STACK_SIZE = 32 * 1024;           // 32KB
constexpr uint8_t BSP_INIT_STACK_ALIGN_LOG2 = 12;             // 4KB 对齐

// 第一堆：4MB, 对齐 2MB(2^21)
constexpr uint32_t VM_ID_FIRST_HEAP = 0x1003;
constexpr uint64_t FIRST_HEAP_SIZE_CONST = 4 * 1024 * 1024;   // 4MB
constexpr uint8_t FIRST_HEAP_ALIGN_LOG2 = 21;     
// 第一堆位图：FIRST_HEAP_SIZE/128, 对齐 4KB(2^12)
// FIRST_HEAP_SIZE 在 init_linker_symbols.h 中定义为 2MB，所以位图大小 = 2MB/128 = 16KB
constexpr uint32_t VM_ID_FIRST_HEAP_BITMAP = 0x1002;
constexpr uint64_t FIRST_HEAP_BITMAP_SIZE = FIRST_HEAP_SIZE_CONST/128;        // 16KB
constexpr uint8_t FIRST_HEAP_BITMAP_ALIGN_LOG2 = 12;          // 4KB 对齐

            // 2MB 对齐

// 日志缓冲区：2MB, 对齐 2MB(2^21)
constexpr uint32_t VM_ID_LOGBUFFER = 0x1004;
constexpr uint64_t LOGBUFFER_SIZE = 2 * 1024 * 1024;          // 4MB
constexpr uint8_t LOGBUFFER_ALIGN_LOG2 = 21;                  // 2MB 对齐

constexpr uint32_t VM_ID_KSYMBOLS = 0x1006;
constexpr uint32_t VM_ID_MEM_MAP = 0x1007;
constexpr uint32_t VM_ID_BCBS_BITMAPS = 0x1008;
constexpr uint32_t MEM_MAP_ALIGN_LOG2 = 21;
// 上层内核空间页目录指针表：1MB, 对齐 4KB(2^12)
//0x2000~0x2fff是x86_64_PGLV4的架构锁死的传递的VM
constexpr uint32_t VM_ID_UP_KSPACE_PDPT = 0x2001;
constexpr uint32_t VM_ID_GRAPHIC_BUFFER = 0x2002;
constexpr uint64_t UP_KSPACE_PDPT_SIZE = 1 * 1024 * 1024;     // 1MB
constexpr uint8_t UP_KSPACE_PDPT_ALIGN_LOG2 = 12;             // 4KB 对齐
constexpr uint32_t VM_ID_HPET_MMIO = 0x2003;


struct init_to_kernel_header {//这个信息包的头也应该是使用available_meminterval_probe分配的
    uint64_t magic;
    uint64_t self_pages_count;
    phymem_segment kmmu_interval;
    uint64_t phymem_segment_count;
    uint64_t memory_map_offset;//相较于头的偏移量
    uint64_t loaded_VM_interval_count;
    uint64_t loaded_VM_intervals_offset;//相较于头的偏移量
    uint64_t pass_through_device_info_count;
    uint64_t pass_through_devices_offset;//相较于头的偏移量
    uint32_t logical_processor_count;
    vm_interval kIMG_self_window;//内核自身连续物理地址需要被映射，使用available_meminterval_probe_keep
    vm_interval kBSS_interval;//内核唯一的bss区连续，使用available_meminterval_probe_keep
    vm_interval pages_arr;//直接用pages_allocator里面的页框数组但是清0,复用不转生
    vm_interval FPA_bitmaps;//从phymem_segment* memory_map;解析可分配内存数目上限，一个页框2bit数据，使用available_meminterval_probe_keep
    vm_interval log_buffer;//日志缓冲区，使用available_meminterval_probe
    vm_interval kernel_entry_stack;//BSP初始化栈，使用available_meminterval_probe
    vm_interval symtable_file;//符号表文件，使用available_meminterval_probe
    vm_interval initramfs_file;//initramfs文件，使用available_meminterval_probe
    vm_interval identity_map_window;//不但要[0,dram_top)进行va_alloc进行映射，而且要对于[16k,dram_top)进行WB+RWX的恒等映射
    uint64_t arch_specify_offset;//相较于头的偏移量
};
/**
 *  init_to_kernel_info信息包规范：init.elf传递给kernel.elf的信息包唯一一个物理地址
 * 其指向的是一个连续的物理地址区间，规定这个物理地址区间的头部必然是init_to_kernel_header结构体。
 * 设物理基址为p,显然（init_to_kernel_header*）head=p；获得信息包基址。
 * 其中的_offset变量是相对于p的偏移量，比如loaded_VM_interval*physegs_arr=(loaded_VM_interval*)(p+head->loaded_VM_intervals_offset)
 * 由是，若将相应的物理地址区间映射到一个虚拟地址区间，且虚拟地址区间基址为v时，则可以更换基址却照样可以映射
 */

