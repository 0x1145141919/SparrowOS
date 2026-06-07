#pragma once
#include "AddresSpace.h"
#include "all_pages_arr.h"
#include "panic.h"

constexpr uint8_t CACHE_SLOT_COUNT = 16;

class PhyAddrAccessor {
    static vm_interval BASIC_interval;          // 底座恒等映射区间
    static vm_interval cache_tb[CACHE_SLOT_COUNT]; // stage2 扩展映射缓存
    static uint64_t   lru_tick[CACHE_SLOT_COUNT];  // LRU 最近访问时间戳 (单调递增)
    static uint64_t   access_clock;                // 全局访问时钟

    // 阶段判定
    static bool is_not_ready() { return BASIC_interval.npages == 0; }
    static bool is_stage1()    { return !is_not_ready() && GlobalKernelStatus < kernel_state::MM_READY; }
    static bool is_stage2()    { return GlobalKernelStatus >= kernel_state::MM_READY; }

    // 辅助：返回 addr 在 cache_tb 中的槽位，-1 为未命中
    static int  cache_lookup(phyaddr_t addr);
    // 辅助：LRU 选出最久未访问的受害者槽位
    static int  cache_evict_lru();
    // 辅助：访问命中时更新时间戳
    static void cache_touch(int slot);
    // 辅助：物理地址 → 可解引用虚拟地址（按阶段决策），返回 0 表示当前不可访问
    static vaddr_t resolve_addr(phyaddr_t addr);

public:
    static void Init(vm_interval basic_interval);

    static uint8_t  readu8(phyaddr_t addr);
    static uint16_t readu16(phyaddr_t addr);
    static uint32_t readu32(phyaddr_t addr);
    static uint64_t readu64(phyaddr_t addr);

    static void writeu8(phyaddr_t addr, uint8_t value);
    static void writeu16(phyaddr_t addr, uint16_t value);
    static void writeu32(phyaddr_t addr, uint32_t value);
    static void writeu64(phyaddr_t addr, uint64_t value);

    // stage1 only：无 MMU 时的粗放搬运，stage2 下返回 false
    static bool paddr_memcpy(phyaddr_t dest, phyaddr_t src, uint64_t size);

    static void cache_flush(phyaddr_t addr);
    static void cache_flush_serial(phyaddr_t addr);
};


