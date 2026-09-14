#pragma once
/**
 * ring_log.h（init.elf 薄层）—— 在共用类库 kcirclebufflogMgr.h 之上，声明 init.elf 自己的日志环。
 *
 * 为什么需要薄层：kernel.elf 与 init.elf 各自在 .bss 拥有一份 DmesgRingBuffer_v2 实例
 * （同名同型、互不干扰），共享代码不能直接摸这个符号 —— 只能经本薄层引用。
 * 实体定义在 src/init/util/ring_log.cpp。
 */
#include "kcirclebufflogMgr.h"

// init.elf 的日志环实例（.bss；绑定前 soul.buff == nullptr，写操作自动空转）
extern DmesgRingBuffer_v2 ring_log;

// 把 backing buffer 绑进环：vbase = log_buffer 映射到【本 ELF 窗口】的 VA，size = 字节数。
// （跨 ELF 转生时另经 soul 的物理描述重绑，见 v2 文档。）
void init_ring_bind(void* vbase, uint64_t size);
