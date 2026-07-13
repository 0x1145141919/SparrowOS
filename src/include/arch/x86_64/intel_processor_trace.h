#pragma once
#include "stdint.h"
#include "memory/memory_base.h"

// ── MSR 地址常量 ──────────────────────────────────────────────────
namespace pt_msr {
    // 输出控制
    constexpr uint16_t OUTPUT_BASE       = 0x560U;  // IA32_RTIT_OUTPUT_BASE
    constexpr uint16_t OUTPUT_MASK_PTRS  = 0x561U;  // IA32_RTIT_OUTPUT_MASK_PTRS
    // 主控制 & 状态
    constexpr uint16_t CTL               = 0x570U;  // IA32_RTIT_CTL
    constexpr uint16_t STATUS            = 0x571U;  // IA32_RTIT_STATUS
    constexpr uint16_t CR3_MATCH         = 0x572U;  // IA32_RTIT_CR3_MATCH
    // 地址范围对
    constexpr uint16_t ADDR0_A           = 0x580U;  // IA32_RTIT_ADDR0_A
    constexpr uint16_t ADDR0_B           = 0x581U;  // IA32_RTIT_ADDR0_B
    constexpr uint16_t ADDR1_A           = 0x582U;  // IA32_RTIT_ADDR1_A
    constexpr uint16_t ADDR1_B           = 0x583U;  // IA32_RTIT_ADDR1_B
    constexpr uint16_t ADDR2_A           = 0x584U;  // IA32_RTIT_ADDR2_A
    constexpr uint16_t ADDR2_B           = 0x585U;  // IA32_RTIT_ADDR2_B
    constexpr uint16_t ADDR3_A           = 0x586U;  // IA32_RTIT_ADDR3_A
    constexpr uint16_t ADDR3_B           = 0x587U;  // IA32_RTIT_ADDR3_B
}

// ── IA32_RTIT_CTL 位域掩码 ─────────────────────────────────────────
namespace pt_ctl {
    constexpr uint64_t TraceEn              = 1ULL << 0;
    constexpr uint64_t CYCEn                = 1ULL << 1;
    constexpr uint64_t OS                   = 1ULL << 2;
    constexpr uint64_t User                 = 1ULL << 3;
    constexpr uint64_t PwrEvtEn             = 1ULL << 4;
    constexpr uint64_t FUPonPTW             = 1ULL << 5;
    constexpr uint64_t FabricEn             = 1ULL << 6;
    constexpr uint64_t CR3Filter            = 1ULL << 7;
    constexpr uint64_t ToPA                 = 1ULL << 8;
    constexpr uint64_t MTCEn                = 1ULL << 9;
    constexpr uint64_t TSCEn                = 1ULL << 10;
    constexpr uint64_t DisRETC              = 1ULL << 11;
    constexpr uint64_t PTWEn                = 1ULL << 12;
    constexpr uint64_t BranchEn             = 1ULL << 13;
    constexpr uint64_t MTCFreq_MASK         = 0xFULL << 14;  // bits 17:14
    constexpr uint64_t CycThresh_MASK       = 0xFULL << 19;  // bits 22:19
    constexpr uint64_t PSBFreq_MASK         = 0xFULL << 24;  // bits 27:24
    constexpr uint64_t EventEn              = 1ULL << 31;
    constexpr uint64_t ADDR0_CFG_MASK       = 0xFULL << 32;  // bits 35:32
    constexpr uint64_t ADDR1_CFG_MASK       = 0xFULL << 36;  // bits 39:36
    constexpr uint64_t ADDR2_CFG_MASK       = 0xFULL << 40;  // bits 43:40
    constexpr uint64_t ADDR3_CFG_MASK       = 0xFULL << 44;  // bits 47:44
    constexpr uint64_t DisTNT               = 1ULL << 55;
    constexpr uint64_t InjectPsbPmiOnEnable = 1ULL << 56;
}

// ── IA32_RTIT_STATUS 位域掩码 ──────────────────────────────────────
namespace pt_status {
    constexpr uint64_t FilterEn             = 1ULL << 0;  // 只读
    constexpr uint64_t ContextEn            = 1ULL << 1;  // 只读
    constexpr uint64_t TriggerEn            = 1ULL << 2;  // 只读
    constexpr uint64_t Error                = 1ULL << 4;
    constexpr uint64_t Stopped              = 1ULL << 5;
    constexpr uint64_t PendPSB              = 1ULL << 6;
    constexpr uint64_t PendToPAPMI          = 1ULL << 7;
    constexpr uint64_t PacketByteCnt_MASK   = 0x1FFFFULL << 32;  // bits 48:32
}

// ── IA32_RTIT_OUTPUT_MASK_PTRS 位域 ────────────────────────────────
namespace pt_output_mask {
    constexpr uint32_t LowerMask_7BITS      = 0x7FU;          // bits 6:0, 硬件强制 1
    constexpr uint32_t MaskOrTableOffset_SHIFT = 7;
    constexpr uint32_t OutputOffset_SHIFT   = 32;
}

// ── IA32_RTIT_CR3_MATCH 位域 ────────────────────────────────────────
namespace pt_cr3_match {
    constexpr uint64_t ADDR_MASK           = ~0x1FULL;       // bits 63:5 低 5 位忽略
}

// ── ADDRn_CFG 编码 ────────────────────────────────────────────────
namespace pt_addr_cfg {
    constexpr uint64_t Disabled            = 0ULL;  // 未使用
    constexpr uint64_t FilterEn            = 1ULL;  // [A..B] 为 FilterEn 范围
    constexpr uint64_t TraceStop           = 2ULL;  // [A..B] 为 TraceStop 范围
}

// ── 默认缓冲区大小 ──────────────────────────────────────────────
constexpr uint32_t default_PT_buffer_size = 1 << 23;  // 8 MB

// ── 从缓冲区字节数计算 OUTPUT_MASK_PTRS 的 mask 值 (bits 31:7) ─────
// mask 必须是连续 1 从 bit 7 起, buffer_size 必须是 128B 的幂
constexpr uint32_t pt_output_mask_from_size(uint32_t buf_bytes) {
    return ((buf_bytes - 1) >> 7) << 7;  // 例如 8MB → mask = 0x007FFF80
}
// ── 黑匣子状态机 ──────────────────────────────────────────────────
enum class pt_blackbox_state : uint8_t {
    not_prepared,   // 缓冲区未分配 / TCG 环境禁用
    prepared,       // 缓冲区已分配，MSR 可配，但 TraceEn=0
    running,        // 已使能，PT 正在向缓冲区写入
};

// 状态转换:
//   not_prepared ──prepare()──→ prepared ──enable()──→ running
//                     ↑                            │
//                     │        recycle()            │ disable()
//                     └──── prepared ←──────────────┘
//
//   TCG 环境: prepare() 拒绝切换, 永久 not_prepared
//   calibrate_offset(): prepared 态下调用, 不改变状态

struct pt_blackbox {
    vm_interval       ring_interval;     // 物理连续环形缓冲区
    uint32_t          tail_offset;       // 停录时的硬件 OutputOffset 快照
    pt_blackbox_state state;             // 三态
};

extern pt_blackbox* global_pt_blackboxes;  // per-CPU 数组, kinit 中 new

KURD_t prepare_blackbox(pt_blackbox *bb);
KURD_t recycle_blackbox(pt_blackbox *bb);
void   enable_blackbox(pt_blackbox *bb);
void   disable_blackbox(pt_blackbox *bb); // 清 TraceEn 并刷回 tail_offset
void   calibrate_offset(pt_blackbox *bb);