#pragma once
/**
 * vprintkv2 —— 无状态纯内存格式化核心（freestanding）。
 *
 * 归属：kernel.elf / init.elf / 宿主三方【共用】的唯一 formatter。
 *   · 只做「fmt + va_list → 调用方内存 buffer」的纯变换；
 *   · 无 I/O、无静态状态、无锁、无分配、无浮点（不碰 XMM/FPU）、可重入；
 *   · 不绑定任何时基 / 系统头：可 -ffreestanding 编进内核，也可直接编进宿主用户态。
 *
 * buffer 的可靠性与生命周期、解析结果如何处置，全归调用者。
 * level / ts / 前缀 / sink / 锁 一律【不进】本核心——它们是与 buffer 并排的元数据。
 */
#include <cstdarg>
#include <stdint.h>

namespace klog
{

// 单行上限（调用侧栈缓冲的惯用容量；含结尾 NUL 位）。对齐 Linux LOG_LINE_MAX。
constexpr uint32_t LOG_LINE_MAX = 1024;

// —— 核心：唯一 formatter 入口 ——
// 返回打包码：[0:31] = 实际写入字节数（不含结尾 NUL）；bit63 = 溢出（发生截断）；
//   [32:62] 保留（暂为 0）。
// max_limit = 缓冲区【总容量】（含结尾 NUL 位）；调用者保证 out_buff 生命周期与地址稳定。
// 【不支持】float（%f/%e/%g/…）→ 输出 `<%f? unsupported>` 且【不】读取参数；
//   未知转换 → `<%x? unknown>`（不静默吐字面量）。
uint64_t vprintkv2(const char* fmt, va_list ap, void* out_buff, uint32_t max_limit);

}  // namespace klog
