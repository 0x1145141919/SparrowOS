#pragma once
// ════════════════════════════════════════════════════════════════
// init_fatal — init.elf 侧的"收尸者"概念
//
// panic 语义只属于拥有持久运行时的 kernel 阶段（写遗言、冻结其它 CPU、
// 转储上下文、广播后端……）。init 是 boot 阶段，没有运行时可言：
// 出错只有一种结局——调用方已完成必要打印，这里负责"收尸"：关中断 + 停机。
//
// 这是刻意区别于 kernel Panic 的独立概念，init.elf 不得再引入 panic.h。
// ════════════════════════════════════════════════════════════════
namespace init_fatal {

[[noreturn]] inline void halt()
{
    asm volatile("cli");
    asm volatile("hlt");
    __builtin_unreachable();
}

}
