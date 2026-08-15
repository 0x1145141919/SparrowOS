#pragma once
#include "stdint.h"

// ════════════════════════════════════════════════════════════════
// main_phyaddr_access_window — 主窗口直映射访问（完全信任 / 高性能扁平旁路）
//
// 语义：与 Linux 直映射 __va() 同级——地址必须在主窗口 [0, dram_top) 内
//       （窗口 pbase=0 归一化），不做任何边界检查、无兜底，直接
//       `main_window_vbase + addr` 解引用。违规使用（addr ≥ dram_top、
//       窗口未就绪 main_window_vbase=0）即访问非法地址。
// 前提：main_window_vbase 在窗口建立时由 PhyAddrAccessor::Init 设置
//       （= vbase - pbase；当前窗口 pbase=0，即窗口 vbase）。
// 非 volatile 原生指针访问：语义与普通 RAM 相同、无 READ_ONCE 屏障，
//       跨 flus/invlpg 的序由调用方自行保证（需屏障的页表场景请配合
//       带 memory clobber 的 asm 或改用 volatile 指针）。
// 窗口外 / 不确定的地址 → 一律走 memory/phyaddr_accessor.h 安全层。
// ════════════════════════════════════════════════════════════════

extern uint64_t main_window_vbase;

// 物理地址 → 主窗口内虚拟地址（纯 add，无检查；完全信任 addr < dram_top）
// 也可用于基址提升：把数组基址算一次，随后以 (T*)PHYACC_VA(base) 数组遍历，
// 循环内不再每次重算主窗口基址。
#define PHYACC_VA(addr) (main_window_vbase + (uint64_t)(addr))

#define PHYACC_READU8(addr)  (*(uint8_t*)(PHYACC_VA(addr)))
#define PHYACC_READU16(addr) (*(uint16_t*)(PHYACC_VA(addr)))
#define PHYACC_READU32(addr) (*(uint32_t*)(PHYACC_VA(addr)))
#define PHYACC_READU64(addr) (*(uint64_t*)(PHYACC_VA(addr)))

#define PHYACC_WRITEU8(addr, val)  (*(uint8_t*)(PHYACC_VA(addr)) = (uint8_t)(val))
#define PHYACC_WRITEU16(addr, val) (*(uint16_t*)(PHYACC_VA(addr)) = (uint16_t)(val))
#define PHYACC_WRITEU32(addr, val) (*(uint32_t*)(PHYACC_VA(addr)) = (uint32_t)(val))
#define PHYACC_WRITEU64(addr, val) (*(uint64_t*)(PHYACC_VA(addr)) = (uint64_t)(val))
