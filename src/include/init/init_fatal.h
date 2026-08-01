#pragma once
// ════════════════════════════════════════════════════════════════
// init_fatal — init.elf 侧的"收尸者"概念
//
// panic 语义只属于拥有持久运行时的 kernel 阶段（写遗言、冻结其它 CPU、
// 转储上下文、广播后端……）。init 是 boot 阶段，没有运行时可言：
// 出错只有一种结局——调用方已完成必要打印，这里负责"收尸"。
//
// 这是刻意区别于 kernel Panic 的独立概念，init.elf 不得再引入 panic.h。
//
// 定位工具语义（迭代开发期）：
//   出错点用 SRC_LOC() 编码 (路径 hash, 行号) 成一个 u64 魔法值，
//   halt(loc) 尽力经 UART(COM1) 直写 "FATAL LOC=0x<hex>"，再 cli+hlt。
//   开发者在宿主端用 build_utils/src_loc_decode.py 还原 path:line。
//   decode 表 srcloc.json 是纯宿主产物，绝不进入 initramfs / 映像。
// ════════════════════════════════════════════════════════════════
#include "abi/src_loc.h"

namespace init_fatal {

// 收尸：尽力打印位置魔法值 → 关中断 + 停机
[[noreturn]] void halt(loc_code_t loc = 0);

// 纯停机兜底（无位置上下文时的最终手段）
[[noreturn]] inline void halt_raw()
{
    asm volatile("cli");
    asm volatile("hlt");
    __builtin_unreachable();
}

}
