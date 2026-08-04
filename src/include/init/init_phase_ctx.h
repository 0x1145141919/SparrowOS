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

// ctx_kernel_loaded 已废弃（退居幕后）——Phase 3a 改为纯产出方：
//   产物（四段 / kimg / entry_vaddr）全部进资产容器（g_asset_registry，隐式状态），
//   函数只返回 loc_code_t；下游调用点从容器按 arg0 取资产。
//   原 kl 字段去向：
//     kmmu          → init_main 直接持有
//     kimg_pbase / kimg_file_size / kIMG_self_window → "kimg"（movable_file_entry_t）
//     entry_vaddr   → "entry_vaddr"（scalar）


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
