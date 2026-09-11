#pragma once
/**
 * printk —— 唯一内核日志面层（formatter + log_sink），bsp_kout 的接替者。
 *
 * ⚠️ 接口契约方向已冻结（方案 B + 单 sink 模型）；实现细节随讨论演化。
 *    设计交流以本文件变更为锚点（草稿文档已删，见 commit a46f730）。
 *
 * 模型：
 *   - vprintk 只对【一个】log_sink 发一条日志，一发一条，一次临界区。
 *   - 由 sink 自行决定「怎么落地」：
 *       · ring  sink → 结构化存（二进制记录头 + body）           ← runtime 唯一目的地
 *       · text  sink → 自己渲染前缀 + body（各后端样式自定）
 *   - runtime printk【绑死】ring：改 screen/uart 都动不到它。
 *   - early_printk【特殊】：多组 vprintk（ring/uart/gop…），
 *     每组各一次临界区，body 各格式化一次（early 罕见，可接受）。
 *
 *   printk(level, fmt, ...)            面层：变参 + __printf 类型安全
 *     └─ vprintk(level, sink, fmt, ap) 核心：kvformat 一次 → 持 sink 锁 → sink->emit 一次
 *           └─ sink->emit(self, level, ts, body, len)   一发一条，原子
 *                 ├─ ring_sink.emit → 二进制头 {len,level,seq,ts} + body（读侧过滤）
 *                 └─ text_sink.emit → render_prefix() + body（UART/屏/early）
 *
 * 契约（红线）：
 *   1) formatter 唯一：kvformat（early/runtime/panic 共用）；无锁/无分配/无浮点。
 *   2) ring 里 level/ts 是【二进制字段】（log_rec_hdr），不是文本前缀。
 *      读侧（kshell dmesg）直接读字段过滤 / 排序，【禁止】字符串解析。
 *   3) emit 是【压死规格】的落地原语：同步、无阻塞、无格式、无分配。
 *      - 只能在【持有 sink->sink_lock】时调用；
 *      - emit 内部【禁止】再调 printk（同锁自旋死）。
 *   4) sink_lock 必须用 spinlock_interrupt_about_guard（irq-save）取。
 *      printk 会在 IRQ 上下文被调用（NVMe AER, NVMe_interrupts.cpp:134/156/223），
 *      裸 spinlock_cpp_t::lock() 会「线程持锁 → IRQ 里 printk → 同 CPU 自旋死」。
 *   5) panic 不走本路径：crash 现场可能正持 sink_lock，取锁即死 → panic 走 dumper 旁路。
 *   6) 无 fanout / 无全局 router：多目的地 = 多次 vprintk（early 特例），
 *      不是 vprintk 内部分流。
 */
#include <cstdarg>
#include <stdint.h>
#include "abi/os_error_definitions.h"
#include "util/lock.h"

namespace klog
{

// —— 级别：复用 abi/os_error_definitions.h 的 level_code ——
//    INVALID=0 < INFO=1 < NOTICE=2 < WARNING=3 < ERROR=4 < FATAL=5
using level_t = uint8_t;
namespace level = level_code;

// 单行 body 上限（栈缓冲；超额截断并打 "...[truncated]" 标记）。对齐 Linux LOG_LINE_MAX。
constexpr uint32_t LOG_LINE_MAX = 1024;

// ——— log_sink：一条日志的落地原语 ———
// 「函数指针 + io_ctx 复合体」：指针负责落地，self 携带上下文。
struct log_sink
{
    // 一发一条。sink 自行决定怎么用 level/ts/body：
    //   ring sink → 存二进制头 + body；text sink → 渲染前缀 + body。
    // 契约：调用方必须已持有 sink_lock；内部禁止调 printk；无分配、无阻塞。
    void (*emit)(void* self, level_t level, uint64_t ts_ns,
                 const char* body, uint64_t len);
    void*           self;       // io_ctx：ring 模块句柄 / 串口端口 / 屏幕状态 …
    spinlock_cpp_t* sink_lock;  // 【必须非空】；vprintk 单次临界区（irq-save）
};

// ——— 纯格式化引擎：无 I/O、无静态状态、可重入、可 host 单测 ———
// 返回写入 out 的字节数（不含 NUL）；>= cap 表示发生截断。
int kvformat(char* out, uint64_t cap, const char* fmt, va_list ap);

// ——— 前缀渲染：[  ts] <LEVEL> ——
// 供【文本 sink】复用：取值与文本化只有一份（避免同一条日志三处不一致）；
// 样式（颜色/省略时间戳）归各后端。ts_ns==0（早期无时基）时省略时间戳段。
uint32_t render_prefix(char* out, uint64_t cap, level_t level, uint64_t ts_ns);

// ——— 核心：唯一 formatter 入口 ———
// 流程（单次临界区）：ts=now_ts(); kvformat(body); 取 sink_lock; sink->emit(level,ts,body,len)。
// sink 必须非空（runtime 缺省即 ring sink）。
void vprintk(level_t level, const log_sink* sink, const char* fmt, va_list ap);

// ——— 面层 ———
#if defined(__GNUC__)
#  define KLOG_PRINTF_ATTR(fmt_idx, arg_idx) \
        __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define KLOG_PRINTF_ATTR(fmt_idx, arg_idx)
#endif

void printk(level_t level, const char* fmt, ...) KLOG_PRINTF_ATTR(2, 3);

// ——— early：特殊路径，【禁止】另起 formatter / 独立 API（early 全部 INFO）———
// 实现：对 ring / 屏 / polling UART 等【多组 vprintk】，每组各一次临界区。
// body 会按 sink 数重复格式化（early 罕见，接受；不为此引入 compose 缓存）。
void early_printk(const char* fmt, ...) KLOG_PRINTF_ATTR(1, 2);

// ——— ring 结构化记录头（B 的核心契约）———
// 定长头 + body；回绕时整条记录被覆盖（头 + body 一起丢弃，不产生半条）。
struct log_rec_hdr
{
    uint16_t len;     // body 字节数
    uint8_t  level;   // level_code
    uint8_t  _rsv;    // 对齐保留
    uint32_t seq;     // 单调序号（回绕/丢失检测）
    uint64_t ts_ns;   // 记录时间戳；0 = 无时基
};
static_assert(sizeof(log_rec_hdr) == 16, "log_rec_hdr must be 16 bytes");

// ——— 缺省 sink（runtime 唯一目的地 = ring）———
//   runtime : set_default_sink(ring_sink)   —— 「绑死只打印那个缓冲区模块」
//   early   : 不走缺省；early_printk 显式多组 vprintk
//   panic   : 走 dumper，不经此处
void            set_default_sink(const log_sink* sink);
const log_sink* get_default_sink();

}  // namespace klog

// ——— 宏层：调用点手感对齐 Linux ——
// 注意方向：不用 EMERG/DEBUG 的歧义命名，用 FATAL 表示最严重（=5）。
#define pr_fatal(fmt, ...)  klog::printk(klog::level::FATAL,    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)    klog::printk(klog::level::ERROR,    fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   klog::printk(klog::level::WARNING,  fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) klog::printk(klog::level::NOTICE,   fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   klog::printk(klog::level::INFO,     fmt, ##__VA_ARGS__)
