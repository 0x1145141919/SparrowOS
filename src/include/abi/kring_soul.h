#pragma once
/**
 * kring_soul.h —— 「环之灵魂」：环形缓冲的跨 ELF 物理描述（ABI 层）。
 *
 * 一份 soul = 一段线性字节缓冲 + 单调里程计 + 记录水位。它【只描述环的物理身世】，
 * 不绑定任何写策略 —— 谁吃它、怎么用，各环自己定：
 *   · DmesgRingBuffer / DmesgRingBuffer_v2 —— 结构化记录（插 log_record_head_t）
 *   · debug_tmp_ring_buff                   —— 裸文本暂存（不插头）
 *
 * 跨 ELF 转生时，soul 是唯一可交给对方的凭证（两边 VA 不同 ⇒ 先带物理描述，
 * 再由上层薄层重绑到本窗口 VA）。因此它属于 ABI 层，而不是某个具体环的私有件。
 *
 * ⚠️ 本头只放数据布局，不放行为；环的算法留在各自 .cpp。
 */
#include <stdint.h>

struct DmesgRingBuffer_soul{
    void *buff;
    uint64_t buffSize;
    uint64_t accumulate_mileage;//通过accumulate_mileage与buffSize做除法，余数是圈内，下一个可写引索，商则是累计回绕次数，整个变量也可以解释为游标移动总里程（单位字节）
    uint64_t accumulate_record_count;//累计这个里面塞了多少个记录
};//转生结构体
