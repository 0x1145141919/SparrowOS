#include "memory/phyaddr_accessor.h"
#include "util/OS_utils.h"

// 静态成员定义
vm_interval PhyAddrAccessor::BASIC_interval = {};
vm_interval PhyAddrAccessor::cache_tb[CACHE_SLOT_COUNT] = {};
uint8_t     PhyAddrAccessor::lfu_freq[CACHE_SLOT_COUNT] = {};

void PhyAddrAccessor::Init(vm_interval basic_interval)
{
    BASIC_interval = basic_interval;
    // 清空缓存槽
    for (uint32_t i = 0; i < CACHE_SLOT_COUNT; i++) {
        cache_tb[i] = {};
        lfu_freq[i] = 0;
    }
}

// ─── Cache helpers ────────────────────────────────────────

int PhyAddrAccessor::cache_lookup(phyaddr_t addr)
{
    for (uint32_t i = 0; i < CACHE_SLOT_COUNT; i++) {
        if (cache_tb[i].npages != 0 && cache_tb[i].paddr_belong(addr))
            return static_cast<int>(i);
    }
    return -1;
}

int PhyAddrAccessor::cache_evict_lfu()
{
    int victim = 0;
    for (uint32_t i = 1; i < CACHE_SLOT_COUNT; i++) {
        if (lfu_freq[i] < lfu_freq[victim])
            victim = i;
    }
    return victim;
}

void PhyAddrAccessor::cache_touch(int slot)
{
    // 频次递增，饱和到 0xFF 防溢出
    if (lfu_freq[slot] < 0xFF)
        lfu_freq[slot]++;
}

// ─── 通用访问辅助：将物理地址转换为可解引用的虚拟地址 ──────
// 返回 vaddr，对于 stage2 cache miss 返回 0
vaddr_t PhyAddrAccessor::resolve_addr(phyaddr_t addr)
{
    if (is_not_ready())
        return 0;

    // stage1 — 仅限 BASIC_interval
    if (is_stage1()) {
        return BASIC_interval.paddr_belong(addr)
                   ? BASIC_interval.vbase() + (addr - BASIC_interval.pbase())
                   : 0;
    }

    // stage2 — BASIC_interval 优先
    if (BASIC_interval.paddr_belong(addr))
        return BASIC_interval.vbase() + (addr - BASIC_interval.pbase());

    // stage2 — cache_tb 查找
    int slot = cache_lookup(addr);
    if (slot >= 0) {
        cache_touch(slot);
        return cache_tb[slot].vbase()
             + (addr - cache_tb[slot].pbase());
    }

    // stage2 — cache miss：暂未实现按需映射，返回 0
    // TODO: 按 2MB 窗口映射到 LRU 槽位后重试
    return 0;
}

// ─── Read ──────────────────────────────────────────────────

uint8_t PhyAddrAccessor::readu8(phyaddr_t addr)
{
    vaddr_t va = resolve_addr(addr);
    return va ? *(volatile uint8_t*)va : 0;
}

uint16_t PhyAddrAccessor::readu16(phyaddr_t addr)
{
    vaddr_t va = resolve_addr(addr);
    return va ? *(volatile uint16_t*)va : 0;
}

uint32_t PhyAddrAccessor::readu32(phyaddr_t addr)
{
    vaddr_t va = resolve_addr(addr);
    return va ? *(volatile uint32_t*)va : 0;
}

uint64_t PhyAddrAccessor::readu64(phyaddr_t addr)
{
    vaddr_t va = resolve_addr(addr);
    return va ? *(volatile uint64_t*)va : 0;
}

// ─── Write ─────────────────────────────────────────────────

void PhyAddrAccessor::writeu8(phyaddr_t addr, uint8_t value)
{
    vaddr_t va = resolve_addr(addr);
    if (va) *(volatile uint8_t*)va = value;
}

void PhyAddrAccessor::writeu16(phyaddr_t addr, uint16_t value)
{
    vaddr_t va = resolve_addr(addr);
    if (va) *(volatile uint16_t*)va = value;
}

void PhyAddrAccessor::writeu32(phyaddr_t addr, uint32_t value)
{
    vaddr_t va = resolve_addr(addr);
    if (va) *(volatile uint32_t*)va = value;
}

void PhyAddrAccessor::writeu64(phyaddr_t addr, uint64_t value)
{
    vaddr_t va = resolve_addr(addr);
    if (va) *(volatile uint64_t*)va = value;
}

// ─── paddr_memcpy (stage1 only) ────────────────────────────

bool PhyAddrAccessor::paddr_memcpy(phyaddr_t dest, phyaddr_t src, uint64_t size)
{
    if (size == 0) return false;
    if (!is_stage1()) return false;        // stage2 拒绝
    if (!BASIC_interval.paddr_belong(dest) || !BASIC_interval.paddr_belong(src))
        return false;
    if (!BASIC_interval.paddr_belong(dest + size - 1) || !BASIC_interval.paddr_belong(src + size - 1))
        return false;

    vaddr_t src_va  = BASIC_interval.vbase() + (src  - BASIC_interval.pbase());
    vaddr_t dest_va = BASIC_interval.vbase() + (dest - BASIC_interval.pbase());
    ksystemramcpy(reinterpret_cast<void*>(src_va),
                  reinterpret_cast<void*>(dest_va), size);
    return true;
}

// ─── Cache flush ───────────────────────────────────────────

void PhyAddrAccessor::cache_flush(phyaddr_t addr)
{
    vaddr_t va = resolve_addr(addr);
    if (va) {
        asm volatile("clflushopt (%0)" :: "r"(va) : "memory");
    }
}

void PhyAddrAccessor::cache_flush_serial(phyaddr_t addr)
{
    vaddr_t va = resolve_addr(addr);
    if (va) {
        asm volatile("clflush (%0)" :: "r"(va) : "memory");
        asm volatile("mfence" ::: "memory");
    }
}
