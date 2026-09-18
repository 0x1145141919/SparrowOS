#pragma once

// ════════════════════════════════════════════════════════════════
// shutdown — 广播关机（cross-AP shutdown broadcast）
//
// 从 kinit.cpp 迁出（2026-09-18）：kinit 只留「内存成熟 → 跳入线程运行时」，
// 关机路径独立成件。调用方：kshell 的关机命令（firmware/uefi_kshell_commands.cpp）。
// ════════════════════════════════════════════════════════════════

// 遍历除 self 外所有 AP，逐一投递关机 IPI；50ms 硬上限，超时则跳过剩余 AP。
extern "C" void broadcast_shutdown();
