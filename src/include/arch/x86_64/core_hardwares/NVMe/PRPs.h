#pragma once
#include <stdint.h>
#include <abi/os_error_definitions.h>

struct prp_root_t {
    uint64_t prp1;
    uint64_t prp2;
    uint64_t list_head_pa;
    uint32_t list_page_count;
    uint32_t page_count;
};

struct mem_segs_t {
    uint64_t count;
    struct entry_t {
        uint64_t base;
        uint64_t nuof_4kbpgs;
    };
    entry_t* entries;
};

// ============================================================
// PRP 模板 — 形态 B：上层预建、按需填充、提交、静态析构
//
// 模板持有预分配的 PRP List 连续页（FPA 主窗口 DRAM，PHYACC_VA 硬转访问），
// 填充零分配：prp1/prp2/List entries 全部由 mem_segs 流式写出。
// I/O 热路径无任何隐式内存分配。
// ============================================================
struct PRP_template {
    uint64_t  prp1;              // fill 输出：数据页 0（1 页时 PRP2=0）
    uint64_t  prp2;              // fill 输出：数据页 1（2 页）或 PRP List 指针（≥3 页）
    uint64_t  list_head_pa;      // 预分配 PRP List 连续页基址（0 = 无 List 页）
    uint32_t  list_page_count;   // List 页容量（连续页数）
    uint32_t  mps_shift;         // 建模板时固化（设备页框大小，CC.MPS）
    uint32_t  capacity_entries;  // 可容纳最大数据页数（2 + list_page_count*(MPS/8-1)）
    uint32_t  used_entries;      // 本次 fill 实际写入数据页数（断言/调试）
};

// 根据 mem_segs 填充模板（自由函数，零分配）
//   校验：segs 有效 / 段 MPS 对齐 / 总容量 ≥ bytes / 数据页数 ≤ 模板容量
//   填充：≤2 页退化为 prp1/prp2；≥3 页 prp2 = List 指针，List 从数据页 1 起流式写
KURD_t prp_template_fill(PRP_template* tpl, const mem_segs_t& segs, uint64_t bytes);

// root_out 必须由调用方提供存储，函数填充后用于 destroy_PRP_root 清理
// （保留：基础 read/write/compare 一次性路径）
KURD_t build_PRP_root(uint64_t pbase, uint32_t page_count,
                       uint32_t mps_shift, prp_root_t* root_out, KURD_t& kurd);

KURD_t destroy_PRP_root(const prp_root_t& root,
                         uint32_t mps_shift, KURD_t& kurd);
