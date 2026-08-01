#pragma once
#include "stdint.h"
#include "memory/memory_base.h"

// ════════════════════════════════════════════════════════════════
// init_heap_v3 — init.elf 专用单堆伴侣分配器
//
// 复刻 kernel 侧 kpoolmemmgr HCB_v2.1 现行位图扫描算法（摘去 KURD）：
//   - 8B/bit flat bitmap   (vs 3×2^N buddy tree)
//   - 8B data_meta         (vs 16B buddy_meta)
//   - next-fit scan + scan_cache
//   - 0xFF canary 越界检测
//   - 无锁（单线程 boot），无多堆支持（init.elf 仅一个堆）
//
// 用法:
//   1. BSS 段定义 s_heap_data[HEAP_SIZE] + s_heap_bitmap[BITMAP_BYTES]
//   2. Phase 1 入口: g_init_heap.linktime_init(data_va, size, bitmap_va)
//   3. 之后 g_init_heap.alloc/free/realloc 立即可用
// ════════════════════════════════════════════════════════════════

class init_heap {
    // ── 常量 ──
    static constexpr uint32_t BYTES_PER_BIT = 8;   // bitmap 1 bit = 8 bytes

    // ── data_meta (8B) ──
    struct alignas(8) data_meta {
        uint32_t data_size;      // 4B, 用户请求大小; 0 = 已释放
        uint8_t  alloc_flags;    // 1B, 编码 alloc_flags_t 位域
        uint8_t  magic;          // 1B, debug 保留
        uint16_t align_pad_bits; // 2B, 对齐浪费的 bitmap bits (free 时用于归还完整 run)
    };
    static_assert(sizeof(data_meta) == 8, "data_meta must be 8 bytes");

    // ── ScanCache (next-fit 扫描缓存) ──
    struct ScanCache {
        uint64_t hint_u64_idx      = 0;  // 下一个待扫 64-bit word
        uint64_t last_success_idx  = 0;  // next-fit 起点 (bit index)
        uint64_t largest_free_hint = 0;  // 最长连续空闲 bits 缓存
        uint64_t free_bit_count    = 0;  // 总空闲 bits
        uint64_t entry_count       = 0;  // 活跃分配数
        uint64_t used_bytes_acc    = 0;  // 用户 payload 累计
    };

    // ── 状态 ──
    vaddr_t   base_         = 0;
    uint32_t  total_size_   = 0;
    uint32_t  total_bits_   = 0;
    vaddr_t   bitmap_va_    = 0;
    uint32_t  bitmap_bytes_ = 0;
    ScanCache cache_;

    // ── bitmap 操作 ──
    data_meta* meta_from_ptr(void* ptr) const;
    uint32_t   bits_needed(uint32_t user_size) const;
    uint64_t   bit_idx_from_ptr(void* ptr) const;
    bool       bitmap_test(uint64_t bit_idx) const;
    void       bitmap_set(uint64_t bit_idx, uint64_t count);
    void       bitmap_clear(uint64_t bit_idx, uint64_t count);
    bool       bitmap_scan_from(uint64_t start, uint64_t needed,
                                uint64_t& out_idx, uint64_t& out_runlen);
    void       fill_canary(uint8_t* user_ptr, uint32_t user_size,
                           uint64_t total_bits_alloced);
    void       check_canary(uint8_t* user_ptr, uint32_t user_size,
                            uint64_t total_bits_alloced);

public:
    // ── 在 BSS 堆区域上初始化 ──
    // data_va:  BSS 堆数据区虚拟地址
    // size:     堆总字节数 (8B 对齐，通常 2MB)
    // bitmap_va: BSS 位图区虚拟地址 (大小 = align_up(total_bits, 8) / 8,
    //            total_bits = size / 8)
    void linktime_init(vaddr_t data_va, uint32_t size, vaddr_t bitmap_va);

    // ── 分配器接口 ──
    void* alloc(uint32_t size, alloc_flags_t flags = default_flags);
    bool  free(void* ptr);
    void* realloc(void* ptr, uint32_t new_size, alloc_flags_t flags = default_flags);
    bool  clear(void* ptr);
    bool  is_addr_belong(void* addr) const;

    // ── 统计 ──
    uint64_t stat_alloc          = 0;
    uint64_t stat_free           = 0;
    uint64_t stat_alloc_fail     = 0;
    uint64_t stat_oom            = 0;
    uint64_t stat_realloc_expand = 0;
    uint64_t stat_scan_fail      = 0;
};

// ── 全局单例 ──
extern init_heap g_init_heap;
