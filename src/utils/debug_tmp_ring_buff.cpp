// ════════════════════════════════════════════════════════════════
// debug_tmp_ring_buff.cpp — 裸文本暂存环（kernel.elf 专用）
//
// 与 DmesgRingBuffer_v2 共用 soul 描述，但写策略刻意更薄：无记录头、无 ts/level，
// 一次 print = 一段文本追加。唯一外部依赖是格式化核心 klog::vprintkv2（freestanding）。
// 锁【不在此取】—— 调用方编排（见头文件）；本 TU 不做任何加锁 / 关中断。
// ════════════════════════════════════════════════════════════════
#include "util/debug_tmp_ring_buff.h"
#include "util/vprintkv2.h"   // klog::vprintkv2 / klog::LOG_LINE_MAX
#include <cstdarg>

// ── 全局 IRQ-safe 断言 / 日志环实例（出生在 kernel_start，见 src/arch/x86_64/boot/kinit.cpp）──
// 出生前为 nullptr：调用方须判空；本 TU 不负责分配 / 绑定。
debug_tmp_ring_buff* interrupt_log_ring = nullptr;

// ── WRAITH 探针面包屑总开关（见 util/wraith_probe.h）──
// 默认 1（开）。排障中若要消融面包屑的环锁/带宽观测效应，改 1→0 重编即可
// （「命中即 panic」的断言不受此开关影响，仍全程生效）。
namespace wraith { uint32_t probe_verbose = 1; }

namespace
{
inline void ring_copy(void* dst, const void* src, uint64_t n)
{
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; ++i) d[i] = s[i];
}
}  // namespace

// ——— 出生：按值继承一份 soul（soul 为临时量也安全：环实体自持副本）———
debug_tmp_ring_buff::debug_tmp_ring_buff(DmesgRingBuffer_soul* soul)
{
    working_soul = soul ? *soul : DmesgRingBuffer_soul{ nullptr, 0, 0, 0 };
}

const DmesgRingBuffer_soul* debug_tmp_ring_buff::get_soul()
{
    return &working_soul;
}

// ——— print：fmt → 栈上 buf → 追加进环（绕尾分段拷贝）———
// 溢出 = 无情覆盖：
//   · 单条超过整环 → 只保留末尾 buffSize 字节（老内容被盖）；
//   · 正常写入越尾 → 分两段拷，最老字节被覆盖。
// 计量：mileage += 实写字节；count += 1（=「打印条数」水位，非记录定界）。
void debug_tmp_ring_buff::print(const char* fmt, ...)
{
    if (!fmt) return;
    if (working_soul.buff == nullptr || working_soul.buffSize == 0) return;

    char buf[klog::LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    uint64_t r = klog::vprintkv2(fmt, ap, buf, klog::LOG_LINE_MAX);
    va_end(ap);

    uint64_t n = (uint32_t)(r & 0xFFFFFFFFu);   // 实写字节（不含结尾 NUL）
    if (n == 0) return;                          // 空写：游标 / 计数都不动

    const char* src = buf;
    if (n > working_soul.buffSize)               // 单条超环 → 只留末尾
    {
        src += (n - working_soul.buffSize);
        n    = working_soul.buffSize;
    }

    uint64_t pos       = working_soul.accumulate_mileage % working_soul.buffSize;
    uint64_t remaining = working_soul.buffSize - pos;

    if (n <= remaining)
    {
        ring_copy((uint8_t*)working_soul.buff + pos, src, n);
    }
    else
    {
        ring_copy((uint8_t*)working_soul.buff + pos, src, remaining);
        ring_copy((uint8_t*)working_soul.buff, src + remaining, n - remaining);
    }

    working_soul.accumulate_mileage      += n;
    working_soul.accumulate_record_count += 1;   // 每次成功 print +1
}
