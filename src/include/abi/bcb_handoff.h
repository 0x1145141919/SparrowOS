#pragma once
#include <stdint.h>

// ════════════════════════════════════════════════════════════════
// bcb_handoff — init.elf → kernel.elf 的 BCB 位图交接契约
//
// 这是两个世界唯一的字节级契约（KURD-free，两世界都 include）：
//   - init.elf 用独立的纯幼年 BCB 分配器产出此描述数组 + 穿越位图
//   - kernel.elf 用 BuddyControlBlock_foundation 收养（init_from_leaves）
//     并按自己的节奏执行成年仪式（fold_up_from_leaves）
//
// 位图布局（每 BCB 一块独立物理分配，全布局 3·2^N bits）：
//   [0, 2^(N+1))        非 order0 内部节点区 — 2-bit/node，init 清零后不碰
//   [2^(N+1), 3·2^N)    order0 叶子区          — 1-bit/page，init 写实
//   叶子位 1 = 空闲，0 = 占用（与 kernel foundation leaf_read 位偏移一致）
//
// 约定：交接时所有 BCB 的位图都处于幼年态（只叶子区有意义），
//       由 kernel.elf 自行决断每个 BCB 何时成年。
// ════════════════════════════════════════════════════════════════

// ── 布局常量（单一事实源：两世界据此算叶子位偏移/区大小） ──

// 叶子区起点位偏移（= 2^(N+1)），页 o 的叶子位 = 该值 + o
constexpr uint64_t bcb_leaf_bit_offset(uint8_t order) { return 1ull << (order + 1); }

// 每 BCB 全布局总位数（= 3·2^N）
constexpr uint64_t bcb_region_bits(uint8_t order)     { return 3ull << order; }

// 每 BCB 位图字节数
constexpr uint64_t bcb_region_bytes(uint8_t order)    { return bcb_region_bits(order) >> 3; }

// 叶子区起始字节偏移（= 2^(N+1)/8 = 2^(N-2)）
constexpr uint64_t bcb_leaf_byte_offset(uint8_t order){ return bcb_leaf_bit_offset(order) >> 3; }

// ── BCB 描述（唯一 plan 源：init.elf 产出 → kernel.elf 收养） ──

struct bcb_desc_t {
    uint64_t managed_base_phyaddr_base;   // 该 BCB 管理的物理内存基址（span = 2^order 页）
    uint64_t bitmap_region_base_pa;       // 该 BCB 全布局位图区的物理基址（绝对 PA，各自独立分配）
    uint8_t  order;                       // 最大 order（span = 2^order × 4KB）
};
