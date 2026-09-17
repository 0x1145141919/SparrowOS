#pragma once
/**
 * debug_tmp_ring_buff.h —— 调试/断点场景的「裸文本暂存环」（长青件）。
 *
 * 与 DmesgRingBuffer(_v2) 的区别（刻意收窄）：
 *   · 【不插记录头】—— 没有 log_record_head_t，没有 ts/level/seq 字段；
 *   · 一次 print = 「整段文本追加 + 游标推进 + 计数 +1」；
 *   · formatter 复用唯一核心 klog::vprintkv2（栈上 LOG_LINE_MAX 缓冲）；
 *   · 纯内存、无后端、无 fire-and-forget —— print 返回时字节【一定】已在环里。
 *
 * 定位：调试场景的 scratch 环（断言 / 中断黑匣子），可多实例化（per-CPU）。
 * 为什么自成一室：上面两件（DmesgRingBuffer/_v2）是罕见的底层原语，而这个是
 * 调试场景的长青件 —— 不该同进同出。
 *
 * 锁【不属于本类】：print 内部【绝不】取锁 / 关中断；公开 lock 供调用方
 * 自行编排（spinlock_interrupt_about_guard）——
 *   · 一段连续 print 可共用一次临界区（成块原子）；
 *   · panic / 冻结现场可选择不取锁直接写。
 *
 * 溢出策略：无情覆盖 —— 写入永不失败、永不丢「最新」；环满即盖掉最老的字节。
 * 空写（n == 0）不推进游标、不计数。
 */
#include "abi/kring_soul.h"   // DmesgRingBuffer_soul（环之灵魂）
#include "util/lock.h"        // spinlock_cpp_t

class debug_tmp_ring_buff
{
private:
    DmesgRingBuffer_soul working_soul;
public:
    spinlock_cpp_t lock;   // 调用方自取；print 内部不碰
    // 出生即绑定一份 soul（【按值】继承：环的实体必须活得比 soul 临时量久）。
    explicit debug_tmp_ring_buff(DmesgRingBuffer_soul* soul);
    // 追加一条文本：fmt + 变参。【不】自动补 '\n'（行尾由调用方 fmt 自带）。
    void print(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    // 灵魂泄露出去，自己看着办拿不拿锁（panic 读侧可不取锁）。
    const DmesgRingBuffer_soul* get_soul();
};

// ── 全局 IRQ-safe 断言 / 日志环（kernel.elf 实例）──
// 定位：断言 / 中断上下文的「一定落盘」文本黑匣子，底层是经主窗口（PHYACC_VA）重链的 4MiB 环。
// 出生：kernel_start 在 AP bring-up 前完成 FPA 分配 + 绑定（见 kinit.cpp）；出生前为 nullptr。
// 用法（临界区由调用方自编排）：
//     if (interrupt_log_ring) {
//         spinlock_interrupt_about_guard g(interrupt_log_ring->lock);
//         interrupt_log_ring->print("...");
//     }
// 头文件只声明；实体定义在 debug_tmp_ring_buff.cpp（初始 nullptr）。
extern debug_tmp_ring_buff* interrupt_log_ring;
