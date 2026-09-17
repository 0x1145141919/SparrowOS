#pragma once
/**
 * wraith_probe.h —— WRAITH 首爆取证探针（检测 + 留证；不改任何时序/不修竞态）。
 *
 * 定位（与 Docs/Debug/WRAITH.md §8-5 对齐）：
 *   把「静默栈损坏 / 首爆被吞」升级为「首爆即留证」。
 *   本头只提供【便宜、irq-safe、无堆分配、可回滚】的留证原语：
 *     · WRAITH_LOG(...)   —— 一段自持临界区的裸文本留证（走 interrupt_log_ring）；
 *     · WRAITH_TRACE(...) —— 受 wraith::probe_verbose 门控的「面包屑」留证；
 *     · wraith::* 自证指纹读取器（rsp / gs_base / gdtr / 当前 task）。
 *   留证一条 = 一次临界区，语句结束即释放锁 —— 【绝不】跨「飞走」的
 *   sched()/atomic_load()/[[noreturn]] 路径持有锁（否则锁永久泄漏）。
 *
 * 观测效应：面包屑在热路径（每次中断/调度）各写一环；实测代价 = 一把自旋锁 +
 *   一次 ≤~160B 的 memcpy（无 UART/无分配）。probe_verbose=0 可在线消融面包屑，
 *   只留「命中即 panic」的断言（常见情形零开销）。详见本次排障报告。
 *
 * 纪律：本头【只读】现场，不写任何内核状态（除环本身）。
 */
#include <stdint.h>
#include "util/debug_tmp_ring_buff.h"                        // interrupt_log_ring / spinlock guard
#include "arch/x86_64/abi/GS_complex.h"                      // get_gs_base / gs_base_is_sane
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"      // PROCESSOR_NOW_RUNNING_TASK_GS_INDEX
#include "arch/x86_64/abi/pt_regs.h"                         // x64_standard_context_v2 / IDT_CS
#include "util/arch/x86-64/cpuid_intel.h"                    // fast_get_processor_id / read_gs_u64

namespace wraith {

// 面包屑总开关（定义在 src/utils/debug_tmp_ring_buff.cpp，默认 1）。
// 置 0 ⇒ 只保留「命中即 panic」的断言，消除面包屑的环锁/带宽观测效应。
extern uint32_t probe_verbose;

// ── 当前 RSP（本函数调用点的栈指针）──
static inline uint64_t rsp_now()
{
    uint64_t v;
    asm volatile("mov %%rsp, %0" : "=r"(v));
    return v;
}

// ── 当前 GS_BASE（本核 gs_complex_t 基址；损坏时是关键指纹）──
static inline uint64_t gs_now()
{
    return (uint64_t)get_gs_base();
}

// ── GDTR（limit+base）；GDT 被踩时 base 会失真 ──
// 用 10 字节裸缓冲搬运，回避 packed-struct 的"sized memory operand"歧义。
static inline void gdtr_now(uint16_t* out_limit, uint64_t* out_base)
{
    uint8_t buf[10];
    asm volatile("sgdt %0" : "=m"(buf));
    uint8_t* lb = (uint8_t*)out_limit;
    lb[0] = buf[0]; lb[1] = buf[1];
    uint8_t* bb = (uint8_t*)out_base;
    for (int i = 0; i < 8; ++i) bb[i] = buf[2 + i];
}

// ── 当前 GS 槽里的「正在运行 task」指针 ──
static inline uint64_t now_running_task()
{
    return read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
}

// ── 「rsp 是否落在给定区间 [base, base+pages*4K)」自证 ──
static inline bool in_range(uint64_t rsp, uint64_t base, uint32_t pages)
{
    if (!base || !pages) return false;
    return rsp >= base && rsp < (base + ((uint64_t)pages << 12));
}

// ── 内核高半区规范地址判定（廉价；“指针是否被踩”的自证）──
// 合法内核指针必落在 0xFFFF8000… 高半区；低半/野值（如 wh02 的取时值 0x75fbf40）判否。
static inline bool kernel_va_ok(const void* p)
{
    return ((uint64_t)p >> 48) == 0xFFFFULL;
}

// ── 异常帧留证：一行写尽「帧 + 现场自证指纹」──
// tag    : 站点标签（PF-kern / PF-CS-CLOBBERED / GP-kern / GP-CS-CLOBBERED / TS-...）
// frame  : 标准异常帧（其 core_ctx.idtctx.iret 即被踩目标）
// errcode: 硬件错误码；linear : #PF 的线性地址（CR2/入参）
static inline void log_exc_frame(const char* tag, const x64_standard_context_v2* frame,
                                 uint64_t errcode, uint64_t linear)
{
    if (!interrupt_log_ring) return;
    uint16_t gdt_limit = 0;
    uint64_t gdt_base  = 0;
    gdtr_now(&gdt_limit, &gdt_base);
    const uint64_t cs   = frame->core_ctx.idtctx.iret.cs;
    const uint64_t rip  = frame->core_ctx.idtctx.iret.rip;
    const uint64_t irsp = frame->core_ctx.idtctx.iret.rsp;
    const uint64_t rfl  = frame->core_ctx.idtctx.iret.rflags;
    const uint64_t tsk  = now_running_task();
    spinlock_interrupt_about_guard g(interrupt_log_ring->lock);
    interrupt_log_ring->print(
        "%s pid=%u cs=%llx lo=%llx rip=%llx irsp=%llx rfl=%llx err=%llx lin=%llx "
        "gs=%llx gdt=%llx tsk=%llx rsp=%llx\n",
        tag, (unsigned)fast_get_processor_id(),
        (unsigned long long)cs, (unsigned long long)(cs & 0x3ULL),
        (unsigned long long)rip, (unsigned long long)irsp,
        (unsigned long long)rfl, (unsigned long long)errcode,
        (unsigned long long)linear, (unsigned long long)gs_now(),
        (unsigned long long)gdt_base, (unsigned long long)tsk,
        (unsigned long long)rsp_now());
}

}  // namespace wraith

// ── 留证：一段自持临界区；语句结束即释放锁 ──
#define WRAITH_LOG(...)                                                          \
    do {                                                                         \
        if (interrupt_log_ring) {                                                \
            spinlock_interrupt_about_guard __wraith_g(interrupt_log_ring->lock); \
            interrupt_log_ring->print(__VA_ARGS__);                              \
        }                                                                        \
    } while (0)

// ── 面包屑：受 probe_verbose 门控（默认开；置 0 消融）──
#define WRAITH_TRACE(...)                        \
    do {                                         \
        if (wraith::probe_verbose) WRAITH_LOG(__VA_ARGS__); \
    } while (0)
