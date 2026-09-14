#pragma once
/**
 * printk（init.elf 专用）—— 与 kernel.elf 的 util/printk.h【完全独立】，只共享格式化核心。
 *
 * init.elf 场景：单线程顺序执行、完成单一工作，无调度 / IRQ / 并发。因此本面层：
 *   · 无 level  —— init 全是顺序过程记录，不进严重度过滤体系；
 *   · 无 ts     —— 早期无时基；日志时间戳省略；
 *   · 无锁      —— 单线程，不套 sink_lock 抽象。
 *
 * 固定文本前缀 "[INIT] "（标识「初始化阶段文本」），并硬编码【广播】三条腿：
 *   ① 内存环（DmesgRingBuffer_v2 结构化记录；待 soul / 环头下沉 abi/ 后接入）
 *   ② GOP 文本控制台
 *   ③ UART(COM1)
 *
 * 唯一 formatter 仍是 klog::vprintkv2（util/vprintkv2.h，freestanding，双向共用）。
 */
#include "util/vprintkv2.h"

namespace klog { }
using klog::LOG_LINE_MAX;

#if defined(__GNUC__)
#  define INIT_PRINTK_ATTR(fmt_idx, arg_idx) \
        __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define INIT_PRINTK_ATTR(fmt_idx, arg_idx)
#endif

// init.elf 的唯一日志出口：固定 "[INIT] " 前缀 + 落环（本轮），无 level / 无 ts / 无锁。
// 全局函数（对齐 bsp_kout 的调用手感）：调用点直接 init_printk(...)，【不写】[INIT]。
void init_printk(const char* fmt, ...) INIT_PRINTK_ATTR(1, 2);

// 便利：printf → 调用方缓冲区（用于循环拼一行等多段组装），返回写入字节数。
// 与 init_printk 共用同一 formatter；不落环、不广播。
inline uint32_t init_format(char* out, uint32_t cap, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uint64_t r = klog::vprintkv2(fmt, ap, out, cap);
    va_end(ap);
    return (uint32_t)(r & 0xFFFFFFFFu);
}
