#pragma once
/**
 * printk —— 唯一内核日志面层（formatter + 文本出口 + 结构化 ring 记录），bsp_kout 的接替者。
 *
 * ⚠️ 接口契约方向已冻结（方案 B）；实现细节随讨论演化。
 *    设计交流以本文件变更为锚点（草稿文档已删，见 commit a46f730）。
 *
 * 分层（B：ring 是结构化记录库，emit 家族只服务文本出口）：
 *
 *   printk(level, fmt, ...)                    面层：变参 + __printf 类型安全
 *     └─ vprintk(level, sink, fmt, ap)         核心：body 只格式化一次，两处落地
 *           ├─ dmesg_record(level, ts, body)   ① 结构化落 ring（带二进制记录头）
 *           └─ sink->emit(self, prefix, n)     ② 文本出口（持 sink_lock，一发一条）
 *              sink->emit(self, body, m)          prefix 与 body 同临界区连发
 *                    └─ 文本 sink：UART / GOP / early（dumb 字节管道）
 *
 * 契约（红线）：
 *   1) formatter 唯一：kvformat（early/runtime/panic 共用）；无锁/无分配/无浮点。
 *   2) ring 里 level/ts 是【二进制字段】（log_rec_hdr），不是文本前缀。
 *      读侧（kshell dmesg）直接读字段过滤 / 排序，【禁止】字符串解析。
 *   3) emit 是【压死规格】的字节管道：同步、无阻塞、无格式、无分配。
 *      - 只能在【持有 sink->sink_lock】时调用；
 *      - emit 内部【禁止】再调 printk（同锁自旋死）。
 *   4) sink_lock 必须用 spinlock_interrupt_about_guard（irq-save）取。
 *      printk 会在 IRQ 上下文被调用（如 NVMe AER, NVMe_interrupts.cpp:134/156/223），
 *      裸 spinlock_cpp_t::lock() 会「线程持锁 → IRQ 里 printk → 同 CPU 自旋死」。
 *   5) panic 不走本路径：crash 现场可能正持 sink_lock，取锁即死 → panic 走 dumper 旁路。
 *   6) 一发一条：prefix 与 body 必须在同一临界区内连发，禁止跨锁拆发（会交错）。
 *
 * 关联：DmesgRingBuffer（src/include/kcirclebufflogMgr.h，本头文件先钉契约、后并实现）。
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

// ——— 文本出口：压到最死的字节管道 ———
// 「puts 指针 + io_ctx 复合体」：指针负责吐，self 携带上下文。
struct log_sink
{
    // dumb pipe：把这坨字节现在就吐给设备。同步、无阻塞、无格式。
    // 契约：调用方必须已持有 sink_lock；内部禁止调 printk。
    void (*emit)(void* self, const char* bytes, uint64_t len);
    void*           self;       // io_ctx：串口端口 / 屏幕状态 / GOP 句柄 …
    spinlock_cpp_t* sink_lock;  // 【必须非空】；vprintk 全程持锁（irq-save）
};

// ——— 纯格式化引擎：无 I/O、无静态状态、可重入、可 host 单测 ———
// 返回写入 out 的字节数（不含 NUL）；>= cap 表示发生截断。
int kvformat(char* out, uint64_t cap, const char* fmt, va_list ap);

// ——— 共享前缀渲染：[  ts] <LEVEL> ——
// ts/level 的【取值 + 文本化】只有一份，禁止各后端私造（否则同一条日志三处不一致）。
// 后端可在此之上做「样式」（颜色/省略时间戳等）——样式归后端，取值归此处。
// ts_ns==0（早期无时基）时省略时间戳段。返回写入字节数。
uint32_t render_prefix(char* out, uint64_t cap, level_t level, uint64_t ts_ns);

// ——— 核心：唯一 formatter 入口（B：level 必须到核心层，才能落二进制头）———
// 流程（临界区内）：
//   1) ts = now_ts();                       // 记录时打一次，三处共用
//   2) kvformat(body, fmt, ap);             // body 只格式化一次
//   3) dmesg_record(level, ts, body, n);    // ① 结构化落 ring
//   4) render_prefix(pfx, level, ts) → sink->emit(pfx) ; sink->emit(body)  // ② 文本出口
// sink 可为 nullptr（纯 ring 落库，无文本出口）。
void vprintk(level_t level, const log_sink* sink, const char* fmt, va_list ap);

// ——— 面层 ———
#if defined(__GNUC__)
#  define KLOG_PRINTF_ATTR(fmt_idx, arg_idx) \
        __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define KLOG_PRINTF_ATTR(fmt_idx, arg_idx)
#endif

void printk(level_t level, const char* fmt, ...) KLOG_PRINTF_ATTR(2, 3);

// ——— early：薄包装，【禁止】另起 formatter / 独立 API（early 全部 INFO）———
// 内部仍走 vprintk；多目的地（UART/GOP/…）由「fanout 文本 sink」承担——
// fanout 只是一个 sink，其 emit 按序转发给多个子 sink，不是全局 router。
void early_printk(const char* fmt, ...) KLOG_PRINTF_ATTR(1, 2);

// ——— ring：结构化记录库（不是普通 sink）———
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

// 追加一条记录（原子）；由 vprintk 调用。读者 API（cursor / 按 level 过滤）后续补。
void dmesg_record(level_t level, uint64_t ts_ns, const char* body, uint64_t len);

// ——— 缺省 sink（「三段所有权」的交接点）———
//   runtime 默认 = 文本出口 sink（无文本出口则 null，只落 ring）；
//   early 阶段 = fanout(屏, polling UART)；
//   panic 走 dumper，不经此处。
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
