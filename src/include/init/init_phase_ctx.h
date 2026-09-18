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
    uint64_t  dram_top;    // freeSystemRam 物理上界（2d 纯视图快照算好，替代 page_allocator::dram_top）
};

// ctx_kernel_loaded 已废弃（退居幕后）——Phase 3a 改为纯产出方：
//   产物（四段 / kimg）全部进资产容器（g_asset_registry，隐式状态），
//   函数只返回 loc_code_t；下游调用点从容器按 arg0 取资产。
//   原 kl 字段去向：
//     kmmu          → init_main 直接持有
//     kimg_pbase / kimg_file_size / kIMG_self_window → "kimg"（movable_file_entry_t）
//     entry_vaddr   → phase_3a out-param 直出（init 内部 phase_4.5 消费）


// Phase 3b 产物 → 供 Phase 4 / Phase 4.5 消费
// kernel_entry_stack 已移除——Phase 4.5 跳转时用 BSP GS 复合体的 rsp0 栈
//
// 死字段已扬（资产容器 = 唯一真源，这些 iv 成员无人读）：
//   FPA_bitmaps / log_buffer / symtable_file / initramfs_file /
//   Kspace_phyaddr_access_window / pages_arr_vbase / extra_vm_arr / extra_vm_count。
// 现在 iv 只剩 Phase 4.5 跳转真正要读的 arch_info。
struct ctx_intervals {
    x86_specify_init_to_kernel_info arch_info;
};
