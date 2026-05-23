#pragma once
#include "memory/memory_base.h"
#include "arch/x86_64/boot.h"
#include "init/kernel_mmu.h"

// ============================================================================
// init.elf 阶段交付上下文
// ============================================================================
// 每个 phase 函数产出对应的上下文结构体，后续 phase 通过 const 指针消费。
// 消除文件级全局变量，编译器保证依赖的显式传递。

// Phase 2 产物 → 供 Phase 2.5 / Phase 3a / Phase 3b 消费
struct ctx_early_mem {
    phyaddr_t xsdt_base;
    phyaddr_t ramfs_base;
    uint64_t  ramfs_size;
};

// Phase 3a 产物 → 供 Phase 3b / Phase 4 消费
struct ctx_kernel_loaded {
    kernel_mmu*   kmmu;
    phyaddr_t     kimg_pbase;           // 瞬态端 kernel.elf 文件映像物理基址
    uint64_t      kimg_file_size;       // 文件原始字节数（非对齐）
    vm_interval   kIMG_self_window;     // 瞬态端文件映像的 VA 窗口（仅供自省）
    vaddr_t       entry_vaddr;          // kernel.elf 入口点
};
// 注：kBSS_interval 已移除——BSS 作为普通 PT_LOAD(有 0x100 标志) 处理，
//     物理地址直接写入 ELF 程序头表 p_paddr。kernel.elf 自省时
//     扫描程序头表即得所有段的 PA，无需 kBSS_interval 专门路径。

// Phase 3b 产物 → 供 Phase 4 / Phase 4.5 消费
// kernel_entry_stack 已移除——Phase 4.5 跳转时用 BSP GS 复合体的 rsp0 栈
struct ctx_intervals {
    vm_interval           FPA_bitmaps;
    vm_interval           log_buffer;
    movable_file_entry_t  symtable_file;
    movable_file_entry_t  initramfs_file;
    vm_interval           Kspace_phyaddr_access_window;
    vaddr_t               pages_arr_vbase;
    loaded_VM_interval*   extra_vm_arr;
    uint64_t              extra_vm_count;
    x86_specify_init_to_kernel_info arch_info;
};
