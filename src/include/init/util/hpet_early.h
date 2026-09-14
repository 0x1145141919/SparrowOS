#pragma once
#include "abi/boot.h"

// ── init.elf 摸黑时基（参考 kernel 侧 HPET_driver / time.cpp）──
//
// phase1（init_io_and_heap）里调用：此时 CR3 已是全物理恒等映射（init_entry.asm），
// 可直接按物理地址解引用 ACPI 表与 HPET MMIO，无需 KMMU。
//   gST → ACPI20 GUID → RSDP → XSDT → 签名 "HPET" → Base_Address
//   → modify_access 把该页改成 UC（恒等映射 VA==PA）
//   → 照 HPET_driver::Init 时序启主计数器
//
// 成功：now_ts_us() 返回自使能以来的微秒数。
// 失败：无 HPET 表 / counter_clk_period==0 → 【静默失败】，now_ts_us() 恒返回 0。
void init_hpet_early(BootInfoHeader* header);
