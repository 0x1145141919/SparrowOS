#include "memory/phyaddr_accessor.h"
#include "util/OS_utils.h"
#ifdef USER_MODE
#include <sys/mman.h>
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
extern "C" uint32_t assigned_cr3;
bool PhyAddrAccessor::is_init_cr3()
{
    #ifdef KERNEL_MODE
    uint64_t cr3=0;
    asm ("mov %%cr3,%0" : "=r"(cr3));
    return cr3==assigned_cr3;
    #endif
    #ifdef USER_MODE
    return false;
    #endif 
}

uint8_t PhyAddrAccessor::readu8(phyaddr_t addr)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint8_t* ptr = (volatile uint8_t*)addr;
        return *ptr;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint8_t*)(BASIC_DESC.start+addr);
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
    return 0;
}

uint16_t PhyAddrAccessor::readu16(phyaddr_t addr)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint16_t* ptr = (volatile uint16_t*)addr;
        return *ptr;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint16_t*)(BASIC_DESC.start+addr);
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
    return 0;
}

uint32_t PhyAddrAccessor::readu32(phyaddr_t addr)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint32_t* ptr = (volatile uint32_t*)addr;
        return *ptr;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint32_t*)(BASIC_DESC.start+addr);
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
    return 0;
}

uint64_t PhyAddrAccessor::readu64(phyaddr_t addr)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint64_t* ptr = (volatile uint64_t*)addr;
        return *ptr;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            return *(uint64_t*)(BASIC_DESC.start+addr);
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
    return 0;
}

void PhyAddrAccessor::cache_flush(phyaddr_t addr)
{
    if (is_init_cr3()) {
        asm volatile("clflushopt (%0)" :: "r"(addr) : "memory");
        return;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            asm volatile("clflushopt (%0)" :: "r"(BASIC_DESC.start+addr) : "memory");
            return;
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
}

void PhyAddrAccessor::cache_flush_serial(phyaddr_t addr)
{
    if (is_init_cr3()) {
        asm volatile("clflush (%0)" :: "r"(addr) : "memory");
        asm volatile("mfence" ::: "memory");
        return;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            asm volatile("clflush (%0)" :: "r"(BASIC_DESC.start+addr) : "memory");
            asm volatile("mfence" ::: "memory");
            return;
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
}

void PhyAddrAccessor::writeu8(phyaddr_t addr, uint8_t value)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint8_t* ptr = (volatile uint8_t*)addr;
        *ptr = value;
        return;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint8_t*)(BASIC_DESC.start+addr)=value;
            return;
        }

    }
    // TODO: 实现非初始化页表情况下的访问逻辑
}

void PhyAddrAccessor::writeu16(phyaddr_t addr, uint16_t value)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint16_t* ptr = (volatile uint16_t*)addr;
        *ptr = value;
        return;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint16_t*)(BASIC_DESC.start+addr)=value;
            return;
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
}

void PhyAddrAccessor::writeu32(phyaddr_t addr, uint32_t value)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint32_t* ptr = (volatile uint32_t*)addr;
        *ptr = value;
        return;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint32_t*)(BASIC_DESC.start+addr)=value;
            return;
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
}

void PhyAddrAccessor::writeu64(phyaddr_t addr, uint64_t value)
{
    if (is_init_cr3()) {
        // 如果使用初始化页表，可以直接访问物理地址（恒等映射）
        volatile uint64_t* ptr = (volatile uint64_t*)addr;
        *ptr = value;
        return;
    }else{
        if(addr<BASIC_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG){
            *(uint64_t*)(BASIC_DESC.start+addr)=value;
            return;
        }
    }
    // TODO: 实现非初始化页表情况下的访问逻辑
}
