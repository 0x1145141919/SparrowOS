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
//   运行时线程世界的入口，也是后续调度器/线程压测语料（硬编码场景）的落点。
//
// 后续测试约定（硬编码法）：
//   同一 kernel.elf，测试分支按场景派生测试线程；release 不引入测试场景分支。
//   当前该分支（if_real_init==false）由 MMU 压测占用（见下 KTHREAD_TEST_SCENARIO 段）。
// ════════════════════════════════════════════════════════════════

// 始祖线程入口：由 create_first_kthread()（kinit.cpp）作为首个 kthread 派生。
// 职责：spawn 各内核服务线程（BQ 超时扫描 / NVMe 并行初始化 / kshell 框架 等）后常驻。
void* kthread_ymir(void* null);

// ── 测试 / 压测线程符号 ──────────────────────────────────────
// Collatz 序列计算（返回其运行所在 CPU id）——调度器压力 / 并行性测试载体。
void* Collatz_kthread(void* init_value);

// 其它预置内核线程（定义在各自模块，这里统一声明，供 ymir 派生 / 测试引用）：
void* burnin_thread(void* arg);
void* i8042_char_listener_thread(void* arg);
void* bq_timeout_sweeper(void*);      // 定义于 src/scheduler/bq_system.cpp

// ── 运行模式开关（业务 vs 测试）───────────────────────────
// true : 业务初始化（派生 BQ / i8042 / NVMe / kshell 等内核服务线程）；
// false: WRAITH 测试初始化（派生测试线程树；业务线程视为噪声，默认跳过）。
// 定义在 kthread_ymir.cpp；测试构建默认 false，正常构建恒 true。
extern bool if_real_init;

// 是否派生 BQ 超时扫描线程（“兜底计时器”）；两档都可开关，测试时按需保留该噪声源。
extern bool if_bq_sweeper;

// ════════════════════════════════════════════════════════════════
// 测试分支（仅 -DKTHREAD_TEST_SCENARIO 时编译）——【鸠占鹊巢：MMU 压测】
//
// 本分支现由【MMU（Kspace 三接口 + invalidate_tlb）压测】占用（见
// Docs/Memory/MMU压测落地设计.md）。旧调度器/WRAITH 测试场景已移除，
// 由 git 历史兜底（HEAD 一带；调度器压测将来按 Docs/Sched/ 两份文档现场重建）。
//
// 保留的通用地基：
//   · wraith_freeze() 截停闸门（串口 #TB#/reason + outb(0x80,0xDB) 魔法断点）；
//   · wraith_test_ring（线程上下文日志环，ring-dump.py 可捞）。
// 新增：g_mmu_* 探针 + wraith 断言账本（g_mmu_ledger），tool 直接按符号读。
// ════════════════════════════════════════════════════════════════
#ifdef KTHREAD_TEST_SCENARIO
#include <stdint.h>

// MMU 测试入口：由 kthread_ymir 在 FLAG 打开时调用。
// 模式经 fw_cfg（opt/sparrow/test）选择：full(默认) / fonly / pf。
void mmu_test_main();

// 截停闸门：串口打 #TB#/reason → 环留证 → outb(0x80,0xDB)（QEMU 补丁 vm_stop）→ cli;hlt。
void wraith_freeze(const char* reason);
#endif
