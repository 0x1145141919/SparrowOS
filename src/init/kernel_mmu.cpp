#include "../init/include/kernel_mmu.h"
#include "../init/include/pages_alloc.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
// 类型别名，简化嵌套类型名的使用
using pages_info_t = seg_to_pages_info_pakage_t::pages_info_t;

/**
 * @brief 将缓存策略转换为 PAT/PCD/PWT 索引
 * 
 * 仿照 KspacMapMgr::cache_strategy_to_idx 的实现，
 * 根据 cache_strategy 查找其在 PAT 配置表中的索引位置，
 * 并返回对应的 PWT、PCD、PAT 位值。
 * 
 * 查找算法:
 * 1. 遍历 DEFAULT_PAT_CONFIG.mapped_entry[0..7]
 * 2. 找到与 cache_strategy 匹配的条目
 * 3. 提取索引 i 的低 3 位作为 PWT、PCD、PAT
 * 
 * @param cache_strategy 缓存策略枚举值
 * @return cache_table_idx_struct_t 包含 PWT、PCD、PAT 三位的结果结构体
 */
cache_table_idx_struct_t cache_strategy_to_idx(cache_strategy_t cache_strategy)
{   
    uint8_t i = 0;
    // 在 PAT 配置表中查找匹配的条目
    for (; i < 8; i++) {
        if (cache_strategy == DEFAULT_PAT_CONFIG.mapped_entry[i]) {
            break;
        }
    }
    
    // 如果未找到（i==8），使用默认值 0（UC 类型）
    if (i >= 8) {
        i = 0;
    }
    
    // 提取低 3 位：bit0=PWT, bit1=PCD, bit2=PAT
    cache_table_idx_struct_t result = {
        .PWT = (uint8_t)(i & 1),
        .PCD = (uint8_t)((i >> 1) & 1),
        .PAT = (uint8_t)((i >> 2) & 1)
    };
    return result;
}

/**
 * @brief vinterval 类的 end() 方法，返回区间结束地址
 */
static inline uint64_t vinterval_end(const vinterval& inter)
{
    return inter.vbase + inter.size;
}

/**
 * @brief mmu_specify_allocator 构造函数
 * 
 * 向 basic_allocator 申请一片默认大小的内存用于页表分配
 * 初始化逻辑:
 * 1. 调用 basic_allocator::pages_alloc 申请 default_mgr_size 大小的内存
 * 2. align_log2=12 (4KB 对齐)
 * 3. 初始化 base, size, top 成员
 */
kernel_mmu::mmu_specify_allocator::mmu_specify_allocator()
{
    // 向 basic_allocator 申请内存
    phyaddr_t phys_addr = basic_allocator::pages_alloc(default_mgr_size, 12);
    
    if (phys_addr == ~0ull) {
        // 分配失败，初始化为无效值
        base = 0;
        size = 0;
        top = 0;
    } else {
        base = phys_addr;
        size = default_mgr_size;
        top = base;
        
        // 标记这片内存为已使用
        mem_interval interval = {base, size};
        basic_allocator::pages_set(interval, OS_KERNEL_DATA);
    }
}

/**
 * @brief 分配一个物理页 (4KB)
 * 
 * 分配逻辑:
 * 1. 检查 top == base + size，如果相等说明内存已用尽，返回空指针
 * 2. 否则返回当前 top 值，并将 top += 4096
 * 
 * @return void* 成功返回物理地址对应的指针，失败返回 nullptr
 */
void* kernel_mmu::mmu_specify_allocator::alloc()
{
    // 检查是否已用尽
    if (top >= base + size) {
        return nullptr;
    }
    
    // 返回当前 top 并递增
    void* result = reinterpret_cast<void*>(top);
    top += 0x1000; // 增加 4KB
    
    return result;
}

/**
 * @brief kernel_mmu 构造函数
 * 
 * 初始化 MMU 管理器:
 * 1. 保存架构类型 (x86_64_PGLV4 或 x86_64_PGLV5)
 * 2. 创建页表分配器 pgallocator
 * 3. 从 pgallocator 分配根表 (root_table)
 * 
 * 对于 x86_64 PGLV4:
 * - 根表为 PML4 表，占用 1 个物理页 (4KB)
 * - 需要 4KB 对齐
 * - 最多支持 512 个 PML4 条目
 */
kernel_mmu::kernel_mmu(arch_enums arch_specify)
{
    this->arch_specify = arch_specify;
    
    // 创建页表分配器
    pgallocator = new mmu_specify_allocator();
    if (!pgallocator) {
        root_table = nullptr;
        return;
    }
    
    // 从 pgallocator 分配根表
    root_table = pgallocator->alloc();
    
    if (!root_table) {
        // 分配失败，清理 pgallocator
        delete pgallocator;
        pgallocator = nullptr;
        return;
    }
    
    // 初始化根表内容 (清零)
    uint64_t* pml4_ptr = reinterpret_cast<uint64_t*>(root_table);
    for (int i = 0; i < 512; i++) {
        pml4_ptr[i] = 0;
    }
}

/**
 * @brief 将虚拟地址区间拆分为不同大小的页面（1GB → 2MB → 4KB）
 * 
 * 仿照 AddressSpace::seg_to_pages_info_get 和 KspaceMapMgr::seg_to_pages_info_get 的实现，
 * 采用状态机方式同时维护虚拟地址和物理地址，尽可能使用大页以减少 TLB miss。
 * 
 * 拆分策略:
 * 1. 使用状态机维护当前虚拟地址 cur_v 和物理地址 cur_p
 * 2. 循环处理直到所有内存分配完毕
 * 3. 每次迭代优先尝试 1GB，然后 2MB，最后 4KB
 * 4. 只有当剩余大小 >= 页面大小 且 v/p 地址都对齐时才使用该页面
 * 5. 更新 cur_v 和 cur_p，继续下一次迭代
 * 
 * 时间复杂度：O(1)，最多生成 5 个条目
 * 空间复杂度：O(1)
 * 
 * @param result 输出参数，存储拆分后的页面信息数组（最多 5 个条目）
 * @param inter 待拆分的虚拟地址区间
 * @return int 成功返回 0，失败返回负值错误码
 */
static int split_vinterval_to_pages(seg_to_pages_info_pakage_t& result, const vinterval& inter)
{
    // 参数校验
   if(inter.vbase % 0x1000 != 0 || inter.phybase % 0x1000 != 0) {
        return -1; // 未对齐
    }
    
   if (inter.size % 0x1000 != 0) {
        return -2; // 大小不是 4KB 倍数
    }
    
    // 初始化结果数组
    for (int i = 0; i < 5; i++) {
        result.entryies[i].vbase = 0;
        result.entryies[i].phybase = 0;
        result.entryies[i].page_size_in_byte = 0;
        result.entryies[i].num_of_pages = 0;
    }
    
   constexpr uint64_t _4KB_SIZE = 0x1000;
   constexpr uint64_t _2MB_SIZE = 0x200000;
   constexpr uint64_t _1GB_SIZE = 0x40000000ULL;
    
    // 状态机：维护当前虚拟地址和物理地址
   uint64_t cur_v = inter.vbase;
   uint64_t cur_p = inter.phybase;
   uint64_t remaining = inter.size;
    
    int idx = 0;
    
    // 循环处理直到所有内存分配完毕
    while (remaining > 0 && idx < 5) {
       uint64_t page_size = 0;
       uint64_t num_pages = 0;
        
        // 尝试使用最大的可用页面大小
       if (remaining >= _1GB_SIZE &&
            (cur_v % _1GB_SIZE == 0) &&
            (cur_p % _1GB_SIZE == 0)) {
            // 虚拟地址和物理地址都 1GB 对齐，可以使用 1GB 页面
           page_size = _1GB_SIZE;
            num_pages = remaining / _1GB_SIZE;
        } 
        else if(remaining >= _2MB_SIZE &&
                 (cur_v % _2MB_SIZE == 0) &&
                 (cur_p % _2MB_SIZE == 0)) {
            // 虚拟地址和物理地址都 2MB 对齐，可以使用 2MB 页面
           page_size = _2MB_SIZE;
            num_pages = remaining / _2MB_SIZE;
        } 
        else {
            // 使用 4KB 页面
           page_size = _4KB_SIZE;
            num_pages = remaining / _4KB_SIZE;
            
            // 如果不足一个完整的 4KB 页面，向上取整
           if (remaining % _4KB_SIZE != 0) {
                num_pages++;
            }
            
            // 确保不超过剩余长度
           if (num_pages * _4KB_SIZE > remaining) {
                num_pages = remaining / _4KB_SIZE;
            }
        }
        
        // 填充当前 entry
        result.entryies[idx].vbase = cur_v;
        result.entryies[idx].phybase = cur_p;
        result.entryies[idx].page_size_in_byte = page_size;
        result.entryies[idx].num_of_pages = num_pages;
        
        // 更新当前地址和剩余长度
       uint64_t mapped_len = num_pages * page_size;
       cur_v += mapped_len;
       cur_p += mapped_len;
        remaining = (remaining > mapped_len) ? (remaining - mapped_len) : 0;
        
        idx++;
        
        // 如果已经使用了 5 个 entry 但还有剩余区域，需要特殊处理
       if (idx >= 5 && remaining > 0) {
            // 合并最后一个 entry，使其包含所有剩余区域
            idx = 4;  // 回到最后一个 entry
            result.entryies[idx].num_of_pages += remaining / _4KB_SIZE;
           if (remaining % _4KB_SIZE != 0) {
                result.entryies[idx].num_of_pages++;
            }
            break;
        }
    }
    
    // 检查是否超出数组容量
   if(remaining > 0) {
        // 理论上不应该发生，因为最坏情况下全部用 4KB 页也能装下
        return -3;
    }
    
    return 0;
}

/**
 * @brief 建立虚拟地址到物理地址的映射（仅支持x86_64 PGLV4）
 * 
 * 仿照 AddressSpace::enable_VM_desc 的实现逻辑，
 * 先按同余等级拆分区间（优先大页），再逐级建立映射。
 * 
 * 映射策略:
 * 1. 调用 vm_interval_to_pages_info 拆分区间为 1GB/2MB/4KB
 * 2. 按同余等级分发映射策略
 * 3. 1GB 页可能跨 PDPT 边界，需要循环处理
 * 
 * 假设条件:
 * - 运行在恒等映射环境，页表指针可直接解引用
 * - 只支持x86_64 PGLV4 架构
 * 
 * @param inter 虚拟地址区间信息
 * @param access 访问权限控制
 * @return int 成功返回 0，失败返回负值错误码
 */
int kernel_mmu::map(vinterval inter, pgaccess access)
{
    // 参数校验
    if (!root_table || !pgallocator) {
        return -1;
    }
    
    if (inter.vbase % 0x1000 != 0 || inter.phybase % 0x1000 != 0) {
        return -2; // 未对齐
    }
    
    if (inter.size % 0x1000 != 0) {
        return -3; // 大小不是 4KB 倍数
    }
    
    // 将缓存策略转换为 PAT/PCD/PWT 索引
    cache_table_idx_struct_t cache_table_idx = cache_strategy_to_idx(access.cache_strategy);
    
    // 拆分区间（按同余等级）
    seg_to_pages_info_pakage_t package;
    VM_DESC vmentry = {};
    vmentry.start = inter.vbase;
    vmentry.end = vinterval_end(inter);
    vmentry.phys_start = inter.phybase;
    vmentry.access = access;
    int status = vm_interval_to_pages_info(package, vmentry);
    if (status != 0) {
        return status;
    }

    auto map_4kb = [&](uint64_t vaddr, uint64_t paddr, uint64_t num_pages) -> int {
        if (num_pages == 0 || num_pages > 512) return -4;
        uint16_t pml4_index = (vaddr >> 39) & 0x1FF;
        uint16_t pdpte_index = (vaddr >> 30) & 0x1FF;
        uint16_t pde_index = (vaddr >> 21) & 0x1FF;
        uint16_t pte_index = (vaddr >> 12) & 0x1FF;
        if (pte_index + num_pages > 512) return -5;

        PageTableEntryUnion* pml4_ptr = reinterpret_cast<PageTableEntryUnion*>(root_table);
        PageTableEntryUnion& pml4e = pml4_ptr[pml4_index];
        if (!pml4e.pml4.present) {
            void* pdpt_page = pgallocator->alloc();
            if (!pdpt_page) return -6;
            PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_page);
            for (int j = 0; j < 512; j++) pdpt_ptr[j].raw = 0;
            pml4e.raw = 0;
            pml4e.pml4.present = 1;
            pml4e.pml4.RWbit = 1;
            pml4e.pml4.KERNELbit = 1;
            pml4e.pml4.pdpte_addr = (uint64_t)pdpt_page >> 12;
        }

        uint64_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
        PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_base);
        PageTableEntryUnion& pdpte = pdpt_ptr[pdpte_index];
        if (!pdpte.pdpte.present) {
            void* pd_page = pgallocator->alloc();
            if (!pd_page) return -6;
            PageTableEntryUnion* pd_ptr = reinterpret_cast<PageTableEntryUnion*>(pd_page);
            for (int j = 0; j < 512; j++) pd_ptr[j].raw = 0;
            pdpte.raw = 0;
            pdpte.pdpte.present = 1;
            pdpte.pdpte.RWbit = 1;
            pdpte.pdpte.KERNELbit = 1;
            pdpte.pdpte.PD_addr = (uint64_t)pd_page >> 12;
        }

        uint64_t pd_base = pdpte.pdpte.PD_addr << 12;
        PageTableEntryUnion* pd_ptr = reinterpret_cast<PageTableEntryUnion*>(pd_base);
        PageTableEntryUnion& pde = pd_ptr[pde_index];
        if (!pde.pde.present) {
            void* pt_page = pgallocator->alloc();
            if (!pt_page) return -6;
            PageTableEntryUnion* pt_ptr = reinterpret_cast<PageTableEntryUnion*>(pt_page);
            for (int j = 0; j < 512; j++) pt_ptr[j].raw = 0;
            pde.raw = 0;
            pde.pde.present = 1;
            pde.pde.RWbit = 1;
            pde.pde.KERNELbit = 1;
            pde.pde.pt_addr = (uint64_t)pt_page >> 12;
        }

        uint64_t pt_base = pde.pde.pt_addr << 12;
        PageTableEntryUnion* pt_ptr = reinterpret_cast<PageTableEntryUnion*>(pt_base);
        for (uint64_t j = 0; j < num_pages; j++) {
            PageTableEntryUnion& pte = pt_ptr[pte_index + j];
            pte.raw = 0;
            pte.pte.present = 1;
            pte.pte.RWbit = access.is_writeable;
            pte.pte.KERNELbit = !access.is_kernel;
            pte.pte.page_addr = (paddr + j * 0x1000) >> 12;
            pte.pte.global = access.is_global;
            pte.pte.EXECUTE_DENY = !access.is_executable;
            pte.pte.PWT = cache_table_idx.PWT;
            pte.pte.PCD = cache_table_idx.PCD;
            pte.pte.PAT = cache_table_idx.PAT;
        }
        return 0;
    };

    auto map_2mb = [&](uint64_t vaddr, uint64_t paddr, uint64_t num_pages) -> int {
        if (num_pages == 0 || num_pages > 512) return -4;
        uint16_t pml4_index = (vaddr >> 39) & 0x1FF;
        uint16_t pdpte_index = (vaddr >> 30) & 0x1FF;
        uint16_t pde_index = (vaddr >> 21) & 0x1FF;
        if (pde_index + num_pages > 512) return -5;

        PageTableEntryUnion* pml4_ptr = reinterpret_cast<PageTableEntryUnion*>(root_table);
        PageTableEntryUnion& pml4e = pml4_ptr[pml4_index];
        if (!pml4e.pml4.present) {
            void* pdpt_page = pgallocator->alloc();
            if (!pdpt_page) return -6;
            PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_page);
            for (int j = 0; j < 512; j++) pdpt_ptr[j].raw = 0;
            pml4e.raw = 0;
            pml4e.pml4.present = 1;
            pml4e.pml4.RWbit = 1;
            pml4e.pml4.KERNELbit = 1;
            pml4e.pml4.pdpte_addr = (uint64_t)pdpt_page >> 12;
        }

        uint64_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
        PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_base);
        PageTableEntryUnion& pdpte = pdpt_ptr[pdpte_index];
        if (!pdpte.pdpte.present) {
            void* pd_page = pgallocator->alloc();
            if (!pd_page) return -6;
            PageTableEntryUnion* pd_ptr = reinterpret_cast<PageTableEntryUnion*>(pd_page);
            for (int j = 0; j < 512; j++) pd_ptr[j].raw = 0;
            pdpte.raw = 0;
            pdpte.pdpte.present = 1;
            pdpte.pdpte.RWbit = 1;
            pdpte.pdpte.KERNELbit = 1;
            pdpte.pdpte.PD_addr = (uint64_t)pd_page >> 12;
        } else if (pdpte.pdpte.large) {
            return -7;
        }

        uint64_t pd_base = pdpte.pdpte.PD_addr << 12;
        PageTableEntryUnion* pd_ptr = reinterpret_cast<PageTableEntryUnion*>(pd_base);
        for (uint64_t j = 0; j < num_pages; j++) {
            PageTableEntryUnion& pde = pd_ptr[pde_index + j];
            pde.raw = 0;
            pde.pde2MB.present = 1;
            pde.pde2MB.RWbit = access.is_writeable;
            pde.pde2MB.KERNELbit = !access.is_kernel;
            pde.pde2MB.large = 1;
            pde.pde2MB._2mb_Addr = ((paddr + j * 0x200000) >> 21) & ((1ULL << 31) - 1);
            pde.pde2MB.global = access.is_global;
            pde.pde2MB.EXECUTE_DENY = !access.is_executable;
            pde.pde2MB.PWT = cache_table_idx.PWT;
            pde.pde2MB.PCD = cache_table_idx.PCD;
            pde.pde2MB.PAT = cache_table_idx.PAT;
        }
        return 0;
    };

    auto map_1gb = [&](uint64_t vaddr, uint64_t paddr, uint64_t num_pages) -> int {
        if (num_pages == 0 || num_pages > 512) return -4;
        uint16_t pml4_index = (vaddr >> 39) & 0x1FF;
        uint16_t pdpte_index = (vaddr >> 30) & 0x1FF;
        if (pdpte_index + num_pages > 512) return -5;

        PageTableEntryUnion* pml4_ptr = reinterpret_cast<PageTableEntryUnion*>(root_table);
        PageTableEntryUnion& pml4e = pml4_ptr[pml4_index];
        if (!pml4e.pml4.present) {
            void* pdpt_page = pgallocator->alloc();
            if (!pdpt_page) return -6;
            PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_page);
            for (int j = 0; j < 512; j++) pdpt_ptr[j].raw = 0;
            pml4e.raw = 0;
            pml4e.pml4.present = 1;
            pml4e.pml4.RWbit = 1;
            pml4e.pml4.KERNELbit = 1;
            pml4e.pml4.pdpte_addr = (uint64_t)pdpt_page >> 12;
        }

        uint64_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
        PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_base);
        for (uint64_t j = 0; j < num_pages; j++) {
            PageTableEntryUnion& pdpte = pdpt_ptr[pdpte_index + j];
            if (pdpte.raw & 1) return -7;
            pdpte.raw = 0;
            pdpte.pdpte1GB.present = 1;
            pdpte.pdpte1GB.RWbit = access.is_writeable;
            pdpte.pdpte1GB.KERNELbit = !access.is_kernel;
            pdpte.pdpte1GB.large = 1;
            pdpte.pdpte1GB._1GB_Addr = ((paddr + j * 0x40000000ULL) >> 30) & ((1ULL << 22) - 1);
            pdpte.pdpte1GB.global = access.is_global;
            pdpte.pdpte1GB.EXECUTE_DENY = !access.is_executable;
            pdpte.pdpte1GB.PWT = cache_table_idx.PWT;
            pdpte.pdpte1GB.PCD = cache_table_idx.PCD;
            pdpte.pdpte1GB.PAT = cache_table_idx.PAT;
        }
        return 0;
    };

    int rc = 0;
    switch (package.congruence_level) {
    case congruence_level_1gb: {
        for (int i = 0; i < 5; i++) {
            auto &entry = package.entryies[i];
            if (entry.num_of_pages == 0) continue;
            uint64_t psize = entry.page_size_in_byte;
            if (psize == 0) return -8;
            if ((entry.vbase % psize) != 0 || (entry.phybase % psize) != 0) return -2;
            switch (entry.page_size_in_byte) {
            case 0x40000000ULL: {
                uint64_t count_left = entry.num_of_pages;
                uint16_t pdpte_idx = (entry.vbase / 0x40000000ULL) & 0x1FF;
                uint64_t processed = 0;
                while (count_left > 0) {
                    uint64_t this_count = (count_left < (512 - pdpte_idx)) ? count_left : (512 - pdpte_idx);
                    uint64_t this_vbase = entry.vbase + processed * 0x40000000ULL;
                    uint64_t this_paddr = entry.phybase + processed * 0x40000000ULL;
                    rc = map_1gb(this_vbase, this_paddr, this_count);
                    if (rc != 0) return rc;
                    count_left -= this_count;
                    processed += this_count;
                    pdpte_idx = 0;
                }
                break;
            }
            case 0x200000:
                for(uint64_t j = 0; j < entry.num_of_pages; j++){
                    rc=map_2mb(entry.vbase + j * 0x200000, entry.phybase + j * 0x200000, 1);
                    if(rc!=0){
                        return rc;
                    }
                }
                break;
            case 0x1000:
                for(uint64_t j = 0; j < entry.num_of_pages; j++){
                    rc=map_4kb(entry.vbase + j * 0x1000, entry.phybase + j * 0x1000, 1);
                    if(rc!=0){
                        return rc;
                    }
                }
                break;
            default:
                return -8;
            }
        }
        break;
    }
    case congruence_level_2mb: {
        for (int i = 0; i < 5; i++) {
            auto &entry = package.entryies[i];
            if (entry.num_of_pages == 0) continue;
            uint64_t psize = entry.page_size_in_byte;
            if (psize == 0) return -8;
            if ((entry.vbase % psize) != 0 || (entry.phybase % psize) != 0) return -2;
            switch (entry.page_size_in_byte) {
            case 0x200000:
                for (uint64_t j = 0; j < entry.num_of_pages; j++) {
                    rc = map_2mb(entry.vbase + j * 0x200000, entry.phybase + j * 0x200000, 1);
                    if (rc != 0) return rc;
                }
                break;
            case 0x1000:
                for(uint64_t j = 0; j < entry.num_of_pages; j++){
                    rc=map_4kb(entry.vbase + j * 0x1000, entry.phybase + j * 0x1000, 1);
                    if (rc != 0) return rc;
                }
                break;
            default:
                return -8;
            }
        }
        break;
    }
    case congruence_level_4kb: {
        for (int i = 0; i < 5; i++) {
            auto &entry = package.entryies[i];
            if (entry.page_size_in_byte != 0x1000) continue;
            for (uint64_t j = 0; j < entry.num_of_pages; j++) {
                rc = map_4kb(entry.vbase + j * 0x1000, entry.phybase + j * 0x1000, 1);
                if (rc != 0) return rc;
            }
        }
        break;
    }
    default:
        return -8;
    }

    return 0;
}

/**
 * @brief 解除虚拟地址映射（仅支持x86_64 PGLV4）
 * 
 * 只清除 inter指定的叶子节点页表项，不回收页表本身。
 * 
 * unmapping 策略:
 * 1. 同样调用 split_vinterval_to_pages 拆分区间为 1GB/2MB/4KB
 * 2. 对每种大小的页面清除对应的叶子节点页表项
 * 3. 不清除中间级页表（PML4/PDPT/PD），也不回收物理内存
 * 
 * 假设条件:
 * - 运行在恒等映射环境，页表指针可直接解引用
 * - 只支持x86_64 PGLV4 架构
 * 
 * @param inter 待解除映射的虚拟地址区间
 * @return int 成功返回 0，失败返回负值错误码
 */
int kernel_mmu::unmap(vinterval inter)
{
    // 参数校验
    if (!root_table || !pgallocator) {
        return -1;
    }
    
    if (inter.vbase % 0x1000 != 0 || inter.phybase % 0x1000 != 0) {
        return -2; // 未对齐
    }
    
    if (inter.size % 0x1000 != 0) {
        return -3; // 大小不是 4KB 倍数
    }
    
    // 拆分区间
    seg_to_pages_info_pakage_t package;
    int status = split_vinterval_to_pages(package, inter);
    if (status != 0) {
        return status;
    }
    
    // 遍历所有拆分后的区段进行反映射
    for (int i = 0; i < 5; i++) {
        seg_to_pages_info_pakage_t::pages_info_t& entry = package.entryies[i];
        
        if (entry.page_size_in_byte == 0) {
            continue; // 跳过空条目
        }
        
        switch (entry.page_size_in_byte) {
            case 0x1000: {  // 4KB 小页
                uint64_t num_pages = entry.num_of_pages;
                if (num_pages == 0 || num_pages > 512) {
                    return -4; // 超出范围
                }
                
                uint64_t vaddr = entry.vbase;
                
                // 解析四级页表索引
                uint16_t pml4_index = (vaddr >> 39) & 0x1FF;
                uint16_t pdpte_index = (vaddr >> 30) & 0x1FF;
                uint16_t pde_index = (vaddr >> 21) & 0x1FF;
                uint16_t pte_index = (vaddr >> 12) & 0x1FF;
                
                if (pte_index + num_pages > 512) {
                    return -5; // 跨 PT 边界
                }
                
                // 获取 PML4 项
                PageTableEntryUnion* pml4_ptr = reinterpret_cast<PageTableEntryUnion*>(root_table);
                PageTableEntryUnion& pml4e = pml4_ptr[pml4_index];
                
                if (!pml4e.pml4.present) {
                    // PML4 项不存在，说明该区域未映射
                    return -6; // 未映射
                }
                
                // 获取 PDPT 基地址
                uint64_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
                PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_base);
                PageTableEntryUnion& pdpte = pdpt_ptr[pdpte_index];
                
                if (!pdpte.pdpte.present) {
                    return -6; // 未映射
                }
                
                // 获取 PD 基地址
                uint64_t pd_base = pdpte.pdpte.PD_addr << 12;
                PageTableEntryUnion* pd_ptr = reinterpret_cast<PageTableEntryUnion*>(pd_base);
                PageTableEntryUnion& pde = pd_ptr[pde_index];
                
                if (!pde.pde.present) {
                    return -6; // 未映射
                }
                
                // 获取 PT 基地址
                uint64_t pt_base = pde.pde.pt_addr << 12;
                PageTableEntryUnion* pt_ptr = reinterpret_cast<PageTableEntryUnion*>(pt_base);
                
                // 清除所有 PTE
                for (uint64_t j = 0; j < num_pages; j++) {
                    PageTableEntryUnion& pte = pt_ptr[pte_index + j];
                    
                    // 检查是否已映射
                    if (!pte.pte.present) {
                        return -6; // 未映射
                    }
                    
                    // 清除 PTE（清零）
                    pte.raw = 0;
                }
                break;
            }
            
            case 0x200000: {  // 2MB 大页
                uint64_t num_pages = entry.num_of_pages;
                if (num_pages == 0 || num_pages > 512) {
                    return -4; // 超出范围
                }
                
                uint64_t vaddr = entry.vbase;
                
                // 解析三级页表索引
                uint16_t pml4_index = (vaddr >> 39) & 0x1FF;
                uint16_t pdpte_index = (vaddr >> 30) & 0x1FF;
                uint16_t pde_index = (vaddr >> 21) & 0x1FF;
                
                if (pde_index + num_pages > 512) {
                    return -5; // 跨 PD 边界
                }
                
                // 获取 PML4 项
                PageTableEntryUnion* pml4_ptr = reinterpret_cast<PageTableEntryUnion*>(root_table);
                PageTableEntryUnion& pml4e = pml4_ptr[pml4_index];
                
                if (!pml4e.pml4.present) {
                    return -6; // 未映射
                }
                
                // 获取 PDPT 基地址
                uint64_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
                PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_base);
                PageTableEntryUnion& pdpte = pdpt_ptr[pdpte_index];
                
                if (!pdpte.pdpte.present) {
                    return -6; // 未映射
                }
                
                // 检查是否是 1GB 大页（冲突）
                if (pdpte.pdpte.large) {
                    return -7; // 页大小不匹配
                }
                
                // 获取 PD 基地址
                uint64_t pd_base = pdpte.pdpte.PD_addr << 12;
                PageTableEntryUnion* pd_ptr = reinterpret_cast<PageTableEntryUnion*>(pd_base);
                
                // 清除所有 PDE（2MB 大页）
                for (uint64_t j = 0; j < num_pages; j++) {
                    PageTableEntryUnion& pde = pd_ptr[pde_index + j];
                    
                    // 检查是否已映射且是 2MB 大页
                    if (!pde.pde2MB.present || !pde.pde2MB.large) {
                        return -7; // 页大小不匹配或未映射
                    }
                    
                    // 清除 PDE（清零）
                    pde.raw = 0;
                }
                break;
            }
            
            case 0x40000000ULL: {  // 1GB 大页
                uint64_t num_pages = entry.num_of_pages;
                
                // 1GB 页可能跨 PML4E 边界，需要循环处理
                uint64_t pdpte_equal_offset = entry.vbase / 0x40000000ULL;
                uint64_t count_to_assign_left = num_pages;
                uint16_t pdpte_idx = pdpte_equal_offset & 0x1FF;
                uint64_t processed_pages = 0;
                
                while (count_to_assign_left > 0) {
                    uint64_t this_count = (count_to_assign_left < (512 - pdpte_idx)) ? 
                                          count_to_assign_left : (512 - pdpte_idx);
                    
                    uint64_t this_vbase = entry.vbase + processed_pages * 0x40000000ULL;
                    
                    // 解析二级页表索引
                    uint16_t pml4_index = (this_vbase >> 39) & 0x1FF;
                    uint16_t pdpte_index = (this_vbase >> 30) & 0x1FF;
                    
                    // 获取 PML4 项
                    PageTableEntryUnion* pml4_ptr = reinterpret_cast<PageTableEntryUnion*>(root_table);
                    PageTableEntryUnion& pml4e = pml4_ptr[pml4_index];
                    
                    if (!pml4e.pml4.present) {
                        return -6; // 未映射
                    }
                    
                    // 获取 PDPT 基地址
                    uint64_t pdpt_base = pml4e.pml4.pdpte_addr << 12;
                    PageTableEntryUnion* pdpt_ptr = reinterpret_cast<PageTableEntryUnion*>(pdpt_base);
                    
                    // 清除所有 PDPTE（1GB 大页）
                    for (uint64_t j = 0; j < this_count; j++) {
                        PageTableEntryUnion& pdpte = pdpt_ptr[pdpte_index + j];
                        
                        // 检查是否已映射且是 1GB 大页
                        if (!pdpte.pdpte1GB.present || !pdpte.pdpte1GB.large) {
                            return -7; // 页大小不匹配或未映射
                        }
                        
                        // 清除 PDPTE（清零）
                        pdpte.raw = 0;
                    }
                    
                    count_to_assign_left -= this_count;
                    processed_pages += this_count;
                    pdpte_idx = 0;
                }
                break;
            }
            
            default:
                return -8; // 未知的页面大小
        }
    }
    
    return 0;
}

phyaddr_t kernel_mmu::get_root_table_base()
{
    return  (phyaddr_t)root_table;
}

mem_interval kernel_mmu::get_self_alloc_interval()
{
    return mem_interval{pgallocator->base,pgallocator->top-pgallocator->base };
}
