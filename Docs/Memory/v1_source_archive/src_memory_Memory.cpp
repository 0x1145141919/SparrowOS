#include "Memory.h"
#include "VideoDriver.h"
#include "errno.h"
#include "OS_utils.h"
#include "kpoolmemmgr.h"
#define KEFLBASE 0x4000000
#define DIRTY_ENTRY 1
#define CLEAN_ENTRY 0
#define PAGE_SIZE 0x1000
#define PAGE_SIZE_4KB 0x1000
GlobalMemoryPGlevelMgr_t gBaseMemMgr;
//efi内存描述符表id为0,操作系统自己维护的内存描述符表id为1
// 默认构造函数

extern uint64_t kernel_load_addr;



void GlobalMemoryPGlevelMgr_t::Init(EFI_MEMORY_DESCRIPTORX64* gEfiMemdescriptromap, uint64_t entryCount)
/**
 * 
 */
{
    this->flags.is_alloc_service_enabled = false;
    this->flags.is_vaddr_enabled = true;
    EfiMemMapEntryCount = entryCount;
    ksystemramcpy(
        gEfiMemdescriptromap,
        EfiMemMap,
        entryCount*sizeof(EFI_MEMORY_DESCRIPTORX64)
    );
    fillMemoryHolesInEfiMap();
    for(uint64_t i=EfiMemMapEntryCount-1; i >= 0; i--)
    {
        if(EfiMemMap[i-1].Type==EFI_RESERVED_MEMORY_TYPE)
        {
            EfiMemMapEntryCount--;
        }else{
            break;
        }
    }
    /*
    扫描gEfiMemdescriptromap的同时新建EfiMemMap，回收其中启动时服务的内存，忽略后面的冗余表项，然后realloc收缩范围
    */
   int rootMapIndex = 0;
   phy_memDescriptor being_constructed_entry;
   for(uint64_t i = 0; i < EfiMemMapEntryCount; i++)
   {PHY_MEM_TYPE tmp_converted_type;
switch (EfiMemMap[i].Type)
    {
    
        case freeSystemRam:
        case EFI_LOADER_DATA:
        case EFI_LOADER_CODE:
        case EFI_BOOT_SERVICES_CODE:
        case EFI_BOOT_SERVICES_DATA:
        tmp_converted_type = freeSystemRam;
        break;
    
    default:
        tmp_converted_type = (PHY_MEM_TYPE)EfiMemMap[i].Type;
    }  
     if(i==0)
   {
    being_constructed_entry.Type = tmp_converted_type;
    being_constructed_entry.PhysicalStart = EfiMemMap[i].PhysicalStart;
    being_constructed_entry.VirtualStart = 0;
    being_constructed_entry.NumberOfPages = EfiMemMap[i].NumberOfPages;
    being_constructed_entry.Attribute = 0;
     rootPhyMemDscptTbBsPtr[rootMapIndex].NumberOfPages += EfiMemMap[i].NumberOfPages;
     rootPhyMemDscptTbBsPtr[rootMapIndex].Type = tmp_converted_type;
     rootPhyMemDscptTbBsPtr[rootMapIndex].PhysicalStart = EfiMemMap[i].PhysicalStart;
    continue;
    }
    if(tmp_converted_type==being_constructed_entry.Type)
    { 
        being_constructed_entry.NumberOfPages += EfiMemMap[i].NumberOfPages;
   }else{
        // 结束前一个合并段
        rootPhyMemDscptTbBsPtr[rootMapIndex].Type = being_constructed_entry.Type;
        rootPhyMemDscptTbBsPtr[rootMapIndex].PhysicalStart = being_constructed_entry.PhysicalStart;
        rootPhyMemDscptTbBsPtr[rootMapIndex].NumberOfPages = being_constructed_entry.NumberOfPages;
         rootPhyMemDscptTbBsPtr[rootMapIndex].remapped_count = 0;
        rootPhyMemDscptTbBsPtr[rootMapIndex].VirtualStart = 0;
        rootPhyMemDscptTbBsPtr[rootMapIndex].Attribute = 0;
        being_constructed_entry.PhysicalStart = EfiMemMap[i].PhysicalStart;
        being_constructed_entry.VirtualStart = 0;
        being_constructed_entry.Type = tmp_converted_type;
        being_constructed_entry.NumberOfPages = EfiMemMap[i].NumberOfPages;
        rootMapIndex++;
    }
   }
ksystemramcpy(
    rootPhyMemDscptTbBsPtr,
    EfiMemMap,
    rootPhymemTbentryCount*sizeof(phy_memDescriptor)
);
  rootPhymemTbentryCount = rootMapIndex;
  EfiMemMapEntryCount = rootMapIndex;
max_phy_addr=rootPhyMemDscptTbBsPtr[rootPhymemTbentryCount-1].PhysicalStart+
rootPhyMemDscptTbBsPtr[rootPhymemTbentryCount-1].NumberOfPages*PAGE_SIZE_4KB;


  
    this->flags.is_alloc_service_enabled = true;
}
phy_memDescriptor *GlobalMemoryPGlevelMgr_t::getGlobalPhysicalMemoryInfo()
{
    return rootPhyMemDscptTbBsPtr;
}
uint64_t GlobalMemoryPGlevelMgr_t::getRootPhysicalMemoryDescriptorTableEntryCount()
{
    return rootPhymemTbentryCount;
}
phy_memDescriptor *GlobalMemoryPGlevelMgr_t::queryPhysicalMemoryUsage(uint64_t addr)
{
    for(int i = 0; i < rootPhymemTbentryCount; i++)
    {
        if (rootPhyMemDscptTbBsPtr[i].PhysicalStart<=addr&&addr<
            rootPhyMemDscptTbBsPtr[i].PhysicalStart+rootPhyMemDscptTbBsPtr[i].NumberOfPages*PAGE_SIZE_4KB)
        {
            return rootPhyMemDscptTbBsPtr+i;
        }
        
    }
    return NULL;
}

// tbid对于EFI传下来的表是0,后面系统自建的物理内存描述广义表是1
/**
 * @brief 判断两个内存描述符的物理地址空间是否相邻,也就是a的尾部到b有无空洞
 *
 * @param index_a
 * @param index_b
 * @param tbid

 */
inline bool GlobalMemoryPGlevelMgr_t::Ismemspaceneighbors(uint16_t index_a, uint16_t index_b, uint64_t tbid)
{
    switch (tbid)
    {
    case 0 :
        if(EfiMemMap[index_a].PhysicalStart + 4096*EfiMemMap[index_a].NumberOfPages == EfiMemMap[index_b].PhysicalStart)
        return true;
        else return false;
    case 1 :
        if (rootPhyMemDscptTbBsPtr[index_a].PhysicalStart+
        4096*rootPhyMemDscptTbBsPtr[index_a].NumberOfPages == 
        rootPhyMemDscptTbBsPtr[index_b].PhysicalStart)
        {
            return true;
        }else return false;
        
    default:
        break;
    }    

    
}/**
     * 整体思路：
     * 1.EfiMemMap把其中的启动时服务修改成空余块
     * 2.空余块合并(脏项删除搞成另外一个私有函数进行实现)
     * 
     * 
     * 将连续的内存块进行合并涉及到
     * 
     *      * 在表中一个一个根据根后继是否连续是否删除块，
     * 1.若连续则修改后继项的起始物理地址与长度与项类型，对于前驱项在flags处标记为脏
     * 2.若不连续则只改变当前项的类型
     * 由尾部到头部遍历数组删除脏项（线性表的删除）
     * 在根据efi表回收完启动时服务相关内存后，
     * 还要手动分配一个页来复制新的物理内存描述符表，在这之后才能用物理页分配器
     * 并在新建的物理内存描述表上把0x4000000连续的内存块的描述改为OS_KERNEL_CODE
     * OS_KERNEL_DATA，上面的改动必须完全同步到物理内存描述符表与efi表
     */

// 对EFI内存表按物理起始地址排序
static int compareEfiDescriptors(const void* a, const void* b) {
    const EFI_MEMORY_DESCRIPTORX64* descA = (const EFI_MEMORY_DESCRIPTORX64*)a;
    const EFI_MEMORY_DESCRIPTORX64* descB = (const EFI_MEMORY_DESCRIPTORX64*)b;
    
    if (descA->PhysicalStart < descB->PhysicalStart) return -1;
    if (descA->PhysicalStart > descB->PhysicalStart) return 1;
    return 0;
}
void GlobalMemoryPGlevelMgr_t::sortEfiMemoryMapByPhysicalStart() {
    // 使用简单的冒泡排序，因为EFI内存表通常不会太大
    for (uint64_t i = 0; i < EfiMemMapEntryCount - 1; i++) {
        for (uint64_t j = 0; j < EfiMemMapEntryCount - i - 1; j++) {
            if (EfiMemMap[j].PhysicalStart > EfiMemMap[j + 1].PhysicalStart) {
                // 交换两个描述符
                EFI_MEMORY_DESCRIPTORX64 temp = EfiMemMap[j];
                EfiMemMap[j] = EfiMemMap[j + 1];
                EfiMemMap[j + 1] = temp;
            }
        }
    }
}
// 检测并填充内存空洞
void GlobalMemoryPGlevelMgr_t::fillMemoryHolesInEfiMap() {
    // 先对内存表排序
    sortEfiMemoryMapByPhysicalStart();
    
    // 检测内存空洞并插入Reserved类型的描述符
    for (uint64_t i = 0; i < EfiMemMapEntryCount - 1; i++) {
        uint64_t currentEnd = EfiMemMap[i].PhysicalStart + 
                             (EfiMemMap[i].NumberOfPages * EFI_PAGE_SIZE);
        uint64_t nextStart = EfiMemMap[i + 1].PhysicalStart;
        
        // 检查是否存在空洞
        if (currentEnd < nextStart) {
            // 计算空洞大小（以页为单位）
            uint64_t holePages = (nextStart - currentEnd) / EFI_PAGE_SIZE;
            
            // 创建Reserved类型的描述符
            EFI_MEMORY_DESCRIPTORX64 holeDesc ;
            setmem(&holeDesc,sizeof(EFI_MEMORY_DESCRIPTORX64),0);
            holeDesc.Type = EFI_RESERVED_MEMORY_TYPE;
            holeDesc.PhysicalStart = currentEnd;
            holeDesc.NumberOfPages = holePages;
            holeDesc.Attribute = 0;
            
            // 插入到表中
            linearTBSerialInsert(
                &EfiMemMapEntryCount,
                i + 1,
                &holeDesc,
                EfiMemMap,
                sizeof(EFI_MEMORY_DESCRIPTORX64),1
            );
            
            // 跳过刚刚插入的项
            i++;
        }
    }
}
// 回收Loader内存（非Runtime属性）

int GlobalMemoryPGlevelMgr_t::FixedPhyaddPgallocate(
    IN phyaddr_t addr,
    IN uint64_t size,
    IN PHY_MEM_TYPE type)
{
    if (flags.is_alloc_service_enabled==0)
    {
       return -EINVAL;
    }
    
    const uint64_t requiredPages = (size + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
    
    // 验证地址对齐
    if (addr % PAGE_SIZE_4KB != 0) {
        return -EINVAL;
    }
    
    phy_memDescriptor* desc = queryPhysicalMemoryUsage(addr);
    if (!desc || desc->Type != freeSystemRam) {
        return -ENOMEM;
    }
    
    const uint64_t endAddr = addr + requiredPages * PAGE_SIZE_4KB;
    const uint64_t descEnd = desc->PhysicalStart + desc->NumberOfPages * PAGE_SIZE_4KB;
    
    if (endAddr > descEnd) {
        return -ENOMEM;
    }
    
    const uint64_t beforePages = (addr - desc->PhysicalStart) / PAGE_SIZE_4KB;
    const uint64_t afterPages = (descEnd - endAddr) / PAGE_SIZE_4KB;
    
    if (beforePages > 0 && afterPages > 0) {
        // ==== 修复1：正确分裂为三个区域 ====
        phy_memDescriptor originalDesc = *desc;  // 保存原始信息
        
        // 1. 修改当前描述符为前部空闲
        desc->NumberOfPages = beforePages;  // 保持freeSystemRam类型
        
        // 2. 创建中间分配描述符
        phy_memDescriptor midDesc = originalDesc;
        midDesc.PhysicalStart = addr;
        midDesc.NumberOfPages = requiredPages;
        midDesc.Type = type;  // 设置分配类型
        
        // 3. 创建后部空闲描述符
        phy_memDescriptor afterDesc = originalDesc;
        afterDesc.PhysicalStart = endAddr;
        afterDesc.NumberOfPages = afterPages;
        
        // 插入两个新描述符（分配块+后部空闲）
        phy_memDescriptor newDescs[2] = {midDesc, afterDesc};
        linearTBSerialInsert(
            &rootPhymemTbentryCount,
            desc - rootPhyMemDscptTbBsPtr + 1,
            newDescs,
            rootPhyMemDscptTbBsPtr,
            sizeof(phy_memDescriptor),
            2  // 插入两个条目
        );
    }
    else if (beforePages > 0) {
        // 情况2：分配尾部区域
        phy_memDescriptor newDesc = *desc;
        newDesc.PhysicalStart = addr;
        newDesc.NumberOfPages = requiredPages;
        newDesc.Type = type;  // 设置分配类型
        
        // 修改原描述符为前部空闲（保持freeSystemRam类型）
        desc->NumberOfPages = beforePages;
        
        // 插入分配块
        linearTBSerialInsert(
            &rootPhymemTbentryCount,
            desc - rootPhyMemDscptTbBsPtr + 1,
            &newDesc,
            rootPhyMemDscptTbBsPtr,
            sizeof(phy_memDescriptor),1
        );
        
        // ==== 修复2：删除错误的类型设置 ====
        // desc->Type 保持 freeSystemRam 不变
    }
    else if (afterPages > 0) {
        // 情况3：分配头部区域
        phy_memDescriptor newDesc = *desc;
        newDesc.PhysicalStart = addr;
        newDesc.NumberOfPages = requiredPages;
        newDesc.Type = type;  // 设置分配类型
        
        // 修改原描述符为后部空闲
        desc->PhysicalStart = endAddr;
        desc->NumberOfPages = afterPages;
        
        // 插入分配块
        linearTBSerialInsert(
            &rootPhymemTbentryCount,
            desc - rootPhyMemDscptTbBsPtr,
            &newDesc,
            rootPhyMemDscptTbBsPtr,
            sizeof(phy_memDescriptor),1
        );
        
        // ==== 修复3：删除错误的类型设置 ====
        // desc->Type 保持 freeSystemRam 不变
    }
    else {
        // 整个块分配（无前后空闲区域）
        desc->Type = type;
    }
    
    return 0;
}

int GlobalMemoryPGlevelMgr_t::defaultPhyaddPgallocate(
    IN OUT phyaddr_t& addr,  // 通过引用修改addr
    IN uint64_t size,
    IN PHY_MEM_TYPE type)
{
        if (flags.is_alloc_service_enabled==0)
    {
       return -EINVAL;
    }
    const uint64_t requiredPages = (size + PAGE_SIZE_4KB - 1) / PAGE_SIZE_4KB;
    
    // 遍历查找合适的空闲块
    for (uint64_t i = 0; i < rootPhymemTbentryCount; ++i) {
        phy_memDescriptor* desc = &rootPhyMemDscptTbBsPtr[i];
        
        if (desc->Type == freeSystemRam && 
            desc->NumberOfPages >= requiredPages) {
            
            addr = desc->PhysicalStart;  // 设置输出地址
            return FixedPhyaddPgallocate(addr, size, type);
        }
    }
    
    return -ENOMEM;  // 没有找到合适的内存块
}


/**
 *物理内存描述符表在初始化后具有起始物理地址随表项引索增大，相邻表项之间不存在间隙的特点
 *
 * 页回收函数传入的地址必须为物理内存描述符表某一个项的物理地址，否则会报错
 * 显然这个查找操作可以使用二分查找优化
 * 删除动作就是把对应表项设置为空闲，并尝试与前后空闲表项合并
 *
 */
int GlobalMemoryPGlevelMgr_t::pageRecycle(phyaddr_t EntryStartphyaddr)
{
        if (flags.is_alloc_service_enabled==0)
    {
       return -EINVAL;
    }
    
    // 查找匹配的内存描述符项
    phy_memDescriptor* targetDesc = nullptr;
    uint64_t targetIndex = 0;
    
    // 线性查找匹配的物理地址（注释中提到可以使用二分查找优化）
    for (uint64_t i = 0; i < rootPhymemTbentryCount; i++) {
        if (rootPhyMemDscptTbBsPtr[i].PhysicalStart == EntryStartphyaddr) {
            targetDesc = &rootPhyMemDscptTbBsPtr[i];
            targetIndex = i;
            break;
        }
    }
    
    // 如果未找到匹配的项，返回错误
    if (targetDesc == nullptr) {
        return -EINVAL;
    }
    
    // 检查remapped_count字段，如果不为0则不能回收
    if (targetDesc->remapped_count != 0) {
        return -EBUSY;  // 资源正忙，无法回收
    }
    
    // 将该项标记为空闲内存
    targetDesc->Type = freeSystemRam;
    
    // 尝试与前一个表项合并
    if (targetIndex > 0) {
        phy_memDescriptor* prevDesc = &rootPhyMemDscptTbBsPtr[targetIndex - 1];
        // 检查前一个表项是否为空闲且与当前项相邻
        if (prevDesc->Type == freeSystemRam && 
            prevDesc->PhysicalStart + (prevDesc->NumberOfPages << 12) == targetDesc->PhysicalStart) {
            // 合并到前一个表项
            prevDesc->NumberOfPages += targetDesc->NumberOfPages;
            // 将当前项标记为无效（通过将后续项前移来"删除"它）
            for (uint64_t i = targetIndex; i < rootPhymemTbentryCount - 1; i++) {
                rootPhyMemDscptTbBsPtr[i] = rootPhyMemDscptTbBsPtr[i + 1];
            }
            rootPhymemTbentryCount--;
        }
    }
    
    // 尝试与后一个表项合并
    if (targetIndex < rootPhymemTbentryCount - 1) {
        phy_memDescriptor* nextDesc = &rootPhyMemDscptTbBsPtr[targetIndex + 1];
        // 检查后一个表项是否为空闲且与当前项相邻
        if (nextDesc->Type == freeSystemRam && 
            targetDesc->PhysicalStart + (targetDesc->NumberOfPages << 12) == nextDesc->PhysicalStart) {
            // 合并到当前表项
            targetDesc->NumberOfPages += nextDesc->NumberOfPages;
            // 将后续项前移来"删除"后一个表项
            for (uint64_t i = targetIndex + 1; i < rootPhymemTbentryCount - 1; i++) {
                rootPhyMemDscptTbBsPtr[i] = rootPhyMemDscptTbBsPtr[i + 1];
            }
            rootPhymemTbentryCount--;
        }
    }
    
    return OS_SUCCESS; // 回收成功
}

void GlobalMemoryPGlevelMgr_t::pageSetValue(phyaddr_t EntryStartphyaddr, uint64_t value)
{
    
    phy_memDescriptor* Entry = queryPhysicalMemoryUsage(EntryStartphyaddr);
    if(Entry)
    {uint64_t* p = (uint64_t*)Entry->PhysicalStart;
        for (int i = 0; i < Entry->NumberOfPages*512; i++)
        {
            p[i]=value;
        }
        
    }else{
        kputsSecure("pageSetValue:Entry is null,Invalid EntryStartphyaddr");
    }
    
}

const char *MemoryTypeToString(UINT32 type)
{
    switch (type) {
        case 0:  return "Reserved";
        case 1:  return "LoaderCode";
        case 2:  return "LoaderData";
        case 3:  return "BootServicesCode";
        case 4:  return "BootServicesData";
        case 5:  return "RuntimeServicesCode";
        case 6:  return "RuntimeServicesData";
        case 7:  return "ConventionalMemory";
        case 8:  return "UnusableMemory";
        case 9:  return "ACPIReclaimMemory";
        case 10: return "ACPIMemoryNVS";
        case 11: return "MemoryMappedIO";
        case 12: return "MemoryMappedIOPortSpace";
        case 13: return "PalCode";
        case 14: return "PERSISTENT_MEMORY";
        case 15: return "EFI_UNACCEPTED_MEMORY_TYPE";
        case 16: return "EFI_MAX_MEMORY_TYPE";
        case 17: return "OS_KERNEL_DATA";
        case 18: return "OS_KERNEL_CODE";
        case 19: return "OS_KERNEL_STACK";

        default: return "UnknownType";
    }
}

// 打印单个内存描述符
void PrintMemoryDescriptor(const EFI_MEMORY_DESCRIPTORX64* desc) {
    // 1. 打印起始物理地址
    kputsSecure("Start: 0x");
    kpnumSecure((void*)&desc->PhysicalStart, UNHEX, sizeof(EFI_PHYSICAL_ADDRESS));
    
    // 2. 计算并打印终止物理地址
    UINT64 endAddress = desc->PhysicalStart + (desc->NumberOfPages * EFI_PAGE_SIZE);
    kputsSecure(" - End: 0x");
    kpnumSecure((void*)&endAddress, UNHEX, sizeof(UINT64));
    
    // 3. 打印内存类型和属性
    kputsSecure(" Type: ");
    kputsSecure((char*)MemoryTypeToString(desc->Type));
    
    kputsSecure(" Attr: 0x");
    kpnumSecure((void*)&desc->Attribute, UNHEX, sizeof(UINT64));
    
    // 4. 打印页数（可选）
    kputsSecure(" Pages: ");
    kpnumSecure((void*)&desc->NumberOfPages, UNDEC, sizeof(UINT64));
    
    // 换行
    kputsSecure("\n");
}
phyaddr_t GlobalMemoryPGlevelMgr_t::getMaxPhyaddr()
{
    return max_phy_addr;
}
void GlobalMemoryPGlevelMgr_t::printEfiMemoryDescriptorTable()
{
        kputsSecure("\n========== Memory Map ==========\n");
    
    // 打印表头
    kputsSecure("Physical Range             Type               Attribute    Pages\n");
    kputsSecure("-----------------------------------------------------------------\n");
    
    // 遍历所有条目
    for (UINTN i = 0; i < EfiMemMapEntryCount; i++) {
        kpnumSecure((void*)&i, UNDEC, sizeof(UINTN));
        kputsSecure(": ");
        PrintMemoryDescriptor(EfiMemMap+i);
    }
    
    kputsSecure("========== End of Map ==========\n");
}

GlobalMemoryPGlevelMgr_t::GlobalMemoryPGlevelMgr_t()
{
}

GlobalMemoryPGlevelMgr_t::GlobalMemoryPGlevelMgr_t(EFI_MEMORY_DESCRIPTORX64 *gEfiMemdescriptromap, uint64_t entryCount)
{
}

void GlobalMemoryPGlevelMgr_t::printPhyMemDesTb()
{
       kputsSecure("\n========== Phy Memory Map ==========\n");
    
    // 打印表头
    kputsSecure("Physical Range             Type               Attribute    Pages\n");
    kputsSecure("-----------------------------------------------------------------\n");
    
    // 遍历所有条目
    for (UINTN i = 0; i < rootPhymemTbentryCount; i++) {
        kpnumSecure((void*)&i, UNDEC, sizeof(UINTN));
        kputsSecure(": ");
        PrintMemoryDescriptor((EFI_MEMORY_DESCRIPTORX64*)(rootPhyMemDscptTbBsPtr+i));
    }
    
    kputsSecure("========== End of Map ==========\n");
}

void GlobalMemoryPGlevelMgr_t::DisableBasicMemService()
{
    flags.is_alloc_service_enabled=0;
}

// 使用二分查找在物理内存描述符表中查找指定物理地址的描述符
phy_memDescriptor* GlobalMemoryPGlevelMgr_t::findDescriptorByAddress(phyaddr_t base) {
    if (rootPhymemTbentryCount == 0) {
        return nullptr;
    }
    
    int left = 0;
    int right = rootPhymemTbentryCount - 1;
    
    while (left <= right) {
        int mid = (left + right) / 2;
        phy_memDescriptor& desc = rootPhyMemDscptTbBsPtr[mid];
        
        if (desc.PhysicalStart == base) {
            return &rootPhyMemDscptTbBsPtr[mid];
        } else if (desc.PhysicalStart < base) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    
    return nullptr;
}

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
int GlobalMemoryPGlevelMgr_t::descriptor_remapped_inc(phyaddr_t base) {
    // 检查虚拟地址功能是否开启
    if (!flags.is_vaddr_enabled) {
        return -EPERM;
    }
    
    // 使用二分查找找到对应的物理描述符
    phy_memDescriptor* desc = findDescriptorByAddress(base);
    if (desc == nullptr) {
        return -EINVAL;
    }
    
    // 检查类型是否为不允许增减重映射数目的类型
    if ((desc->Type >= MEMORY_TYPE_OEM_RESERVED_MIN && desc->Type <= MEMORY_TYPE_OEM_RESERVED_MAX) ||
        (desc->Type >= MEMORY_TYPE_OS_RESERVED_MIN && desc->Type <= MEMORY_TYPE_OS_RESERVED_MAX)) {
        return -EPERM;
    }
    
    // 增加重映射数目
    desc->remapped_count++;
    return OS_SUCCESS;
}

int GlobalMemoryPGlevelMgr_t::descriptor_remapped_dec(phyaddr_t base) {
    // 检查虚拟地址功能是否开启
    if (!flags.is_vaddr_enabled) {
        return -EPERM;
    }
    
    // 使用二分查找找到对应的物理描述符
    phy_memDescriptor* desc = findDescriptorByAddress(base);
    if (desc == nullptr) {
        return -EINVAL;
    }
    
    // 检查类型是否为不允许增减重映射数目的类型
    if ((desc->Type >= MEMORY_TYPE_OEM_RESERVED_MIN && desc->Type <= MEMORY_TYPE_OEM_RESERVED_MAX) ||
        (desc->Type >= MEMORY_TYPE_OS_RESERVED_MIN && desc->Type <= MEMORY_TYPE_OS_RESERVED_MAX)) {
        return -EPERM;
    }
    
    // 检查重映射数目是否已经为0
    if (desc->remapped_count == 0) {
        return -EINVAL;
    }
    
    // 减少重映射数目
    desc->remapped_count--;
    return OS_SUCCESS;
}

/**
 * 增加一个新增memorymappedio的成员函数
 * 有些硬件设备需要使用memorymappedio，所以需要增加这个成员函数
 * 类似于FixedPhyaddPgallocate，但是不进行位检查。
 * 只有在分配的物理地址空间满足将分配物理地址区间在原描述符表中全部为Reserved,
 * 或一部分在Reserved,另一部分高于最大物理地址范围
 * 或完全高于最大物理地址范围时，才允许进行分配
 */
int GlobalMemoryPGlevelMgr_t::registMMIO(IN phyaddr_t addr, IN uint64_t numof_4kbpgs) {

    
    // 验证地址对齐
    if (addr % PAGE_SIZE_4KB != 0) {
        return -EINVAL;
    }
    
    const uint64_t requiredPages = numof_4kbpgs;
    const uint64_t endAddr = addr + requiredPages * PAGE_SIZE_4KB;
    
    // 检查地址范围是否有效
    // 如果完全在物理内存范围内，需要检查是否全部为Reserved类型
    if (endAddr <= max_phy_addr) {
        // 完全在物理内存范围内
        bool all_reserved = true;
        phyaddr_t current_addr = addr;
        
        while (current_addr < endAddr) {
            phy_memDescriptor* desc = queryPhysicalMemoryUsage(current_addr);
            if (!desc) {
                all_reserved = false;
                break;
            }
            
            // 检查当前描述符是否为Reserved类型
            if (desc->Type != EFI_RESERVED_MEMORY_TYPE) {
                all_reserved = false;
                break;
            }
            
            phyaddr_t desc_end = desc->PhysicalStart + desc->NumberOfPages * PAGE_SIZE_4KB;
            current_addr = (endAddr < desc_end) ? endAddr : desc_end;
        }
        
        if (!all_reserved) {
            return -EPERM; // 不满足条件，不允许分配
        }
    } 
    // 如果部分在物理内存范围内，部分超出
    else if (addr < max_phy_addr) {
        // 检查物理内存范围内的部分是否为Reserved类型
        phyaddr_t current_addr = addr;
        bool all_reserved = true;
        
        while (current_addr < max_phy_addr) {
            phy_memDescriptor* desc = queryPhysicalMemoryUsage(current_addr);
            if (!desc) {
                all_reserved = false;
                break;
            }
            
            // 检查当前描述符是否为Reserved类型
            if (desc->Type != EFI_RESERVED_MEMORY_TYPE) {
                all_reserved = false;
                break;
            }
            
            phyaddr_t desc_end = desc->PhysicalStart + desc->NumberOfPages * PAGE_SIZE_4KB;
            current_addr = (max_phy_addr < desc_end) ? max_phy_addr : desc_end;
        }
        
        if (!all_reserved) {
            return -EPERM; // 不满足条件，不允许分配
        }
    }
    // 如果完全高于最大物理地址范围，则直接允许分配
    
    // 执行分配操作，类似于FixedPhyaddPgallocate，但类型为EFI_MEMORY_MAPPED_IO
    phy_memDescriptor* desc = queryPhysicalMemoryUsage(addr);
    if (!desc) {
        // 创建新的描述符项（完全高于物理内存范围的情况）
        phy_memDescriptor newDesc;
        setmem(&newDesc, sizeof(phy_memDescriptor), 0);
        newDesc.Type = EFI_MEMORY_MAPPED_IO;
        newDesc.PhysicalStart = addr;
        newDesc.NumberOfPages = requiredPages;
        newDesc.Attribute = 0;
        newDesc.remapped_count = 0;
        
        // 在适当位置插入新描述符
        // 找到插入位置
        uint64_t insertIndex = 0;
        for (insertIndex = 0; insertIndex < rootPhymemTbentryCount; insertIndex++) {
            if (rootPhyMemDscptTbBsPtr[insertIndex].PhysicalStart > addr) {
                break;
            }
        }
        
        linearTBSerialInsert(
            &rootPhymemTbentryCount,
            insertIndex,
            &newDesc,
            rootPhyMemDscptTbBsPtr,
            sizeof(phy_memDescriptor),
            1
        );
    } else {
        // 修改现有描述符（在物理内存范围内的部分）
        const uint64_t descEnd = desc->PhysicalStart + desc->NumberOfPages * PAGE_SIZE_4KB;
        
        if (endAddr > descEnd) {
            return -ENOMEM; // 超出当前描述符范围
        }
        
        const uint64_t beforePages = (addr - desc->PhysicalStart) / PAGE_SIZE_4KB;
        const uint64_t afterPages = (descEnd - endAddr) / PAGE_SIZE_4KB;
        
        if (beforePages > 0 && afterPages > 0) {
            // 分裂为三个区域
            phy_memDescriptor originalDesc = *desc;
            
            // 1. 修改当前描述符为前部保留
            desc->NumberOfPages = beforePages;
            
            // 2. 创建中间MMIO描述符
            phy_memDescriptor midDesc = originalDesc;
            midDesc.PhysicalStart = addr;
            midDesc.NumberOfPages = requiredPages;
            midDesc.Type = EFI_MEMORY_MAPPED_IO;
            
            // 3. 创建后部保留描述符
            phy_memDescriptor afterDesc = originalDesc;
            afterDesc.PhysicalStart = endAddr;
            afterDesc.NumberOfPages = afterPages;
            
            // 插入两个新描述符（MMIO块+后部保留）
            phy_memDescriptor newDescs[2] = {midDesc, afterDesc};
            linearTBSerialInsert(
                &rootPhymemTbentryCount,
                desc - rootPhyMemDscptTbBsPtr + 1,
                newDescs,
                rootPhyMemDscptTbBsPtr,
                sizeof(phy_memDescriptor),
                2
            );
        }
        else if (beforePages > 0) {
            // 分配尾部区域
            phy_memDescriptor newDesc = *desc;
            newDesc.PhysicalStart = addr;
            newDesc.NumberOfPages = requiredPages;
            newDesc.Type = EFI_MEMORY_MAPPED_IO;
            
            // 修改原描述符为前部保留
            desc->NumberOfPages = beforePages;
            
            // 插入MMIO块
            linearTBSerialInsert(
                &rootPhymemTbentryCount,
                desc - rootPhyMemDscptTbBsPtr + 1,
                &newDesc,
                rootPhyMemDscptTbBsPtr,
                sizeof(phy_memDescriptor),
                1
            );
        }
        else if (afterPages > 0) {
            // 分配头部区域
            phy_memDescriptor newDesc = *desc;
            newDesc.PhysicalStart = addr;
            newDesc.NumberOfPages = requiredPages;
            newDesc.Type = EFI_MEMORY_MAPPED_IO;
            
            // 修改原描述符为后部保留
            desc->PhysicalStart = endAddr;
            desc->NumberOfPages = afterPages;
            
            // 插入MMIO块
            linearTBSerialInsert(
                &rootPhymemTbentryCount,
                desc - rootPhyMemDscptTbBsPtr,
                &newDesc,
                rootPhyMemDscptTbBsPtr,
                sizeof(phy_memDescriptor),
                1
            );
        }
        else {
            // 整个块分配（无前后保留区域）
            desc->Type = EFI_MEMORY_MAPPED_IO;
        }
    }
    
    return OS_SUCCESS;
}

int GlobalMemoryPGlevelMgr_t::unregistMMIO(IN phyaddr_t addr) {
    // 查找匹配的内存描述符项
    phy_memDescriptor* targetDesc = nullptr;
    uint64_t targetIndex = 0;
    
    // 线性查找匹配的物理地址
    for (uint64_t i = 0; i < rootPhymemTbentryCount; i++) {
        if (rootPhyMemDscptTbBsPtr[i].PhysicalStart == addr) {
            targetDesc = &rootPhyMemDscptTbBsPtr[i];
            targetIndex = i;
            break;
        }
    }
    
    // 如果未找到匹配的项，返回错误
    if (targetDesc == nullptr) {
        return -EINVAL;
    }
    
    // 检查是否为MMIO类型
    if (targetDesc->Type != EFI_MEMORY_MAPPED_IO) {
        return -EPERM;
    }
    
    // 只有完全高于最大物理地址范围时，而且重映射数目为0时，才允许进行释放
    if (addr >= max_phy_addr && targetDesc->remapped_count == 0) {
        // 删除该项
        for (uint64_t i = targetIndex; i < rootPhymemTbentryCount - 1; i++) {
            rootPhyMemDscptTbBsPtr[i] = rootPhyMemDscptTbBsPtr[i + 1];
        }
        rootPhymemTbentryCount--;
        return OS_SUCCESS;
    }
    
    // 如果不满足条件，不允许释放
    return -EPERM;
}
