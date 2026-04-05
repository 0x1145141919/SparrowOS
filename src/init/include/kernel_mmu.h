#include "arch/x86_64/abi/pgtable45.h"
#include "memory/memory_base.h"
#pragma once
struct vinterval{
    uint64_t phybase;
    uint64_t vbase;
    uint64_t size;    
};

enum arch_enums{
    x86_64_PGLV4,
    x86_64_PGLV5
};
using phyaddr_t = uint64_t;
struct mem_interval;
class kernel_mmu{
        uint16_t arch_specify;//暂时只支持x86_64_PGLV4
        void*root_table;//根表,物理地址
        class mmu_specify_allocator{
            //极简分配器，只分配不回收，
            //初始化逻辑为找basic_allocator申请一片default_mgr_size,align_log2=12的内存
            public:
            uint64_t base;
            static constexpr uint64_t default_mgr_size=0x40000;
            uint64_t size;
            uint64_t top;
            public:
            mmu_specify_allocator();
            //分配逻辑为检验top==base+size,为真返回空指针表示用尽
            //为假返回top+=4096并返回原来的值
            void* alloc();
        };
        mmu_specify_allocator*pgallocator;
    public:
        kernel_mmu(arch_enums arch_specify);
        int map(vinterval inter, pgaccess access);
        int unmap(vinterval inter);
        phyaddr_t get_root_table_base();
        mem_interval get_self_alloc_interval();
        
};