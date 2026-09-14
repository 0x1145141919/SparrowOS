// ════════════════════════════════════════════════════════════════
// ring_log.cpp（init.elf 薄层实体）—— init.elf 的 DmesgRingBuffer_v2 实例。
// .bss 定义；实体本身不进共享代码，仅经 ring_log.h 薄层暴露。
// ════════════════════════════════════════════════════════════════
#include "init/util/ring_log.h"
#include "util/OS_utils.h"   // ksetmem_8

DmesgRingBuffer_v2 ring_log;

void init_ring_bind(void* vbase, uint64_t size)
{
    // .ringlog 是 NOLOAD（不依赖 loader 补零）→ 绑定时显式清零，环从干净状态出生。
    if (vbase && size) ksetmem_8(vbase, 0, size);

    DmesgRingBuffer_soul soul{ vbase, size, 0, 0 };
    ring_log.Reincarnate(&soul);
}
