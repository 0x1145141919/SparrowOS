#include "arch/x86_64/abi/pgtable45.h"
#include "memory/memory_base.h"
#include "init/page_allocator_v2.h"
#include "util/Ktemplats.h"
#pragma once
struct vinterval{
    uint64_t phybase;
    uint64_t vbase;
    uint64_t size;    
};
struct kmmu_entry_t{
    vm_interval interval;
    char* property_name;
    uint64_t flags;
};

// ── kmmu_entry_t.flags 位定义 ──
// TRANSIENT : 过渡期映射（identity map / kIMG 窗口），不进 handoff
// PERSISTENT: 持久映射（log_buffer/GS/hdstacks/窗口/MMIO），交 kernel.elf 认领
enum : uint64_t {
    KMMU_ENTRY_FLAG_TRANSIENT  = 1ULL << 0,
    KMMU_ENTRY_FLAG_PERSISTENT = 1ULL << 1,
};

// 红黑树比较器：仅按 property_name 字典序（strcmp_in_kernel），定义在 kernel_mmu.cpp
int kmmu_entry_name_cmp(const kmmu_entry_t& a, const kmmu_entry_t& b);

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
            // 页表分配器：直接消费纯静态 page_allocator_v2（首个采用者）。
            // 每张页表页即时 free_ram_explore(1,12) + pages_set(kernel_pinned)，
            // mem_map 账本是唯一记账且可穿越至 kernel.elf，由内核端回收所有页表。
            // init.elf 侧 unmap 也会适时 free 空页表页回账本（pages_set free）。
            public:
            // 从 v2 分配器取 1 页 4KB；失败返回 nullptr
            static void* alloc();
            // 归还 1 页 4KB 回 v2 分配器
            static void free(void* page);
        };
        mmu_specify_allocator*pgallocator;
        // 名字锚点映射台账：以 property_name 字典序为键。
        // 条目自带 interval + flags，map/unmap/lookup 均围绕 kmmu_entry_t。
        Ktemplats::RBTree<kmmu_entry_t, kmmu_entry_name_cmp> m_tree;
    public:
        kernel_mmu(arch_enums arch_specify);
        // 工厂：pbase/vbase/size → vm_interval（vpn/ppn/npages），免调用点手拼错位
        static kmmu_entry_t make_entry(phyaddr_t pbase, vaddr_t vbase, uint64_t size,
                                       pgaccess access, const char* name, uint64_t flags = 0);
        int map(const kmmu_entry_t& entry);
        int unmap(const char* property_name);
        const kmmu_entry_t* lookup(const char* property_name) const;
        phyaddr_t get_root_table_base();
        mem_interval get_self_alloc_interval();
        
};