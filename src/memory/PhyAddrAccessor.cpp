#include "memory/phyaddr_accessor.h"
#include "util/OS_utils.h"
#ifdef USER_MODE
#include <sys/mman.h>
#include "phyaddr_accessor.h"
#endif

// 定义PhyAddrAccessor的静态成员变量
// 根据编译时宏定义选择合适的页表根地址




#ifdef USER_MODE
    PhyAddrAccessor gAccessor;
    PhyAddrAccessor::PhyAddrAccessor()
    {
    // 设置基本描述符的大小
    BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG = 0x10000000000; // 1TB大小
    
    // 使用mmap映射一段虚拟地址空间，用于模拟物理内存访问
    void* mapped_addr = mmap(NULL, 
                             BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG, 
                             PROT_READ | PROT_WRITE, 
                             MAP_PRIVATE | MAP_ANONYMOUS|MAP_NORESERVE, 
                             -1, 
                             0);
    
    if (mapped_addr == MAP_FAILED) {
        // 如果映射失败，使用默认值
        BASIC_DESC.start = 0x10000000000;
    } else {
        BASIC_DESC.start = (vaddr_t)mapped_addr;
    }
    }
#endif
VM_DESC PhyAddrAccessor::BASIC_DESC={0};
VM_DESC PhyAddrAccessor::cache_tb[CACHE_VMDESC_MAX]={0};
void PhyAddrAccessor::Init(vm_interval basic_desc)
{
    BASIC_DESC.start=basic_desc.vbase;
    BASIC_DESC.phys_start=basic_desc.pbase;
    BASIC_DESC.access=basic_desc.access;
    BASIC_DESC.end=basic_desc.vbase+basic_desc.size;
}
uint8_t PhyAddrAccessor::readu8(phyaddr_t addr)
{
    
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint8_t*)(BASIC_DESC.start+addr);
        }
    
     
    return 0;
}

bool PhyAddrAccessor::paddr_memcpy(phyaddr_t dest, phyaddr_t src, uint64_t size)
{
    if (size == 0) return false;
    if (src >= BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG) return false;
    if (dest >= BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG) return false;
    if (src + size > BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG) return false;
    if (dest + size > BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG) return false;

    vaddr_t src_va  = BASIC_DESC.start + src;
    vaddr_t dest_va = BASIC_DESC.start + dest;
    ksystemramcpy((void*)src_va, (void*)dest_va, size);
    return true;
}
uint16_t PhyAddrAccessor::readu16(phyaddr_t addr)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint16_t*)(BASIC_DESC.start+addr);
        }
    
     
    return 0;
}

uint32_t PhyAddrAccessor::readu32(phyaddr_t addr)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint32_t*)(BASIC_DESC.start+addr);
        }
    
     
    return 0;
}

uint64_t PhyAddrAccessor::readu64(phyaddr_t addr)
{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint64_t*)(BASIC_DESC.start+addr);
        }
    
     
    return 0;
}

void PhyAddrAccessor::cache_flush(phyaddr_t addr)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            asm volatile("clflushopt (%0)" :: "r"(BASIC_DESC.start+addr) : "memory");
            return;
        }
    
     
}

void PhyAddrAccessor::cache_flush_serial(phyaddr_t addr)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            asm volatile("clflush (%0)" :: "r"(BASIC_DESC.start+addr) : "memory");
            asm volatile("mfence" ::: "memory");
            return;
        }
    
     
}

void PhyAddrAccessor::writeu8(phyaddr_t addr, uint8_t value)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint8_t*)(BASIC_DESC.start+addr)=value;
            return;
        }

    
     
}

void PhyAddrAccessor::writeu16(phyaddr_t addr, uint16_t value)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint16_t*)(BASIC_DESC.start+addr)=value;
            return;
        }
    
     
}

void PhyAddrAccessor::writeu32(phyaddr_t addr, uint32_t value)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint32_t*)(BASIC_DESC.start+addr)=value;
            return;
        }
    
     
}

void PhyAddrAccessor::writeu64(phyaddr_t addr, uint64_t value)
{

        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint64_t*)(BASIC_DESC.start+addr)=value;
            return;
        }
    
     
}
