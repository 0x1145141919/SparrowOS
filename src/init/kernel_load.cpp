#include "abi/boot.h"
#include "../init/include/heap_alloc.h"
#include "../init/include/load_kernel.h"
#include "../init/include/pages_alloc.h"
#include "../init/include/util/kout.h"
#include "../init/include/panic.h"
#include "memory/page_struct.h"
#include <elf.h>



/**
 * @brief ELF program header param 字段的位域定义和掩码
 * 
 * kld.ld 中通过 param 字段传递加载参数，格式如下:
 * - bit[0]:    x - 可执行权限
 * - bit[1]:    w - 可写权限
 * - bit[2]:    r - 可读权限
 * - bit[3-7]:  reserved
 * - bit[8]:    pa_random - PA随机化标志（需要从 basic_allocator分配）
 * - bit[9]:    va_random - VA随机化标志（从高地址向下单调分配）
 * - bit[10-31]: reserved2
 */
constexpr uint32_t PHDR_PARAM_X_MASK          = 0x1;        ///< bit[0] 可执行权限掩码
constexpr uint32_t PHDR_PARAM_W_MASK          = 0x2;        ///< bit[1] 可写权限掩码
constexpr uint32_t PHDR_PARAM_R_MASK          = 0x4;        ///< bit[2] 可读权限掩码
constexpr uint32_t PHDR_PARAM_PA_RANDOM_MASK  = 0x100;      ///< bit[8] PA随机化掩码
constexpr uint32_t PHDR_PARAM_VA_RANDOM_MASK  = 0x200;      ///< bit[9] VA随机化掩码

/**
 * @brief 内核加载参数解析结构
 * 
 * 用于解析 ELF program header 的 param 字段，控制加载行为
 */
struct phdr_kernel_specify_param{
    uint32_t x:1;           ///< 可执行权限
    uint32_t w:1;           ///< 可写权限
    uint32_t r:1;           ///< 可读权限
    uint32_t reserved:5;    ///< 保留位
    uint32_t pa_random:1;   ///< 物理地址随机化（需符合 p_align）
    uint32_t va_random:1;   ///< 虚拟地址随机化（需符合 p_align）
    uint32_t reserved2:22;  ///< 保留位
};

/**
 * @brief VA 单调递减分配器状态
 * 
 * 用于后续匿名内存映射时的虚拟地址分配，从高地址开始向下单调分配
 * 注意：kernel_load 函数中内核段的加载不使用此机制，严格按照 ELF 声明的 VA 加载
 */
constexpr uint64_t g_va_alloc_top = 0xFFFFFFFFFFFFF000ULL;  // 初始 VA 分配起点（高半空间顶部）
uint64_t current_top = g_va_alloc_top;
uint64_t va_alloc(uint64_t size,uint8_t align_log2){
    if (align_log2 < 12) {
        align_log2 = 12;
    }
    uint64_t alignment = 1ULL << align_log2;
    size = align_up(size, alignment);
    current_top = align_down(current_top - size, alignment);
    return current_top;
}
/**
 * @brief VM_interval_specifyid 分配系数
 * 
 * 为每个 PT_LOAD 段预留多个 VM_interval 槽位，以支持内核的其他映射需求
 * 例如：代码段、数据段、BSS 段可能需要独立的映射描述符
 */
constexpr uint64_t VM_INTERVAL_ALLOC_FACTOR = 3;  // 每段预留 3 个槽位


/**
 * @brief 内核加载函数
 * 
 * 解析 ELF 格式的内核文件，将 PT_LOAD 类型的段加载到内存并建立映射。
 * 
 * 输入要求:
 * - kmmu: 必须有效的 kernel_mmu 指针
 * - kernel_file_entry: 必须包含有效的 raw_data 指针和 file_size
 * 
 * 输出结果:
 * - VM_entry_count: 成功加载的段数量
 * - VM_entries: 在堆上分配的 VM 区间数组，描述每个加载段的映射信息
 * 
 * 加载策略:
 * - 只处理 p_type == PT_LOAD 的 program header
 * - 根据 phdr_kernel_specify_param 的位域设置访问权限
 * - 使用恒等映射（虚拟地址 = 物理地址）
 * 
 * @param pak 内核加载信息包（引用传递）
 * @return int 成功返回 0，失败返回负值错误码
 *         - 0: 成功
 *         - -1: 参数无效（kmmu 或 raw_data 为空）
 *         - -2: ELF 魔数不匹配
 *         - -3: 内存分配失败
 *         - -4: 没有可加载的段
 */
int kernel_load(load_kernel_info_pack&pak){
    // 参数校验
    bsp_kout.shift_hex();
    if (!pak.kmmu || !pak.kernel_file_entry.raw_data) {
        bsp_kout<< "[ERROR] kernel_load: invalid parameters" << kendl;
        return -1;
    }
    
    // 获取 ELF 文件起始地址
    uint8_t* elf_base = reinterpret_cast<uint8_t*>(pak.kernel_file_entry.raw_data);
    
    // 解析 ELF header
    Elf64_Ehdr* ehdr = reinterpret_cast<Elf64_Ehdr*>(elf_base);
    
    // 验证 ELF 魔数
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || 
        ehdr->e_ident[EI_MAG1] != ELFMAG1 || 
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || 
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        bsp_kout<< "[ERROR] Invalid ELF magic number" << kendl;
        return -2;
    }
    
    bsp_kout<< "[INFO] ELF file detected:" << kendl;
    bsp_kout<< "  - Program headers: " << static_cast<uint32_t>(ehdr->e_phnum) << kendl;
    bsp_kout<< "  - Entry point: 0x" << reinterpret_cast<void*>(ehdr->e_entry) << kendl;
    
    
    // 计算 program header 表起始位置
    uint8_t* phdr_table = elf_base + ehdr->e_phoff;
    
    // 统计 PT_LOAD 段的数量
    uint64_t load_segment_count = 0;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* phdr = reinterpret_cast<Elf64_Phdr*>(phdr_table + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD) {
            load_segment_count++;
        }
    }
    
    if (load_segment_count == 0) {
        bsp_kout<< "[ERROR] No PT_LOAD segments found" << kendl;
        return -4;
    }
    
    bsp_kout<< "[INFO] Found " << load_segment_count << " PT_LOAD segments" << kendl;
    
    // 在堆上分配 VM_entries 数组
    // 考虑内核的其他映射需求，按段数乘以系数分配槽位
    uint64_t vm_entries_capacity = load_segment_count * VM_INTERVAL_ALLOC_FACTOR;
    pak.VM_entries = new loaded_VM_interval[vm_entries_capacity];
    if (!pak.VM_entries) {
        bsp_kout<< "[ERROR] Failed to allocate VM_entries array (capacity: " 
                      << vm_entries_capacity << ")" << kendl;
        return -3;
    }
    
    // 初始化计数器和 ID 分配器
    pak.VM_entry_count = 0;
    uint64_t next_vm_interval_id = 0;  // 从 0 开始自增分配 ID
    
    // 遍历所有 program headers，加载 PT_LOAD 段
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* phdr = reinterpret_cast<Elf64_Phdr*>(phdr_table + i * ehdr->e_phentsize);
        
        // 只处理 PT_LOAD 类型
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        
        // 解析 segment 参数
        phdr_kernel_specify_param param;
        param.reserved = 0;
        param.reserved2 = 0;
        
        // 从 p_flags 解析权限位（ELF 标准：PF_X=1, PF_W=2, PF_R=4）
        param.x = (phdr->p_flags & PF_X) ? 1 : 0;
        param.w = (phdr->p_flags & PF_W) ? 1 : 0;
        param.r = (phdr->p_flags & PF_R) ? 1 : 0;
        
        // 从 param 字段提取 PA/VA随机化标志（使用掩码位运算）
       uint32_t elf_param = phdr->p_flags >> 8;  // 假设 param 在高 16 位或通过其他方式传递
        param.pa_random = (elf_param & 0x1) ? 1 : 0;  // bit[8]
        param.va_random = (elf_param & 0x2) ? 1 : 0;  // bit[9]
        
       uint64_t seg_start = phdr->p_paddr;
       uint64_t seg_size = phdr->p_memsz;
        
        // 处理 PA 随机化：通过 basic_allocator 分配符合对齐要求的物理地址
       if (param.pa_random && phdr->p_align > 0) {
           uint8_t align_log2 = 0;
            // 计算对齐要求的 log2 值
           uint64_t align_temp = phdr->p_align;
            while ((align_temp >>= 1) > 0) {
                align_log2++;
            }
            
            // 确保最小对齐为 4KB
           if (align_log2 < 12) {
                align_log2 = 12;
            }
            
            // 调用 basic_allocator 分配符合对齐要求的物理地址
            phyaddr_t alloc_pa = basic_allocator::pages_alloc(seg_size, align_log2);
           if (alloc_pa == ~0ull) {
               bsp_kout<< "[WARN] Failed to allocate random PA for segment " 
                              << pak.VM_entry_count << ", using default" << kendl;
            } else {
                seg_start = alloc_pa;
               bsp_kout<< "[INFO] Allocated random PA: 0x" << seg_start 
                              << " (align: " << static_cast<uint32_t>(align_log2) << ")" << kendl;
                
                // 标记该区域为已分配
                mem_interval pa_interval{seg_start, seg_size};
                basic_allocator::pages_set(pa_interval, PHY_MEM_TYPE::OS_KERNEL_DATA);
            }
        }else{
            basic_allocator::pages_set(mem_interval{seg_start,seg_size}, PHY_MEM_TYPE::OS_KERNEL_DATA);
        }
        
        // 严格按照 ELF 声明的虚拟地址加载，不支持 VA随机化
      uint64_t seg_vaddr = phdr->p_vaddr;
        
        // 计算文件内容的源地址
        uint8_t* file_content = elf_base + phdr->p_offset;
        
        bsp_kout<< "[INFO] Loading segment " << pak.VM_entry_count << ":" << kendl;
        bsp_kout<< "  - Physical addr: 0x" << seg_start << kendl;
        bsp_kout<< "  - Virtual addr: 0x" << phdr->p_vaddr << kendl;
        bsp_kout<< "  - File size: 0x" << phdr->p_filesz << kendl;
        bsp_kout<< "  - Memory size: 0x" << seg_size << kendl;
        bsp_kout<< "  - Flags: " << (param.r ? "R" : "-") 
                      << (param.w ? "W" : "-") 
                      << (param.x ? "X" : "-") << kendl;
        
        // 创建 VM interval
      loaded_VM_interval& vm_interval = pak.VM_entries[pak.VM_entry_count];
       vm_interval.pbase = seg_start;
       vm_interval.vbase = seg_vaddr;  // 使用 ELF 声明的虚拟地址
       vm_interval.size = seg_size;
        
        // 动态分配 VM_interval_specifyid，从 0 开始自增
        vm_interval.VM_interval_specifyid = next_vm_interval_id++;
        
        // 设置访问权限
        vm_interval.access.is_kernel = 1;
        vm_interval.access.is_writeable = param.w;
        vm_interval.access.is_readable = param.r;
        vm_interval.access.is_executable = param.x;
        vm_interval.access.is_global = seg_vaddr>=0xFFFF800000000000;
        vm_interval.access.cache_strategy = cache_strategy_t::WB;  // 内核代码/数据使用 WB 策略
        
        bsp_kout<< "  - VM_interval_specifyid: " << vm_interval.VM_interval_specifyid << kendl;
        
        // 将文件内容复制到目标物理地址（假设恒等映射环境）
       if (phdr->p_filesz > 0) {
           uint8_t* dest_ptr = reinterpret_cast<uint8_t*>(seg_start);
            // 复制文件内容
            for (uint64_t j = 0; j < phdr->p_filesz; j++) {
                dest_ptr[j] = file_content[j];
            }
        }
        
        // 如果 p_memsz > p_filesz，将多余部分清零（不校验是否为 .bss 段）
       if (seg_size > phdr->p_filesz) {
           uint8_t* zero_start = reinterpret_cast<uint8_t*>(seg_start + phdr->p_filesz);
           uint64_t zero_size = seg_size - phdr->p_filesz;
            
           bsp_kout<< "[INFO] Clearing segment tail: 0x" << zero_size 
                          << " bytes (p_memsz: 0x" << seg_size 
                          << ", p_filesz: 0x" << phdr->p_filesz << ")" << kendl;
            
            // 使用 ksetmem_8 接口清零
            ksetmem_8(zero_start, 0, zero_size);
        }
        int result=pak.kmmu->map(vinterval{vm_interval.pbase,vm_interval.vbase,align_up(vm_interval.size,4096)},vm_interval.access);
        // 递增计数器
        if(result!=OS_SUCCESS){
            bsp_kout<< "kernel_load: map failed" << kendl;
            asm volatile ("hlt");
        }
        pak.VM_entry_count++;
    }
    auto anonymous_mem_map=[&](uint64_t size,uint8_t align_log2,uint32_t assigned_id)->int{
        // 参数校验
        if (size == 0 || align_log2 < 12) {
            bsp_kout<< "[ERROR] anonymous_mem_map: invalid parameters" << kendl;
            return -1;
        }
        
        // 检查是否有足够的槽位
        if (pak.VM_entry_count >= vm_entries_capacity) {
            bsp_kout<< "[ERROR] anonymous_mem_map: no available VM_entries slots" << kendl;
            return -2;
        }
        
        // 确保对齐至少为 4KB
        if (align_log2 < 12) {
            align_log2 = 12;
        }
        
        // 1. 分配物理地址（使用 basic_allocator）
        phyaddr_t alloc_pa = basic_allocator::pages_alloc(align_up(size,4096), align_log2);
        if (alloc_pa == ~0ull) {
            bsp_kout<< "[ERROR] anonymous_mem_map: failed to allocate PA, size: 0x" 
                          << size << kendl;
            return -3;
        }
        
        // 标记该区域为已分配
        mem_interval pa_interval{alloc_pa, align_up(size,4096)};
        int pa_set_result = basic_allocator::pages_set(pa_interval, PHY_MEM_TYPE::OS_ALLOCATABLE_MEMORY);
        if (pa_set_result != OS_SUCCESS) {
            bsp_kout<< "[WARN] anonymous_mem_map: pages_set failed for PA, but continuing..." << kendl;
        }
        ksetmem_8((void*)alloc_pa, 0, size);
        // 2. 分配虚拟地址（统一走 va_alloc 单调递减接口）
        uint64_t alloc_va = va_alloc(align_up(size,4096), align_log2);
        uint64_t alignment = 1ULL << align_log2;
        
        bsp_kout<< "[INFO] Anonymous memory mapping:" << kendl;
        bsp_kout<< "  - Physical addr: 0x" << alloc_pa << kendl;
        bsp_kout<< "  - Virtual addr: 0x" << alloc_va << kendl;
        bsp_kout<< "  - Size: 0x" << size << kendl;
        bsp_kout<< "  - Align: " << static_cast<uint32_t>(align_log2) << " (0x" << alignment << ")" << kendl;
        bsp_kout<< "  - Assigned ID: " << assigned_id << kendl;
        
        // 3. 创建 VM interval 并添加到数组
        loaded_VM_interval& vm_interval = pak.VM_entries[pak.VM_entry_count];
        vm_interval.pbase = alloc_pa;
        vm_interval.vbase = alloc_va;
        vm_interval.size = size;
        vm_interval.VM_interval_specifyid = assigned_id;
        
        // 设置访问权限：RW, WB 策略
        vm_interval.access.is_kernel = 1;
        vm_interval.access.is_writeable = 1;
        vm_interval.access.is_readable = 1;
        vm_interval.access.is_executable = 0;
        vm_interval.access.is_global = 1;
        vm_interval.access.cache_strategy = cache_strategy_t::WB;
        
        // 4. 递增计数器
        pak.VM_entry_count++;
        int result=pak.kmmu->map(vinterval{vm_interval.pbase,vm_interval.vbase,vm_interval.size},vm_interval.access);
        // 递增计数器
        if(result!=OS_SUCCESS){
            bsp_kout<< "kernel_load: map failed" << kendl;
            asm volatile ("hlt");
        }
        
        bsp_kout<< "[INFO] Anonymous memory mapped successfully, total entries: " 
                      << pak.VM_entry_count << "/" << vm_entries_capacity << kendl;
        
        return 0;
    };
    basic_allocator::privious_alloc_end=0;
    // ============================================
    // 加载匿名内存区域（按照 VM_ID 顺序调用）
    // ============================================
    bsp_kout<< "[INFO] Loading anonymous memory regions..." << kendl;
    // 1. BSP 初始栈 (VM_ID_BSP_INIT_STACK = 0x1001)
    int stack_result = anonymous_mem_map(
        BSP_INIT_STACK_SIZE, 
        BSP_INIT_STACK_ALIGN_LOG2, 
        VM_ID_BSP_INIT_STACK
    );
    if (stack_result != 0) {
        bsp_kout<< "[ERROR] Failed to load BSP init stack" << kendl;
    }
    
    // 2. 第一堆位图 (VM_ID_FIRST_HEAP_BITMAP = 0x1002)
    int heap_bitmap_result = anonymous_mem_map(
        FIRST_HEAP_BITMAP_SIZE, 
        FIRST_HEAP_BITMAP_ALIGN_LOG2, 
        VM_ID_FIRST_HEAP_BITMAP
    );
    if (heap_bitmap_result != 0) {
        bsp_kout<< "[ERROR] Failed to load first heap bitmap" << kendl;
    }
    
    // 4. 上层内核空间页目录指针表 (VM_ID_UP_KSPACE_PDPT = 0x2001)
    int pdpt_result = anonymous_mem_map(
        UP_KSPACE_PDPT_SIZE, 
        UP_KSPACE_PDPT_ALIGN_LOG2, 
        VM_ID_UP_KSPACE_PDPT
    );
    if (pdpt_result != 0) {
        bsp_kout<< "[ERROR] Failed to load upper kernel space PDPT" << kendl;
    }
    
    // 5. 第一堆 (VM_ID_FIRST_HEAP = 0x1003)
    int heap_result = anonymous_mem_map(
        FIRST_HEAP_SIZE_CONST, 
        FIRST_HEAP_ALIGN_LOG2, 
        VM_ID_FIRST_HEAP
    );
    if (heap_result != 0) {
        bsp_kout<< "[ERROR] Failed to load first heap" << kendl;
    }
    
    // 6. 日志缓冲区 (VM_ID_LOGBUFFER = 0x1004)
    int logbuffer_result = anonymous_mem_map(
        LOGBUFFER_SIZE, 
        LOGBUFFER_ALIGN_LOG2, 
        VM_ID_LOGBUFFER
    );
    if (logbuffer_result != 0) {
        bsp_kout<< "[ERROR] Failed to load log buffer" << kendl;
    }
    uint64_t entry_count=0;
    phymem_segment*base=basic_allocator::get_pure_memory_view(&entry_count);
    uint64_t accumulated_dram_size=0;
    for(uint64_t i=0;i<entry_count;i++){
        if(base[i].type==PHY_MEM_TYPE::freeSystemRam){
            accumulated_dram_size+=base[i].size;
        }
    }
    uint64_t dram_pages_count=accumulated_dram_size>>12;
    int mem_map_result = anonymous_mem_map(
        align_up(dram_pages_count*sizeof(page),4096), 
        21, 
        VM_ID_MEM_MAP
    );
    if(mem_map_result!=0){
        bsp_kout<< "[ERROR] Failed to load main frame array" << kendl;
    }
    constexpr uint16_t page_size=4096;
    constexpr uint16_t pages_count_per_byte=4;
    uint64_t dram_pages_BCB_bitmaps_4kbpages_count=(dram_pages_count+pages_count_per_byte*page_size-1)/(pages_count_per_byte*page_size);
    int bcb_bitmap_result = anonymous_mem_map(
        dram_pages_BCB_bitmaps_4kbpages_count*4096, 
        21, 
        VM_ID_BCBS_BITMAPS
    );
    if(bcb_bitmap_result!=0){
        bsp_kout<< "[ERROR] Failed to load BCB bitmaps" << kendl;
    }
    bsp_kout<< "[INFO] All anonymous memory regions loaded successfully." << kendl;
    
    // ============================================
    // 设置内核入口点和栈底地址
    // ============================================
    // 内核入口虚拟地址（从 ELF header 获取）
    pak.entry_vaddr = reinterpret_cast<vaddr_t>(ehdr->e_entry);
    bsp_kout<< "[INFO] Kernel entry vaddr: 0x" << pak.entry_vaddr << kendl;
    
    // 查找 BSP 初始栈的 VM_interval 以计算栈底地址
    vaddr_t bsp_stack_vbase = 0;
    for(uint64_t i = 0; i < pak.VM_entry_count; i++){
        if(pak.VM_entries[i].VM_interval_specifyid == VM_ID_BSP_INIT_STACK){
            bsp_stack_vbase = pak.VM_entries[i].vbase;
            break;
        }
    }
    
    if(bsp_stack_vbase != 0){
        // 栈底地址 = 栈基址 + 栈大小（栈向下生长，栈底在高地址）
        pak.stack_bottom = bsp_stack_vbase + BSP_INIT_STACK_SIZE;
        bsp_kout<< "[INFO] BSP stack bottom: 0x" << pak.stack_bottom 
                      << " (base: 0x" << bsp_stack_vbase 
                      << ", size: " << BSP_INIT_STACK_SIZE << ")" << kendl;
    } else {
        bsp_kout<< "[ERROR] Failed to find BSP init stack VM interval" << kendl;
        pak.stack_bottom = 0;
    }
    
    bsp_kout<< "[INFO] Kernel loaded successfully, " 
                  << pak.VM_entry_count << " segments" << kendl;
    
    return 0;
}
