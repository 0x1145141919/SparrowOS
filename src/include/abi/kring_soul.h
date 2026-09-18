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

// ────────────────────────────────────────────────────────────────
// DmesgRing_handoff —— 环转生凭证（跨 ELF，全物理化）
//
// 设计取向（见 Init_v3_string_tag_container.md §三.2）：包外资产一律经 desc blob
// 引用【物理地址】，不做转生拷贝。环的缓冲内容与 odometer 都随内存存活，kernel
// 侧经 Kspace_phyaddr_access_window 把 obj_pbase 重链后，直接当 DmesgRingBuffer_v2*
// 读【活体】working_soul（含最新 accumulate_mileage / record_count），再回填到本
// 世界已重绑的环实例——init 跳转前后写入的记录因此连续，一条不丢。
//
// 两侧职责：
//   · init.elf：claim（对象页 + 缓冲段置 kernel_persisit）+ 登记本 desc；
//               对象在 .bss（自裁区间内）→ 由 phase_4.5 自裁时「圈出」保活。
//   · kernel.elf：phyaddr 重链 obj_pbase → 读 soul → 以本窗口 VA 重绑环。
//
// ⚠️ 尺寸契约：asset_names::ring_log 的 arg2 写死 0x20 = sizeof(下方结构)，任一
//    字段增减必须同步改名字里的十六进制串（下方 static_assert 会拦住结构侧漂移）。
// ────────────────────────────────────────────────────────────────
struct DmesgRing_handoff {
    uint64_t obj_pbase;    // 环对象（DmesgRingBuffer_v2）物理基址
    uint64_t obj_bytes;    // 对象字节数
    uint64_t buff_pbase;   // 环缓冲物理基址
    uint64_t buff_bytes;   // 环缓冲字节数
};
static_assert(sizeof(DmesgRing_handoff) == 0x20,
              "ring_log 资产名写死 0x20，改结构请同步 asset_names::ring_log");
