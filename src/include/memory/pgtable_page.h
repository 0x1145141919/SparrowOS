#pragma once
#include "memory/memory_base.h"
#include "abi/os_error_definitions.h"

// ════════════════════════════════════════════════════════════════
// pgtable_page — 页表页专用分配/释放轮子（4KB 物理页，页表页身份）
//
// 页表页身份特殊：一律以 page_state_t::kernel_pgtable 记账（FPA 重建 /
// 状态查询时页表页不会被当作普通内存复用），分配后立即整页清零
// （rep stosq，经主窗口 PHYACC_VA）。
//
// 分配阶段路由（early_alloc_open 判定）：
//   - true  → page_frame_state_mgr 早期直配（ascending first-fit，4KB 对齐）
//   - false → FPA（绝大多数 = 运行时履职，[[unlikely]] 标记早期分支，
//              即暗示编译器 FPA 为热路径）
//   params 仅 FPA 路径生效（如内核空间用 BUDDY_ALLOC_DOWN_4GB）。
//
// 释放：
//   - early 阶段分配的页表页生命周期 = 内核（不可释放，no-op）
//   - 运行时（early 已 close）走 FPA 归还
//   ⚠ 若早期分配的页表页拖到运行时才释放，会误走 FPA——early 页表页
//     设计上永驻，不允许释放（调用方保证）。
// ════════════════════════════════════════════════════════════════

// 分配一页页表页（4KB）。成功：返回物理基址（已清零）；失败：返回 0 并填 kurd。
phyaddr_t pgtable_page_alloc(buddy_alloc_params params, KURD_t& kurd);

// 归还一页页表页。early 阶段为 no-op（页表页永驻）；运行时走 FPA。
KURD_t pgtable_page_free(phyaddr_t base);
