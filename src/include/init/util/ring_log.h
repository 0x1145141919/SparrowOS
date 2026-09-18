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

// 把 backing buffer 绑进环：vbase = 本 ELF 窗口内的缓冲 VA（init 侧 = .ringlog 段），
// size = 字节数。
// （跨 ELF 转生：init 侧经 "ring_log blob" 资产交 DmesgRing_handoff 物理凭证（见
//   abi/kring_soul.h），kernel 侧重链后重绑到本窗口 VA。）
void init_ring_bind(void* vbase, uint64_t size);
