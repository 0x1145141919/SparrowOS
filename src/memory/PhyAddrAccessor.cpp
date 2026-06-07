#include "memory/phyaddr_accessor.h"
#include "util/OS_utils.h"

// 静态成员定义
vm_interval PhyAddrAccessor::BASIC_interval = {};
vm_interval PhyAddrAccessor::cache_tb[CACHE_SLOT_COUNT] = {};
uint64_t    PhyAddrAccessor::lru_tick[CACHE_SLOT_COUNT] = {};
uint64_t    PhyAddrAccessor::access_clock = 0;

void PhyAddrAccessor::Init(vm_interval basic_interval)
{
    BASIC_interval = basic_interval;
    // 清空缓存槽
    for (uint32_t i = 0; i < CACHE_SLOT_COUNT; i++) {
        cache_tb[i] = {};
        lru_tick[i] = 0;
    }
    access_clock = 0;
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

int PhyAddrAccessor::cache_evict_lru()
{
    int victim = 0;
    for (uint32_t i = 1; i < CACHE_SLOT_COUNT; i++) {
        if (lru_tick[i] < lru_tick[victim])
            victim = i;
    }
    return victim;
}

void PhyAddrAccessor::cache_touch(int slot)
{
    lru_tick[slot] = ++access_clock;
}

// ─── 通用访问辅助：将物理地址转换为可解引用的虚拟地址 ──────
// 返回 vaddr，对于 stage2 cache miss 返回 0
constexpr uint32_t _1GB_SIZE = 0x40000000;
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

    // stage2 — cache miss：逐出 LRU 槽 → 分配并映射 1GB UC_WR 窗口
    {
        phyaddr_t phybase = addr & ~(phyaddr_t)(_1GB_SIZE - 1);
        int slot = cache_evict_lru();

        // 逐出现有条目（Kspace_phyaddr_direct_unmap 包含 vm_table remove + 清 PTE + TLB shootdown）
        if (cache_tb[slot].npages != 0) {
            Kspace_phyaddr_direct_unmap(cache_tb[slot]);
            cache_tb[slot] = {};
        }

        // 分配 + 映射 1GB 窗口（vm_table insert + _4lv_pdpte_1GB_entries_set）
        vm_interval new_interval = {
            .vpn    = 0,                    // 0 = 自动分配虚拟地址
            .ppn    = phybase >> 12,
            .npages = _1GB_SIZE >> 12,
            .access = KSPACE_RW_UC_ACCESS,
        };
        KURD_t kurd;
        vaddr_t vbase = Kspace_pinterval_alloc_and_map(new_interval, &kurd);
        if (vbase == 0 || error_kurd(kurd)) [[unlikely]]
            return 0;

        // 更新缓存条目
        cache_tb[slot] = {
            .vpn    = vbase >> 12,
            .ppn    = phybase >> 12,
            .npages = _1GB_SIZE >> 12,
            .access = KSPACE_RW_UC_ACCESS,
        };
        cache_touch(slot);

        return vbase + (addr - phybase);
    }
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
