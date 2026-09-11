#pragma once
/**
 * printk —— 唯一内核日志面层（formatter + sink），bsp_kout 的接替者。
 *
 * ⚠️ 本文件的接口契约已冻结方向；实现细节随讨论演化。
 *    设计讨论以本文件变更为锚点（草稿文档已删，见 commit）。
 *
 * 分层：
 *
 *   printk(level, fmt, ...)              面层：变参 + __printf 类型安全
 *     └─ vprintk(level, sink, fmt, ap)   核心：格式化 body + 组织「一条」记录
 *           └─ sink->emit(self, level, ts, body, len)   —— 一发，原子
 *                 ├─ ring_emit(self,…)  落二进制头 {len,level,seq,ts} + body
 *                 └─ text_emit(self,…)  render_prefix() + body（UART/屏/early）
 *
 * 契约（红线）：
 *   1) formatter 唯一（early / runtime / panic 共用）；无锁、无分配、无浮点。
 *   2) body 里【绝不含】前缀。level / ts 以【字段】随 emit 交出：
 *        - ring 存二进制 → 读侧（kshell dmesg）过滤 + 渲染前缀；
 *        - 文本 sink 在 emit 内自行 render_prefix()。
 *   3) 【一发一条】：禁止把 prefix 与 body 拆成两次 emit
 *        （多核 / 中断下会交错，ring 会插进别人的记录、UART 会撕行）。
 *   4) level 方向【与 Linux 相反】：复用 level_code，值越大越严重；
 *        阈值判定统一为 `level >= threshold` 才输出。
 *   5) 路由不做全局 router：多目的地 = 一个 fanout sink（组合），不是分发器。
 *
 * 关联：DmesgRingBuffer（src/include/kcirclebufflogMgr.h，待补记录头 + 读 API）。
 */
#include <cstdarg>
#include <stdint.h>
#include "abi/os_error_definitions.h"

namespace klog
{

// —— 级别：复用 abi/os_error_definitions.h 的 level_code ——
//    INVALID=0 < INFO=1 < NOTICE=2 < WARNING=3 < ERROR=4 < FATAL=5
using level_t = uint8_t;
namespace level = level_code;

// 单行 body 上限（栈缓冲；超额截断并打 "...[truncated]" 标记）。
// 对齐 Linux LOG_LINE_MAX；无动态分配。
constexpr uint32_t LOG_LINE_MAX = 1024;

// ——— sink：如何把一条日志「同步」吐出去 ———
// 即设计方所述「puts 函数指针 + io_ctx 复合体」：指针负责吐，self 携带上下文。
struct log_sink
{
    // 一发调用。body 不含前缀；level / ts_ns 为带外字段，由 sink 决定怎么用。
    void (*emit)(void* self, level_t level, uint64_t ts_ns,
                 const char* body, uint64_t len);
    void*    self;   // io_ctx：ring 句柄 / 串口端口 / 屏幕状态 …
    uint32_t flags;  // sink_flags
};

enum sink_flags : uint32_t
{
    SINK_NONE        = 0,
    SINK_TEXT_PREFIX = 1u << 0,  // 文本 sink：emit 内先 render_prefix() 再追 body
    SINK_MASK_LEVEL  = 1u << 1,  // 预留：sink 侧按自持 threshold 过滤
};

// ——— 纯格式化引擎 ———
// 无 I/O、无静态状态、可重入、可 host 单测。
// 返回写入 out 的字节数（不含 NUL）；>= cap 表示发生截断。
int kvformat(char* out, uint64_t cap, const char* fmt, va_list ap);

// ——— 共享前缀渲染 ———
// 产出 "[  ts] <LEVEL> "（ts_ns==0 时省略时间戳段，供早期无时基场景）。
// 供所有文本 sink 复用，禁止各自私造，防止多套前缀格式。
// 返回写入字节数。
uint32_t render_prefix(char* out, uint64_t cap, level_t level, uint64_t ts_ns);

// ——— 核心：唯一 formatter 入口 ——
void vprintk(level_t level, const log_sink* sink, const char* fmt, va_list ap);

// ——— 面层 ——
#if defined(__GNUC__)
#  define KLOG_PRINTF_ATTR(fmt_idx, arg_idx) \
        __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define KLOG_PRINTF_ATTR(fmt_idx, arg_idx)
#endif

void printk(level_t level, const char* fmt, ...) KLOG_PRINTF_ATTR(2, 3);

// ——— early：薄包装，【禁止】另起 formatter / 独立 API（early 全部 INFO）———
void early_printk(const char* fmt, ...) KLOG_PRINTF_ATTR(1, 2);

// ——— 缺省 sink（「三段所有权」的交接点）———
//   runtime 默认 = ring sink（单目的地）；
//   early 阶段 = fanout(ring, 屏, polling UART)；
//   panic 走 dumper，不经此处。
void            set_default_sink(const log_sink* sink);
const log_sink* get_default_sink();

}  // namespace klog

// ——— 宏层：调用点手感对齐 Linux ——
// 注意方向：我们不设 EMERG/DEBUG 命名歧义，用 FATAL 表示最严重（=5）。
#define pr_fatal(fmt, ...)  klog::printk(klog::level::FATAL,    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)    klog::printk(klog::level::ERROR,    fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   klog::printk(klog::level::WARNING,  fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) klog::printk(klog::level::NOTICE,   fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   klog::printk(klog::level::INFO,     fmt, ##__VA_ARGS__)
