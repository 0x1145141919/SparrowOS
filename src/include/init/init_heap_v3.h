#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#include "util/BuddyControlBlock_foundation.h"

// ════════════════════════════════════════════════════════════════
// init_heap_v3 — init.elf 专用单堆伴侣分配器
//
// 基于 HCB_v3（kernel 侧已百万测试）简化而来：
//   - 无锁（单线程 boot）
//   - 无多堆支持（init.elf 仅一个堆）
//   - 返回 bool/void* 替代 KURD
//   - 16B metadata (buddy_meta)，BCB 底座
//
// 用法:
//   1. BSS 段定义 s_heap_data[HEAP_SIZE] + s_heap_bitmap[BITMAP_BYTES]
//   2. Phase 1 入口: g_init_heap.linktime_init(data_va, size, bitmap_va)
//   3. 之后 g_init_heap.alloc/free/realloc 立即可用
// ════════════════════════════════════════════════════════════════

// 16B metadata — 与 kernel 侧 HCB_v3::buddy_meta 布局兼容
struct alignas(16) init_buddy_meta {
    uint32_t data_size;
    uint8_t  flags;         // bit0=force_first_linekd_heap, bit1=force_new_addr
    uint64_t magic;         // MAGIC_ALLOCATED
};
static_assert(sizeof(init_buddy_meta) == 16, "init_buddy_meta must be 16 bytes");

class init_heap {
    // ── 常量 ──
    static constexpr uint32_t BYTES_PER_ORDER0 = 32;   // 16B meta + 16B min payload
    static constexpr uint8_t  MAX_ORDER        = 16;
    static constexpr uint8_t  PER_ORDER_CACHE  = 8;
    static constexpr uint64_t MAGIC_ALLOCATED  = 0xDEADBEEFCAFEBABEull;

    // ── 成员 ──
    BuddyControlBlock_foundation fnd;
    vaddr_t  base_       = 0;
    uint32_t total_size_ = 0;
    uint64_t cache_entries_[MAX_ORDER + 1][PER_ORDER_CACHE];
    uint8_t  cache_cursor_[MAX_ORDER + 1];

    // ── 内部辅助 ──
    init_buddy_meta* meta_from_ptr(void* ptr) const;
    uint8_t  size_to_order(uint32_t size_with_meta) const;
    uint64_t ptr_to_offset(void* ptr, uint8_t order) const;

    bool internal_alloc(uint64_t& out_offset, uint8_t order);
    bool internal_free(uint64_t offset, uint8_t order);

    void cache_insert(uint8_t order, uint64_t offset);
    bool cache_pick(uint8_t order, uint64_t& out_offset);

public:
    // ── 在 BSS 堆区域上初始化 ──
    // data_va: BSS 堆数据区虚拟地址
    // size:    堆总字节数 (32B 对齐，通常 2MB)
    // bitmap_va: BSS 位图区虚拟地址 (大小 = align_up(total_bits, 64) / 8, total_bits = 3 << max_order)
    void linktime_init(vaddr_t data_va, uint32_t size, vaddr_t bitmap_va);

    // ── 分配器接口 ──
    void* alloc(uint32_t size, alloc_flags_t flags = default_flags);
    bool  free(void* ptr);
    void* realloc(void* ptr, uint32_t new_size, alloc_flags_t flags = default_flags);
    bool  clear(void* ptr);
    bool  is_addr_belong(void* addr) const;

    // ── 统计 ──
    uint64_t stat_alloc   = 0;
    uint64_t stat_free    = 0;
    uint64_t stat_alloc_fail  = 0;
    uint64_t stat_coalesce    = 0;
    uint64_t stat_split       = 0;
    uint64_t stat_cache_hit   = 0;
    uint64_t stat_scan        = 0;
};

// ── 全局单例 ──
extern init_heap g_init_heap;
