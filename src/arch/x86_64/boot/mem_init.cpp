#include "memory/memory_base.h"
#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/init_memory_info.h"
#include "memory/AddresSpace.h"
#include "memory/phyaddr_accessor.h"
#include "arch/x86_64/mem_init.h"
#include "util/kout.h"
#include "elf.h"
#include "panic.h"
uint64_t VM_intervals_count;
phymem_segment *phymem_segments;
uint64_t phymem_segments_count; 
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
vm_interval hpet_mmio;
vm_interval gop_buffer;
phymem_segment legacy_mmu_interval;
KURD_t pesisitent_properties_set(){
    KURD_t bsp_init_kurd;
    bsp_init_kurd= all_pages_arr::simp_pages_set(kIMG_self_window.pbase,alignup_and_shift_right(kIMG_self_window.size,12),page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    bsp_init_kurd=all_pages_arr::simp_pages_set(kBSS_interval.pbase,alignup_and_shift_right(kBSS_interval.size,12),page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    bsp_init_kurd=all_pages_arr::simp_pages_set(pages_arr.pbase,alignup_and_shift_right(pages_arr.size,12),page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
    bsp_init_kurd=all_pages_arr::simp_pages_set(FPA_bitmaps.pbase,alignup_and_shift_right(FPA_bitmaps.size,12),page_state_t::kernel_persisit);
    if(error_kurd(bsp_init_kurd))return bsp_init_kurd;
}
void fpa_properties_deal(){
    phymem_segment initramfs;
    initramfs.start=initramfs_file.pbase;
    initramfs.size=initramfs_file.size;
    phymem_segment symboltable;
    symboltable.start=symtable_file.pbase;
    symboltable.size=symtable_file.size;
    phymem_segment log_interval;
    log_interval.start=log_buffer.pbase;
    log_interval.size=log_buffer.size;
    phymem_segment entry_kernel_stack;
    entry_kernel_stack.start=kernel_entry_stack.pbase;
    entry_kernel_stack.size=kernel_entry_stack.size;
    FreePagesAllocator::interval_pollute(legacy_mmu_interval);
    FreePagesAllocator::interval_pollute(symboltable);
    FreePagesAllocator::interval_pollute(initramfs);
    FreePagesAllocator::interval_pollute(log_interval);
    FreePagesAllocator::interval_pollute(entry_kernel_stack);
    FreePagesAllocator::activate();
}
KURD_t KImage_map_rebuild(){
    KURD_t success(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        0, level_code::INFO, err_domain::CORE_MODULE
    );

    // 1. 通过旧页表的 kIMG_self_window 读取 ELF（CR3 尚未切换）
    uint8_t* elf_base = (uint8_t*)kIMG_self_window.vbase;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        bsp_kout << "[KImage_map_rebuild] bad ELF magic" << kendl;
        KURD_t fail(
            result_code::FATAL, 0, module_code::MEMORY,
            MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
            0, level_code::FATAL, err_domain::CORE_MODULE
        );
        return fail;
    }

    uint8_t* ptbl = elf_base + ehdr->e_phoff;

    // 2. 遍历 PT_LOAD，在 KspacePageTable（新 PML4）中重建映射
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        // BSS segment：使用已就绪的 kBSS_interval
        if (ph->p_filesz == 0 && ph->p_memsz > 0) {
            KURD_t kurd = KspacePageTable::enable_VMentry(kBSS_interval);
            if (error_kurd(kurd)) {
                bsp_kout << "[KImage_map_rebuild] BSS map fail" << kendl;
                return kurd;
            }
            continue;
        }

        // 根据 ELF program header 权限构造页表访问属性
        pgaccess acc;
        acc.is_kernel      = 1;
        acc.is_writeable   = (ph->p_flags & PF_W) ? 1 : 0;
        acc.is_readable    = 1;
        acc.is_executable  = (ph->p_flags & PF_X) ? 1 : 0;
        acc.is_global      = 1;
        acc.cache_strategy = WB;

        // 非 BSS 段：物理地址 = 文件镜像基址 + 段文件偏移，VA 为 ELF 固定地址
        phyaddr_t pa = kIMG_self_window.pbase + ph->p_offset;
        vaddr_t   va = ph->p_vaddr;
        uint64_t  sz = ph->p_memsz;

        vm_interval seg_iv = {va, pa, sz, acc};
        KURD_t kurd = KspacePageTable::enable_VMentry(seg_iv);
        if (error_kurd(kurd)) {
            bsp_kout << "[KImage_map_rebuild] seg " << i << " map fail" << kendl;
            return kurd;
        }
    }

    // 3. 直接复刻 kIMG_self_window 到新 PML4（固定 VA 模式）
    //    phyaddr_direct_map 校验 vbase 为合法内核地址后，在相同 VA 重建映射
    {
        vm_interval window = kIMG_self_window;
        KURD_t map_kurd;
        vaddr_t ret = phyaddr_direct_map(&window, &map_kurd);
        if (ret == 0) {
            bsp_kout << "[KImage_map_rebuild] self-window remap fail" << kendl;
            return map_kurd;
        }
        // window.vbase 不变（与原 kIMG_self_window.vbase 同），新 PML4 下相同 VA 可用
        kIMG_self_window = window;
    }

    bsp_kout.shift_hex();
    bsp_kout << "[KImage_map_rebuild] done: kIMG 0x" << kIMG_self_window.pbase
             << " ->0x" << kIMG_self_window.vbase << kendl;
    bsp_kout.shift_dec();

    return success;
}
// 全量 TLB 刷出：reload CR3 使所有非全局缓存项失效
// 在 phyaddr_direct_map 后必须立即调用，防止 old identity-map TLB 残留
static void tlb_full_flush() {
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    asm volatile("mov %0, %%cr3" :: "r"(cr3_val) : "memory");
}
KURD_t kimg_affiliate_property_map1(){
    struct aff_entry {
        vm_interval* iv;
        const char* name;
    };
    aff_entry list[] = {
        {&FPA_bitmaps,         "FPA_bitmaps"},
        {&pages_arr,           "pages_arr"},
        {&log_buffer,          "log_buffer"},
        {&kernel_entry_stack,  "kernel_entry_stack"},
        {&symtable_file,       "symtable_file"},
        {&initramfs_file,      "initramfs_file"},
        {&hpet_mmio,           "hpet_mmio"},
        {&gop_buffer,          "gop_buffer"},
    };

    for (auto& e : list) {
        if (e.iv->size == 0 || (e.iv->pbase & 0xFFF)) {
            bsp_kout << "[kimg_affiliate_property_map1] skip " << e.name
                     << " (size=0 or misaligned)" << kendl;
            continue;
        }
        vm_interval copy = *e.iv;
        KURD_t map_kurd;
        vaddr_t ret = phyaddr_direct_map(&copy, &map_kurd);
        if (ret == 0 || error_kurd(map_kurd)) {
            bsp_kout << "[kimg_affiliate_property_map1] " << e.name
                     << " map fail" << kendl;
            return map_kurd;
        }
        *e.iv = copy;
        bsp_kout << "[kimg_affiliate_property_map1] " << e.name
                 << " 0x" << HEX << e.iv->pbase << " ->0x" << e.iv->vbase << kendl;
        bsp_kout.shift_dec();
    }
    KURD_t ok(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        0, level_code::INFO, err_domain::CORE_MODULE
    );
    return ok;
}
KURD_t properties_modify_stage1(){
    phymem_segment initramfs;
    initramfs.start=initramfs_file.pbase;
    initramfs.size=initramfs_file.size;
    KURD_t kurd;
    initramfs_file.pbase=FreePagesAllocator::alloc(
        initramfs_file.size,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_anonymous,kurd
    );
    if(error_kurd(kurd)){
        bsp_kout<<"initramfs_file alloc Failed"<<kendl;
        return kurd;
    }
    PhyAddrAccessor::paddr_memcpy(initramfs_file.pbase,initramfs.start,initramfs.size);
    uint64_t vbase=phyaddr_direct_map(&initramfs_file,&kurd);
    tlb_full_flush();
    if(vbase==0||error_kurd(kurd)){
        bsp_kout<<"initramfs_file phyaddr_direct_map Failed"<<kendl;
        return kurd;
    }
    FreePagesAllocator::interval_clean(initramfs);
    
    // --- symboltable ---
    phymem_segment symboltable;
    symboltable.start=symtable_file.pbase;
    symboltable.size=symtable_file.size;
    symtable_file.pbase=FreePagesAllocator::alloc(
        symtable_file.size,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_anonymous,kurd
    );
    if(error_kurd(kurd)){
        bsp_kout<<"symtable_file alloc Failed"<<kendl;
        return kurd;
    }
    PhyAddrAccessor::paddr_memcpy(symtable_file.pbase,symboltable.start,symboltable.size);
    vbase=phyaddr_direct_map(&symtable_file,&kurd);
    tlb_full_flush();
    if(vbase==0||error_kurd(kurd)){
        bsp_kout<<"symtable_file phyaddr_direct_map Failed"<<kendl;
        return kurd;
    }
    FreePagesAllocator::interval_clean(symboltable);

    // --- log_interval ---
    phymem_segment log_interval;
    log_interval.start=log_buffer.pbase;
    log_interval.size=log_buffer.size;
    log_buffer.pbase=FreePagesAllocator::alloc(
        log_buffer.size,BUDDY_ALLOC_DEFAULT_FLAG,page_state_t::kernel_anonymous,kurd
    );
    if(error_kurd(kurd)){
        bsp_kout<<"log_buffer alloc Failed"<<kendl;
        return kurd;
    }
    PhyAddrAccessor::paddr_memcpy(log_buffer.pbase,log_interval.start,log_interval.size);
    vbase=phyaddr_direct_map(&log_buffer,&kurd);
    tlb_full_flush();
    if(vbase==0||error_kurd(kurd)){
        bsp_kout<<"log_buffer phyaddr_direct_map Failed"<<kendl;
        return kurd;
    }
    FreePagesAllocator::interval_clean(log_interval);
    FreePagesAllocator::interval_clean(legacy_mmu_interval);
    KURD_t ok(
        result_code::SUCCESS, 0, module_code::MEMORY,
        MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR,
        0, level_code::INFO, err_domain::CORE_MODULE
    );
    return ok;
}

KURD_t mem_init(){ 
    KURD_t bsp_init_kurd;
    PhyAddrAccessor::Init(identity_map_window);
    bsp_init_kurd=all_pages_arr::Init(&pages_arr);
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"phymemspace_mgr Init Failed"<<kendl;
        return bsp_init_kurd;
    }
     bsp_init_kurd=pesisitent_properties_set();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"pesisitent_properties_set Failed"<<kendl;
        return bsp_init_kurd;
    }
    bsp_init_kurd=FreePagesAllocator::Init(FreePagesAllocator::BEST_FIT,&FPA_bitmaps);//传入一个loaded_VM_entry
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"FreePagesAllocator Init Failed"<<kendl;
        return bsp_init_kurd;
    }
    fpa_properties_deal();
    if(error_kurd(bsp_init_kurd)){
        return bsp_init_kurd;
    }
    bsp_init_kurd=KspacePageTable::Init();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"KspaceMapMgr Init Failed"<<kendl;
        return bsp_init_kurd;
    }
    bsp_init_kurd=KImage_map_rebuild();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"KImage_map_rebuild Failed"<<kendl;
        return bsp_init_kurd;
    }
    bsp_init_kurd=kimg_affiliate_property_map1();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"kimg_affiliate_property_map1 Failed"<<kendl;
        return bsp_init_kurd;
    }
    gKernelSpace=new AddressSpace();
    bsp_init_kurd=gKernelSpace->second_stage_init();//传入trasfer,特殊重载，要接手相关内存
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"identity map fail"<<kendl;
        return bsp_init_kurd;
    }
    gKernelSpace->unsafe_load_pml4_to_cr3(KERNEL_SPACE_PCID);
    properties_modify_stage1();
    bsp_init_kurd=kpoolmemmgr_t::multi_heap_enable();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"Kpoolmemmgr_t::multi_heap_enable Failed"<<kendl;
    }
    GlobalKernelStatus=kernel_state::MM_READY;
}