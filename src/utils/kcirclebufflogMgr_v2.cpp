// ════════════════════════════════════════════════════════════════
// kcirclebufflogMgr_v2.cpp — DmesgRingBuffer_v2（kernel.elf / init.elf 共用资产）
//
// 与 v1（kcirclebufflogMgr.cpp，静态成员、kernel-only）并存；本 TU 只实现 v2。
// 零「侧向」依赖：只用 stdint + 头里的类；唯一外部符号是 now_ts_us()（谁接入谁提供）。
// 锁【不属于本类】：调用方自持锁调用（init 单线程不加锁；kernel 走 irq-save）。
//
// 环模型：
//   · 底层是一段线性字节缓冲（buff，容量 buffSize），单里程计 accumulate_mileage
//     单调递增 = 自出生以来累计追加字节数。
//        圈内写点 = accumulate_mileage % buffSize
//        回绕圈数 = accumulate_mileage / buffSize
//     记录数 accumulate_record_count（同时用作单调 seq）。
//   · 一条记录 = [log_record_head_t(17B, 基址 8B 对齐)][body(len B)]；
//     record_add 只写头并回传 {ts_us, record_seq}；body 由紧随的 putsk 写。
//
// ⚠️ 跨 ELF 转生：soul 必须带【物理描述】才能交给对方（两边 VA 不同）。
//    本 TU 只吃 soul.buff 原样；物理化 / VA 重绑由上层薄层负责。
// ════════════════════════════════════════════════════════════════
#include "kcirclebufflogMgr.h"

namespace
{
inline void ring_copy(void* dst, const void* src, uint64_t n)
{
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; ++i) d[i] = s[i];
}

inline uint64_t align_up8(uint64_t v) { return (v + 7ull) & ~7ull; }
}

// ——— 转生：内容原地完整继承，odometer 随行 ——
void DmesgRingBuffer_v2::Reincarnate(DmesgRingBuffer_soul* soul)
{
    if (!soul) return;
    working_soul = *soul;
}

const DmesgRingBuffer_soul* DmesgRingBuffer_v2::get_soul()
{
    return &working_soul;
}

// ——— record_add：插入记录头（8B 对齐，可绕尾），返回压缩身份 {ts_us, record_seq} ——
// 语义：
//   1) 把记录基址上对齐到 8（跳过不足 8 的残余，残余内容无意义）；
//   2) 若基址 + sizeof(head) 越尾 → 跳到下一圈圈首（mileage 补到 buffSize 的整数倍）；
//   3) 写头（含 magic 锚点），mileage += 17，record_count++（=record_seq）；
//   4) 返回 {now_ts_us(), record_seq} 供多后端共享同一 ts/seq。
log_record_head_compressed DmesgRingBuffer_v2::record_add(uint16_t len, uint8_t level)
{
    log_record_head_compressed out{0, 0};
    if (working_soul.buff == nullptr || working_soul.buffSize == 0) return out;

    // 1) 记录基址 8B 对齐（假设 buffSize 为 8 的倍数，则换圈后仍对齐）
    working_soul.accumulate_mileage = align_up8(working_soul.accumulate_mileage);
    uint64_t pos = working_soul.accumulate_mileage % working_soul.buffSize;

    // 2) 头放不下 → 绕到尾后圈首
    if (pos + sizeof(log_record_head_t) > working_soul.buffSize)
    {
        working_soul.accumulate_mileage += (working_soul.buffSize - pos);
        pos = 0;
    }

    // 3) 组装并落头
    uint32_t seq = (uint32_t)(working_soul.accumulate_record_count + 1);
    uint64_t ts  = now_ts_us();

    log_record_head_t head;
    head.len        = len;
    head.level      = level;
    head.magic1     = LOG_REC_MAGIC1;
    head.record_seq = seq;
    head.ts_us      = ts;
    head.magic2     = LOG_REC_MAGIC2;

    ring_copy((uint8_t*)working_soul.buff + pos, &head, sizeof(head));
    working_soul.accumulate_mileage      += sizeof(log_record_head_t);
    working_soul.accumulate_record_count += 1;

    out.ts_us      = ts;
    out.record_seq = seq;
    return out;
}

// ——— putsk：从当前写点落 body ——
// 语义：越尾则分两段拷贝（环绕）；len 超过整环则保留末尾 buffSize 字节；mileage += 实写字节。
void DmesgRingBuffer_v2::putsk(char* str, uint16_t len_in_bytes)
{
    if (str == nullptr || len_in_bytes == 0) return;
    if (working_soul.buff == nullptr || working_soul.buffSize == 0) return;

    uint64_t n = len_in_bytes;
    if (n > working_soul.buffSize)
    {
        str += (n - working_soul.buffSize);
        n = working_soul.buffSize;
    }

    uint64_t pos       = working_soul.accumulate_mileage % working_soul.buffSize;
    uint64_t remaining = working_soul.buffSize - pos;

    if (n <= remaining)
    {
        ring_copy((uint8_t*)working_soul.buff + pos, str, n);
    }
    else
    {
        ring_copy((uint8_t*)working_soul.buff + pos, str, remaining);
        ring_copy((uint8_t*)working_soul.buff, str + remaining, n - remaining);
    }
    working_soul.accumulate_mileage += n;
}
