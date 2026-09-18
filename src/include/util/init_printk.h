#pragma once
/**
 * init_printk（kernel.elf 面层）—— 对齐 init.elf 的同名面层，用于 kernel 启动期
 * （入口 → create_first_kthread）替代 bsp_kout。
 *
 * 与 init.elf 的 init_printk【函数签名完全一致】；差别只在：
 *   · 无 "[INIT] " 前缀（这是 kernel 世界，不是 init 阶段）；
 *   · 环（转生自 init 的 ring_log）恒落；文本后端 UART / GOP 由启动期 boot_cfg
 *     的 g_boot_cfg.log_uart / .log_gop 独立放行；
 *   · level 固定 INFO、无锁 —— 调用窗口限定在 create_first_kthread 之前（单 BSP）。
 *
 * 环（关键）：kernel 不新造环，而是认领 init.elf 转生的 ring_log blob
 *   （abi/kring_soul.h DmesgRing_handoff）：经主窗口把物理凭证重链成内核可访问
 *   地址后，把【同一条 v2 环】重绑到本窗口 VA —— odometer / record_count 连续，
 *   init 在跳转前写的记录与 kernel 的第一条记录无缝接续。
 *
 * 生命周期：
 *   init_printk_bringup()  由 exec_env_prepare 在 UART / GOP / ring 三后端就绪后
 *                          调用一次（认领环 + 置就绪位）；
 *   就绪前 init_printk 为 no-op（与 init 侧「环未绑则空转」同构）。
 */
#include <stdint.h>

#if defined(__GNUC__)
#  define KINIT_PRINTK_ATTR(fmt_idx, arg_idx) \
        __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define KINIT_PRINTK_ATTR(fmt_idx, arg_idx)
#endif

// kernel.elf 启动期统一日志出口：无前缀 + 环/UART/GOP 三后端广播。
// 须在 init_printk_bringup() 之后才有输出。
void init_printk(const char* fmt, ...) KINIT_PRINTK_ATTR(1, 2);

// 上位：认领继承的 ring_log blob 并以本窗口 VA 重绑；置就绪位。幂等。
void init_printk_bringup();

// 便利：printf → 调用方缓冲区（与 init_printk 共用同一 formatter；不广播、不掉环）。
inline uint32_t init_format(char* out, uint32_t cap, const char* fmt, ...) KINIT_PRINTK_ATTR(3, 4);

#include <cstdarg>
#include "util/vprintkv2.h"

inline uint32_t init_format(char* out, uint32_t cap, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uint64_t r = klog::vprintkv2(fmt, ap, out, cap);
    va_end(ap);
    return (uint32_t)(r & 0xFFFFFFFFu);
}
