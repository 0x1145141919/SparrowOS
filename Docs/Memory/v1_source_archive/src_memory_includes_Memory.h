#pragma once
#include  <stdint.h>
#include  <efi.h>
#include  "pgtable45.h"
#include "devices_typeids.h"
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
    ERROR_FAIL_TO_FIND,
    OS_ALLOCATABLE_MEMORY,
    OS_RESERVED_MEMORY,
    OS_PGTB_SEGS,
    MEMORY_TYPE_OEM_RESERVED_MIN = 0x70000000,
  MEMORY_TYPE_OEM_RESERVED_MAX = 0x7FFFFFFF,
  MEMORY_TYPE_OS_RESERVED_MIN  = 0x80000000,
  MEMORY_TYPE_OS_RESERVED_MAX  = 0xFFFFFFFF
} PHY_MEM_TYPE;
typedef uint64_t phyaddr_t;
typedef uint64_t vaddr_t;
#define IN
#define OUT
#pragma pack(push, 8)  // 强制8字节对齐（x64 ABI要求）

typedef struct {
    PHY_MEM_TYPE     Type;          // 4字节
    uint32_t ReservedA;
    
    EFI_PHYSICAL_ADDRESS  PhysicalStart;  // 8字节
    EFI_VIRTUAL_ADDRESS   VirtualStart;   // 8字节
    UINT64                NumberOfPages;  // 8字节
    UINT64                Attribute;      // 8字节
    UINT64                ReservedB;      // 8字节
} EFI_MEMORY_DESCRIPTORX64;

#pragma pack(pop)  // 恢复默认对齐
typedef struct phy_memDescriptor{
    PHY_MEM_TYPE                          Type;           // Field size is 32 bits followed by 32 bit pad
    uint32_t remapped_count;
    
    EFI_PHYSICAL_ADDRESS            PhysicalStart;  // Field size is 64 bits
    EFI_VIRTUAL_ADDRESS             VirtualStart;   // Field size is 64 bits
    UINT64                          NumberOfPages;  // Field size is 64 bits
    UINT64                          Attribute;      // Field size is 64 bits
    /**
     *  上面的attribute0位是否占用，1位是否读，2位是否写，3位是否可执行，
     * 
     * 上面的约定只在phy_memDesriptor *KernelSpacePgsMemMgr::queryPhysicalMemoryUsage(phyaddr_t base, uint64_t len_in_bytes)
     * 的返回结果中有效
     */
    phy_memDescriptor*submaptable;
}  phy_memDescriptor; //PHY_MEMORY_DESCRIPTOR
class GlobalMemoryPGlevelMgr_t {
    private:
    
    phy_memDescriptor rootPhyMemDscptTbBsPtr[256];//这个结构是后面在init函数中内部接口reclaimBootTimeMemory创建的
    EFI_MEMORY_DESCRIPTORX64 EfiMemMap[256];
    uint64_t EfiMemMapEntryCount;
    uint64_t rootPhymemTbentryCount;
    phyaddr_t max_phy_addr;
    struct gBasephymemmgr_flags_t
    {
        uint16_t is_alloc_service_enabled:1;//管理基本内存管理器对于基本内存管理功能是否可用
        uint64_t is_vaddr_enabled:1;//开启虚拟地址功能，就可以使用部分物理内存表项的重映射功能
    };
    gBasephymemmgr_flags_t flags;

    void sortEfiMemoryMapByPhysicalStart();
    void fillMemoryHolesInEfiMap();
    // 使用二分查找在物理内存描述符表中查找指定物理地址的描述符
    phy_memDescriptor* findDescriptorByAddress(phyaddr_t base);

  
    public:
    /*
    先检验flags.is_vaddr_enabled:1是否开启
    在物理内存描述符表中寻找指定起始物理地址的描述符
    并检查类型不要为
    MEMORY_TYPE_OEM_RESERVED_MIN = 0x70000000,
    MEMORY_TYPE_OEM_RESERVED_MAX = 0x7FFFFFFF,
    MEMORY_TYPE_OS_RESERVED_MIN  = 0x80000000,
    MEMORY_TYPE_OS_RESERVED_MAX  = 0xFFFFFFFF
    这四种类型，这四种不能增减重映射数目
    然后再对对应的描述符进行重映射数目的增减少
    能保证物理内存描述符没有内存空洞，起始地址不断增大
    使用二分查找找到对应的物理描述符
    */
 
    phyaddr_t getMaxPhyaddr();
    void printEfiMemoryDescriptorTable();
    GlobalMemoryPGlevelMgr_t();
    GlobalMemoryPGlevelMgr_t(EFI_MEMORY_DESCRIPTORX64* gEfiMemdescriptromap, uint64_t entryCount);
    // 移除析构函数声明，因为在freestanding环境中不需要
    void Init(EFI_MEMORY_DESCRIPTORX64* gEfiMemdescriptromap, uint64_t entryCount);
    phy_memDescriptor* queryPhysicalMemoryUsage(phyaddr_t addr);
    phy_memDescriptor* getGlobalPhysicalMemoryInfo();
    uint64_t getRootPhysicalMemoryDescriptorTableEntryCount();
    bool Ismemspaceneighbors(uint16_t index_a, uint16_t index_b, uint64_t tbid);
    int FixedPhyaddPgallocate(IN phyaddr_t addr,
                              IN uint64_t size,
                              IN PHY_MEM_TYPE type);                                     // 这个函数分配的内存会在根物理描述符表中增加表项
    int defaultPhyaddPgallocate( IN OUT phyaddr_t& addr,uint64_t size,PHY_MEM_TYPE type);//默认分配器，从前往后尽可能减少内存碎片
    void* SameNeighborMerge(phyaddr_t addr);
    int pageRecycle(phyaddr_t EntryStartphyaddr);//回收起始地址为addr项的内存,若是已经合并了则不可以
    void pageSetValue(phyaddr_t EntryStartphyaddr,uint64_t value);//起始地址为addr项的内存,若是已经合并了则不可以
    void printPhyMemDesTb();
    void DisableBasicMemService();
    uint64_t getGlobalPhysicalMemoryMgrflags();
    int descriptor_remapped_inc(phyaddr_t base);
  int descriptor_remapped_dec(phyaddr_t base);
  /**
   * 增加一个新增memorymappedio的成员函数
   * 有些硬件设备需要使用memorymappedio，所以需要增加这个成员函数
   * 类似于FixedPhyaddPgallocate，但是不进行位检查。
   *只有在分配的物理地址空间满足将分配物理地址区间在原描述符表中全部为Reserved,
   或一部分在Reserved,另一部分高于最大物理地址范围
   或完全高于最大物理地址范围时，才允许进行分配
   */
  int registMMIO(IN phyaddr_t addr, IN uint64_t numof_4kbpgs);
  int unregistMMIO(IN phyaddr_t addr);//只有完全高于最大物理地址范围时，而且重映射数目为0时，才允许进行释放
};
extern GlobalMemoryPGlevelMgr_t gBaseMemMgr;
