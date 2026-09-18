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
// 后续测试约定（规划 · 硬编码法）：
//   同一 kernel.elf，按场景名在此派生测试线程（Collatz_kthread 等），
//   压测调度器状态机与跨核并发；release 不引入测试场景分支。
//   （WRAITH 收尾的调度器大压测即落点于此，见 Docs/Debug/WRAITH/RESOLUTION_2026-09-17.md §4）
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
// WRAITH 验收 · 测试分支（仅 -DKTHREAD_TEST_SCENARIO 时编译）
//
// 目的：验证 F1–F5 修复的牢固性。ymir 派生一棵【测试线程树】：
//   · 线程可自派生（自相似）；
//   · 跑完【不原子销毁】（set_zombie 后不 release）→ 僵尸停车区保留其内核栈，
//     供截停后栈检查工具（Tools/wraith/）取证；
//   · 在“合适时机”主动触发魔法断点冻结（wraith_freeze）；
//   · 每个测试线程在【浅层帧】持一枚栈金丝雀，并在每轮热路径自校验。
// 观测低开销：金丝雀/登记只在热循环内做原子读写与比较，不打印、不上锁。
// 详见 Docs/Debug/WRAITH/（战报）与 Tools/wraith/（工具）。
// ════════════════════════════════════════════════════════════════
#ifdef KTHREAD_TEST_SCENARIO
#include <stdint.h>

constexpr uint64_t WRAITH_CANARY_MAGIC = 0x57425241495448ull;  // "WBRAITH"
constexpr uint32_t WRAITH_TEST_MAX     = 128;

// 每个测试线程登记一条（tool 直接按符号读它）。
struct wraith_test_slot {
    volatile uint64_t in_use;
    volatile uint64_t tid;
    volatile uint64_t task_ptr;         // task 对象地址（tool 用它读 task 字段）
    volatile uint64_t role;
    volatile uint64_t seq;              // 热循环轮数
    volatile uint64_t last_cpu;
    volatile uint64_t canary_addr;      // 该线程【栈上】金丝雀的地址
    volatile uint64_t canary_expected;  // 期望值 = MAGIC ^ tid
};

extern wraith_test_slot g_wraith_slots[WRAITH_TEST_MAX];
extern volatile uint64_t g_wraith_slot_count;

// 测试入口：由 kthread_ymir 在 FLAG 打开时调用（派生 root）。
void kthread_test_main();

// 截停闸门：串口打 #TB# → 环留证 → outb(0x80,0xDB)（QEMU 补丁 vm_stop）→ cli;hlt。
void wraith_freeze(const char* reason);
#endif
