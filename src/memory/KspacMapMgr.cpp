#include "memory/AddresSpace.h"
#include "memory/memory_base.h"
#include "memory/all_pages_arr.h"
#include "abi/os_error_definitions.h"
#include "linker_symbols.h"
#include "util/OS_utils.h"
#include "util/kptrace.h"
#include "util/kout.h"
#include "core_hardwares/lapic.h"
#include "panic.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "firmware/ACPI_APIC.h"
spinlock_cpp_t kspace_pagetable_modify_lock;
int VM_vaddr_cmp(VM_DESC *a, VM_DESC *b)
{
    if(a->start < b->start&&a->end <= b->start)return -1;
    if(a->start > b->start&&a->start >= b->end)return 1;
    return 0;
}
// 定义KspaceMapMgr的静态成员变量
spinlock_cpp_t KspacePageTable::GMlock = spinlock_cpp_t();
phyaddr_t KspacePageTable::kspace_uppdpt_phyaddr = 0;
kspace_vm_table_t*kspace_vm_table;
namespace {
static inline KspacePageTableStatisitcs* kpt_stat_for_current_cpu()
{
    if (kspace_pagetable_statistics == nullptr || logical_processor_count == 0) {
        return nullptr;
    }
    uint32_t pid = fast_get_processor_id();
    if (pid >= logical_processor_count) {
        pid = pid % logical_processor_count;
    }
    return &kspace_pagetable_statistics[pid];
}
}

bool pglv_4_or_5=PAGE_TBALE_LV::LV_4;//true代表4级页表，false代表5级页表
cache_table_idx_struct_t cache_strategy_to_idx(cache_strategy_t cache_strategy)
{   
    uint8_t i=0;
    for(;i<8;i++){
        if(cache_strategy==DEFAULT_PAT_CONFIG.mapped_entry[i])break;
    }
    cache_table_idx_struct_t result={
        .PWT=(uint8_t)(i&1),
        .PCD=(uint8_t)((i>>1)&1),
        .PAT=(uint8_t)((i>>2)&1)
    };
    return result;
}
constexpr uint32_t _1GB_SIZE=0x40000000;
constexpr uint32_t _4KB_SIZE=0x1000;
constexpr uint32_t _2MB_SIZE=0x200000;
kspace_vm_table_t::kspace_vm_table_t() = default;

void kspace_vm_table_t::all_node_print()
{
    lock.lock();
    bsp_kout<< "[KSPACE_VM_TABLE] inorder traversal:" << kendl;
    auto inorder = [&](auto&& self, Node* n) -> void {
        if (!n) return;
        self(self, n->left);
        const VM_DESC& d = n->data;
        bsp_kout<< "  node: "<<HEX
                      << "start=0x" << d.start
                      << ", end=0x" << d.end
                      << ", phys=0x" << d.phys_start<<DEC
                      << ", map_type=" << static_cast<uint32_t>(d.map_type)
                      << ", access(k=" << static_cast<uint32_t>(d.access.is_kernel)
                      << ", w=" << static_cast<uint32_t>(d.access.is_writeable)
                      << ", r=" << static_cast<uint32_t>(d.access.is_readable)
                      << ", x=" << static_cast<uint32_t>(d.access.is_executable)
                      << ", g=" << static_cast<uint32_t>(d.access.is_global)
                      << ", cache=" << static_cast<uint32_t>(d.access.cache_strategy)
                      << ")"
                      << ", color=" << (n->is_red ? "R" : "B")
                      << kendl;
        self(self, n->right);
    };
    inorder(inorder, root);
    lock.unlock();
}

namespace {
    using KspaceNode = kspace_vm_table_t::Node;

    KspaceNode* subtree_min(KspaceNode* node)
    {
        KspaceNode* cur = node;
        while (cur && cur->left) cur = cur->left;
        return cur;
    }

    KspaceNode* subtree_max(KspaceNode* node)
    {
        KspaceNode* cur = node;
        while (cur && cur->right) cur = cur->right;
        return cur;
    }

    KspaceNode* successor(KspaceNode* node)
    {
        if (!node) return nullptr;
        if (node->right) {
            KspaceNode* cur = node->right;
            while (cur->left) cur = cur->left;
            return cur;
        }
        KspaceNode* p = node->parent;
        while (p && node == p->right) {
            node = p;
            p = p->parent;
        }
        return p;
    }
}

kspace_vm_table_t::Node* kspace_vm_table_t::search(vaddr_t vaddr) {
    lock.lock();
    Node* cur = root;
    while (cur) {
        if (vaddr < cur->data.start) {
            cur = cur->left;
        } else if (vaddr >= cur->data.end) {
            cur = cur->right;
        } else {
            lock.unlock();
            return cur;
        }
    }
    lock.unlock();
    return nullptr;
}

int kspace_vm_table_t::insert(VM_DESC data) {
    lock.lock();
    bool ret = Ktemplats::RBTree<VM_DESC,VM_desc_cmp>::insert(data);
    lock.unlock();
    return ret?OS_SUCCESS:OS_MEMRY_ALLOCATE_FALT;
}

int kspace_vm_table_t::remove(vaddr_t vaddr) {
    lock.lock();
    Node* cur = root;
    while (cur) {
        if (vaddr < cur->data.start) {
            cur = cur->left;
        } else if (vaddr >= cur->data.end) {
            cur = cur->right;
        } else {
            break;
        }
    }
    if (!cur) {
        lock.unlock();
        return -1;
    }
    int ret = Ktemplats::RBTree<VM_DESC,VM_desc_cmp>::erase(cur->data) ? 0 : -1;
    lock.unlock();
    return ret;
}
KURD_t KspacePageTable::enable_VMentry(const vm_interval& interval)
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->enable_vmentry_count++;
    }
    KURD_t success = default_success();
    KURD_t fail = default_failure();
    KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_ENABLE_VMENTRY;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_ENABLE_VMENTRY;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_ENABLE_VMENTRY;
    
    // basic alignment checks (4KB)
    if (interval.vbase % _4KB_SIZE || interval.size % _4KB_SIZE || interval.pbase % _4KB_SIZE) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
        return fail;
    }

    if (interval.size == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
        return fail;
    }
    GMlock.lock();
    // 1) Split segment into page-sized runs
    VM_DESC vmentry = {
        .start = interval.vbase,
        .end = interval.vbase + interval.size,
        .map_type = VM_DESC::MAP_PHYSICAL,
        .phys_start = interval.pbase,
        .access = interval.access,
        .committed_full = true,
        .is_vaddr_alloced = false,
        .is_out_bound_protective = false,
    };
    seg_to_pages_info_pakage_t pkg;
    vm_interval_to_pages_info(pkg, vmentry);
    // Helper lambda: given page size (p), call the appropriate setter in chunks
    KURD_t rc=KURD_t();
    // 2) Iterate over pkg.entryies and dispatch
    switch(pkg.congruence_level)
    case congruence_level_1gb:{
    for (int i = 0; i < 5; ++i) {
        auto &e = pkg.entryies[i];
        if (e.num_of_pages == 0) continue;

        uint64_t psize = e.page_size_in_byte;
        // sanity check: vbase and base should be aligned to page size
        if ((e.vbase % psize) != 0 || (e.phybase % psize) != 0) {
            fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
            GMlock.unlock();
            return fail;
        }

        switch(e.page_size_in_byte) {
            case _1GB_SIZE: {
                uint64_t count = e.num_of_pages;
                rc = _4lv_pdpte_1GB_entries_set(e.phybase, e.vbase, count, interval.access);
                break;
            }
            case _2MB_SIZE: {
                uint16_t count = static_cast<uint16_t>(e.num_of_pages);
                rc = _4lv_pde_2MB_entries_set(e.phybase, e.vbase, count, interval.access);
                break;
            }
            case _4KB_SIZE: {
                uint16_t count = static_cast<uint16_t>(e.num_of_pages);
                rc = _4lv_pte_4KB_entries_set(e.phybase, e.vbase, count, interval.access);
                break;
            }
            default:
                fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::ENABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALIDE_PAGES_SIZE;
                GMlock.unlock();
                return fatal; // unknown page size
        }
        if (rc.result != result_code::SUCCESS) {
            GMlock.unlock();
            return rc;
        }
    }
    break;
    case congruence_level_2mb:{ 
        for (int i = 0; i < 5; ++i) {
        auto &e = pkg.entryies[i];
        if (e.num_of_pages == 0) continue;

        uint64_t psize = e.page_size_in_byte;
        // sanity check: vbase and base should be aligned to page size
        if ((e.vbase % psize) != 0 || (e.phybase % psize) != 0) {
            fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::ENABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
            GMlock.unlock();
            return fail;
        }

        switch(e.page_size_in_byte) {
            case _2MB_SIZE: {
                uint64_t count = e.num_of_pages;
                for(uint64_t j=0;j<count;j++){
                    rc = _4lv_pde_2MB_entries_set(e.phybase+j*_2MB_SIZE, e.vbase+j*_2MB_SIZE, 1, interval.access);
                    if(rc.result!=result_code::SUCCESS){

                    }
                }break;
            }
            case _4KB_SIZE: {
                uint16_t count = static_cast<uint16_t>(e.num_of_pages);
                rc = _4lv_pte_4KB_entries_set(e.phybase, e.vbase, count, interval.access);
                break;
            }
            default:
                fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::ENABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALIDE_PAGES_SIZE;
                GMlock.unlock();
                return fatal; // unknown page size
        }
        if (rc.result != result_code::SUCCESS) {
            GMlock.unlock();
            return rc;
        }
    }
        break;
    }
    case congruence_level_4kb:{
        for(int i = 0; i < 5; ++i){
            auto &e = pkg.entryies[i];
            if(e.page_size_in_byte!=_4KB_SIZE){

            }else{
                for(uint64_t j=0;j<e.num_of_pages;j++){
                    rc = _4lv_pte_4KB_entries_set(e.phybase+j*_4KB_SIZE, e.vbase+j*_4KB_SIZE, 1, interval.access);
                }
                if(rc.result!=result_code::SUCCESS){
                    GMlock.unlock();
                    return rc;
                }
            }
        }
        break;
    }
    }

    // Optionally mark vmentry as enabled (if VM_DESC has such a field)
    // vmentry.enabled = true;  // uncomment if VM_DESC supports it
    GMlock.unlock();
    return success;
}



KURD_t KspacePageTable::disable_VMentry(const vm_interval& interval)
{
   KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
   if (stat) {
       stat->disable_vmentry_count++;
   }
   KURD_t success = default_success();
   KURD_t fail = default_failure();
   KURD_t fatal = default_fatal();
    success.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_DISABLE_VMENTRY;
    fail.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_DISABLE_VMENTRY;
    fatal.event_code = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::EVENT_CODE_DISABLE_VMENTRY;

    // basic alignment checks (4KB)
   if (interval.vbase % _4KB_SIZE || interval.size % _4KB_SIZE || interval.pbase % _4KB_SIZE) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
       return fail;
    }

   if (interval.size == 0) {
        fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
       return fail;
    }
    spinlock_interrupt_about_guard guard(GMlock);
    // 1) Split segment into page-sized runs
    seg_to_pages_info_pakage_t pkg;
    VM_DESC vmentry = {
        .start = interval.vbase,
        .end = interval.vbase + interval.size,
        .map_type = VM_DESC::MAP_PHYSICAL,
        .phys_start = interval.pbase,
        .access = interval.access,
        .committed_full = true,
        .is_vaddr_alloced = false,
        .is_out_bound_protective = false,
    };
   vm_interval_to_pages_info(pkg, vmentry);
    
    // 2) Iterate over pkg.entryies and dispatch based on congruence level
   KURD_t rc=KURD_t();
    switch(pkg.congruence_level) {
        case congruence_level_1gb: {
            for (int i = 0; i < 5; ++i) {
                auto &e = pkg.entryies[i];
               if (e.num_of_pages == 0) continue;

               uint64_t psize = e.page_size_in_byte;
                // sanity check: vbase and base should be aligned to page size
               if ((e.vbase % psize) != 0 || (e.phybase % psize) != 0) {
                    fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
                   return fail;
                }

                switch(e.page_size_in_byte) {
                    case _1GB_SIZE: {
                       uint64_t count = e.num_of_pages;
                        rc = _4lv_pdpte_1GB_entries_clear(e.vbase, static_cast<uint16_t>(count));
                        break;
                    }
                    case _2MB_SIZE: {
                       uint16_t count = static_cast<uint16_t>(e.num_of_pages);
                        rc = _4lv_pde_2MB_entries_clear(e.vbase, count);
                        break;
                    }
                    case_4KB_SIZE: {
                       uint16_t count = static_cast<uint16_t>(e.num_of_pages);
                        rc = _4lv_pte_4KB_entries_clear(e.vbase, count);
                        break;
                    }
                    default:
                        fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALIDE_PAGES_SIZE;
                       return fatal;
                }
               if (rc.result != result_code::SUCCESS) {
                   return rc;
                }
            }
            break;
        }
        case congruence_level_2mb: {
            for (int i = 0; i < 5; ++i) {
                auto &e = pkg.entryies[i];
               if (e.num_of_pages == 0) continue;

               uint64_t psize = e.page_size_in_byte;
                // sanity check: vbase and base should be aligned to page size
               if ((e.vbase % psize) != 0 || (e.phybase % psize) != 0) {
                    fail.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FAIL_REASONS::REASON_CODE_BAD_VMENTRY;
                   return fail;
                }

                switch(e.page_size_in_byte) {
                    case _2MB_SIZE: {
                       uint64_t count = e.num_of_pages;
                        for(uint64_t j=0; j<count; j++){
                            rc = _4lv_pde_2MB_entries_clear(e.vbase+j*_2MB_SIZE, 1);
                           if(rc.result != result_code::SUCCESS){
                               return rc;
                            }
                        }
                        break;
                    }
                    case _4KB_SIZE: {
                       uint16_t count = static_cast<uint16_t>(e.num_of_pages);
                        rc = _4lv_pte_4KB_entries_clear(e.vbase, count);
                        break;
                    }
                    default:
                        fatal.reason = MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALIDE_PAGES_SIZE;
                       return fatal;
                }
               if (rc.result != result_code::SUCCESS) {
                   return rc;
                }
            }
            break;
        }
        case congruence_level_4kb: {
            for(int i = 0; i < 5; ++i){
                auto &e = pkg.entryies[i];
               if(e.page_size_in_byte != _4KB_SIZE){
                    continue;
                } else {
                    for(uint64_t j=0; j<e.num_of_pages; j++){
                        rc = _4lv_pte_4KB_entries_clear(e.vbase+j*_4KB_SIZE, 1);
                       if(rc.result != result_code::SUCCESS){
                           return rc;
                        }
                    }
                }
            }
            break;
        }
        default:
            //fatal.reason= MEMMODULE_LOCAIONS::KSPACE_MAPPER_EVENTS::DISABLE_VMENTRY_RESULTS::FATAL_REASONS::REASON_CODE_INVALID_CONGRUENCE_LEVEL;
           return fatal;
    }
    shared_inval_kspace_VMentry_info.info_package = pkg;
    shared_inval_kspace_VMentry_info.is_package_valid = true;
    shared_inval_kspace_VMentry_info.completed_processors_count.store(0);
    if(GlobalKernelStatus>=SCHEDUL_READY){
        x2apic::x2apic_driver::broadcast_exself_fixed_ipi(invalidate_seg);
        //这里要做自旋等待，若1000mius每完成认为失败要panic
    }

   return success;
}
KURD_t KspacePageTable::v_to_phyaddrtraslation(vaddr_t vaddr, phyaddr_t &result)
{
    KspacePageTableStatisitcs* stat = kpt_stat_for_current_cpu();
    if (stat) {
        stat->tran_to_phy_entry_count++;
    }
    PageTableEntryUnion badentry={0};
    PageTableEntryUnion& result_entry=badentry;
    uint32_t page_size;
    KURD_t status = v_to_phyaddrtraslation_entry(vaddr, result_entry, page_size);
    if(status.result != result_code::SUCCESS)return status;
    switch (page_size)
    {
        case _4KB_SIZE:
            {
                result=result_entry.pte.page_addr<<12;
                return default_success();
            }
        case _2MB_SIZE:
            {
                result=result_entry.pde2MB._2mb_Addr<<21;
                return default_success();
            }
        case _1GB_SIZE:
            {
                result=result_entry.pdpte1GB._1GB_Addr<<30;
                return default_success();
            }
    }
}
/**
 * @brief 虚拟地址比较函数
 * 如果地址段有重复，则返回0,
 * a.start < b.start, 且 a.end <= b.start, 返回负值
 * a.start > b.start, 且 a.start >= b.end, 返回正值
 */

/**
 * 这个接口有大问题，至少需要传入一个参数表示原有物理地址余1GB的偏移，虚拟地址必须与这个偏移对应
 *选取起始虚拟地址也有讲究，不能一刀切必须与物理地址1gb同余，这样要造成虚拟地址浪费
 *根据穿来的物理地址便宜和大小判断应该采取分配虚拟地址策略，如果对应物理段合并了2mb大页，那么虚拟地址，物理地址要2mb同余
*如果对应物理段合并了1gb大页，那么虚拟地址，物理地址要1gb同余
    *没有合并则最简单
 */
/**
 * 所以需要把起始虚拟地址向上调整操作，鉴定物理段使用的大页操作这些拿出来做成函数
 */
vaddr_t kspace_vm_table_t::alloc_available_space(uint64_t size,uint32_t target_vaddroffset)
{
    lock.lock();
    if(size==0||size%_4KB_SIZE||target_vaddroffset>=_1GB_SIZE||target_vaddroffset%_4KB_SIZE){
        lock.unlock();
        return 0;
    }
    auto search_no_lock = [&](vaddr_t vaddr) -> Node* {
        Node* cur = root;
        while (cur) {
            if (vaddr < cur->data.start) {
                cur = cur->left;
            } else if (vaddr >= cur->data.end) {
                cur = cur->right;
            } else {
                return cur;
            }
        }
        return nullptr;
    };
    enum PHY_SEG_MAX_PAGE:uint8_t{
        PHY_SEG_MAX_PAGE_4KB=0,
        PHY_SEG_MAX_PAGE_2MB,
        PHY_SEG_MAX_PAGE_1GB
    };
    auto max_type_identifier=[size,target_vaddroffset]()->PHY_SEG_MAX_PAGE{
        phyaddr_t phybase=target_vaddroffset;
        phyaddr_t phyend=phybase+size;

       uint64_t base_1GB=align_up(phybase,_1GB_SIZE);
       uint64_t end_1GB=align_down(phyend,_1GB_SIZE);
        if(base_1GB<end_1GB){
            return PHY_SEG_MAX_PAGE_1GB;
        }
       uint64_t base_2MB=align_up(phybase,_2MB_SIZE);
       uint64_t end_2MB=align_down(phyend,_2MB_SIZE);
        if(base_2MB<end_2MB){
            return PHY_SEG_MAX_PAGE_2MB;
        }
        return PHY_SEG_MAX_PAGE_4KB;
    };
    PHY_SEG_MAX_PAGE max_page_type=max_type_identifier();
    auto base_addr_modifier=[max_page_type,target_vaddroffset](vaddr_t base)->vaddr_t{ 
        switch (max_page_type)
        {
            case PHY_SEG_MAX_PAGE_4KB:
                return base;
            case PHY_SEG_MAX_PAGE_2MB:
                {
                   uint32_t lower_offset=base%_2MB_SIZE;
                   uint64_t basebase=base-lower_offset;
                   uint32_t upper_offset=target_vaddroffset%_2MB_SIZE;
                    if(upper_offset<lower_offset)return basebase+upper_offset+_2MB_SIZE;
                    else return basebase+upper_offset;
                }
            case PHY_SEG_MAX_PAGE_1GB:
                {
                   uint32_t lower_offset=base%_1GB_SIZE;
                   uint64_t basebase=base-lower_offset;
                   uint32_t upper_offset=target_vaddroffset%_1GB_SIZE;
                    if(upper_offset<lower_offset)return basebase+upper_offset+_1GB_SIZE;
                    else return basebase+upper_offset;
                }
            default:
                    return 0;
        }
    };
    
    // 优化：优先尝试从上次分配的结尾位置开始分配（缓存命中）
    if(this->last_alloc_end!=0){
       vaddr_t cache_start=base_addr_modifier(this->last_alloc_end);
        // 检查缓存位置是否可用（不与任何已分配区间重叠）
        Node*node=search_no_lock(cache_start);
        if(node==nullptr){  // 缓存位置未被占用
            // 验证从缓存位置开始的连续空间是否足够
          vaddr_t test_end=cache_start+size;
            Node*test_node=search_no_lock(test_end-1);
            if(test_node==nullptr){  // 整个区间都未被占用
                this->last_alloc_end=test_end;  // 更新缓存
                lock.unlock();
                return cache_start;
            }
        }
    }
    
    // 缓存未命中或空间不足，尝试从最高地址之后分配
    kspace_vm_table_t::Node* maxnode=subtree_max(this->root);
    if(maxnode!=nullptr){
        VM_DESC*maxentry=&maxnode->data;
      vaddr_t upper_alloc_base=base_addr_modifier(maxentry->end);
      uint64_t upper_top_avaliable=~upper_alloc_base+1;
        if(upper_top_avaliable>size){
            this->last_alloc_end=upper_alloc_base+size;  // 更新缓存
            lock.unlock();
            return upper_alloc_base;
        }
    }
    
    // 在红黑树的空隙中查找可用空间
    kspace_vm_table_t::Node* minnode=subtree_min(this->root);
    if(minnode==nullptr){  // 树为空，从头开始
       vaddr_t first_alloc=base_addr_modifier(0);
        this->last_alloc_end=first_alloc+size;  // 更新缓存
        lock.unlock();
        return first_alloc;
    }
    
    kspace_vm_table_t::Node* backnode=minnode;
    kspace_vm_table_t::Node* front_node=successor(minnode);
    do{
      vaddr_t gap_start=base_addr_modifier(backnode->data.end);
      vaddr_t gap_end=front_node->data.start;
        if(gap_end>gap_start&&(gap_end-gap_start)>=size){
            this->last_alloc_end=gap_start+size;  // 更新缓存
            lock.unlock();
            return gap_start;
        }
        backnode=front_node;
        front_node=successor(front_node);
    }while(front_node!=maxnode);
    
    lock.unlock();
    return 0;  // 未找到足够的连续空间
}
