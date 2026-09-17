#pragma once
/**
 * sched_handoff.h —— 跨核 handoff 安全登记 + 调度不可重入门
 *
 * 背景（见 Docs/Debug/WRAITH/WRAITH_multischedule_review.md 与 Docs/Debug/WRAITH/WRAITH_static_review.md）：
 *   样本 w13 `0x2C700` / wh02 双核同爆 / wl02 `this=0` 能够被同一机制一次解释：
 *   「阻塞/退出者已经把自己的状态发布出去（blocked/zombie），但它还没真正切离本核的
 *     内核栈时，另一个核通过唤醒 + sched() 跨核偷取，把同一个 task（=同一片物理栈）
 *     恢复执行」→ 同一片栈被两核同时压栈。
 *
 * 本头提供三件最小、可独立回退的登记/门，供修复 F3/F4/F5 使用：
 *   · g_cpu_running[cpu]  —— [FIX-F4 / MS-1/2/4/5] 「某核当前占用哪片任务栈」的登记。
 *        sched() 完成交接时写入；只要一个 task 仍在任一核的该登记里，就视为「还在跑」。
 *   · g_cpu_in_sched[cpu] —— [FIX-F3 / MS-2/14] 每核「正在调度」门，使 resched 不可重入。
 *   · g_sched_ncpu        —— 核数快照（= logical_processor_count），避免扫描越界。
 *
 * 纪律：
 *   · 全部为「自持」的原子标量读写（对齐的指针/字节），不跨「飞走」的 iretq 持锁；
 *   · 这些登记由 sched()/交接点更新，此处只做读取与初始化；
 *   · 观测/判定函数只读，不改任何调度状态。
 *
 * 依据：Docs/Debug/WRAITH/WRAITH_multischedule_review.md §0/§4.1/§4.3/§6；WRAITH_static_review.md §2.A/§2.B。
 */
#include <stdint.h>
#include "arch/x86_64/abi/base.h"   // MAX_PROCESSORS_COUNT

class task;

// 每核「当前正在执行的任务」：sched() 完成交接后，该核占用的那片内核栈的属主。
// 0/null = 本核尚无登记（启动早期）。
extern task* volatile g_cpu_running[MAX_PROCESSORS_COUNT];

// [FIX-F3] 每核「正在调度」门：next_task_with_routine()/resched() 入口置位，
//          提交切换（iretq 飞走）前清位；resched 见到置位即跳过本次（不嵌套）。
extern volatile uint8_t g_cpu_in_sched[MAX_PROCESSORS_COUNT];

// 本机核数快照（sched_handoff_init 填充；0 = 未初始化）。
extern uint32_t g_sched_ncpu;

// 在 BSP 起 AP / 首次调度之前调用一次。
void sched_handoff_init();

// 返回 task 当前被登记在哪个核上执行；未在任何核上执行返回 -1。
int sched_owner_cpu(const task* t);

// t 是否登记在「除 self_cpu 之外的某个核」上执行（= 仍在别核栈上）。
bool sched_running_off_cpu(const task* t, uint32_t self_cpu);
