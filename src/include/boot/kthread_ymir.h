#pragma once
#include <stdint.h>

// ════════════════════════════════════════════════════════════════
// kthread_ymir — 内核线程初始化（始祖线程 YMIR 及其派生线程）
//
// 设计边界（为什么单独成一个文件）：
//   kinit.cpp 只保留「内存完全成熟 → 跳入线程运行时」的引导胶水
//   （kernel_start / create_first_kthread / ap_init /
//     ipi_start_sched 等 IPI 入口 + C++ runtime 胶水）；关机路径在 boot/shutdown.cpp。
//   由 kthread_ymir 派生或管理的「线程初始化」全部集中在本文件——它是
//   运行时线程世界的入口。
//
// 当前本文件只含业务初始化逻辑；测试场景分支（MMU/WRAITH 等）已摘除，
// 代码由 git 历史兜底（设计/实测记录见 Docs/Memory/MMU压测落地设计.md）。
// ════════════════════════════════════════════════════════════════

// 始祖线程入口：由 create_first_kthread()（kinit.cpp）作为首个 kthread 派生。
// 职责：spawn 各内核服务线程（BQ 超时扫描 / NVMe 并行初始化 / kshell 框架 等）后常驻。
void* kthread_ymir(void* null);

// 其它预置内核线程（定义在各自模块，这里统一声明，供 ymir 派生 / 测试引用）：
void* burnin_thread(void* arg);
void* i8042_char_listener_thread(void* arg);
void* bq_timeout_sweeper(void*);      // 定义于 src/scheduler/bq_system.cpp

// ── 运行模式开关 ─────────────────────────────────────────────
// 世界选择由启动期 boot_cfg.ymir 决定（见 boot/boot_cfg.h）：
//   NORMAL  → 业务初始化
//   TEST_*  → 预留测试分支（当前仅跳过业务初始化，代码由 git 历史兜底）

// 是否派生 BQ 超时扫描线程（“兜底计时器”）；定义在 kthread_ymir.cpp。
extern bool if_bq_sweeper;
