#include "arch/x86_64/abi/pgtable45.h"
#include "memory/memory_base.h"
#include "init/init_bcb_juvenile.h"
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
            // 页表分配器：直接消费纯静态 init_bcb_juvenile（首个采用者）。
            // 不再自挖连续 carve-out——每张页表页即时从幼年位图 alloc(1,12)，
            // 位图是唯一记账且可穿越至 kernel.elf，由内核端 BFS 回收所有页表。
            public:
            // 从幼年分配器取 1 页 4KB；失败返回 nullptr
            static void* alloc();
        };
        mmu_specify_allocator*pgallocator;
    public:
        kernel_mmu(arch_enums arch_specify);
        int map(vinterval inter, pgaccess access);
        int unmap(vinterval inter);
        phyaddr_t get_root_table_base();
        mem_interval get_self_alloc_interval();
        
};