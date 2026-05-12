#pragma once
#include <stdint.h>
#include "abi/boot.h"
#include "memory/memory_base.h"
#include "pages_alloc.h"  // mem_interval, phyaddr_t

/**
 * ============================================================================
 * page_allocator — init.elf 阶段页级物理分配器
 * ============================================================================
 *
 * basic_allocator 的正式继任者。
 *
 * 两区分配设计:
 *   "瞬态端"（可释放）从高地址向下生长;
 *   "保持端"（不可移动）从低地址向上生长。
 *   两堆向中间靠拢，交错碎片最小化。
 *
 * 扫描光标:
 *   scan_top_base   — 瞬态端光标，从最高区间地址上限向下推进
 *   scan_down_base  — 保持端光标，从 max(1MB, 首个区间 base) 向上推进
 *
 * 与 basic_allocator 的关系:
 *   basic_allocator  → EFI 描述符链表，无堆阶段的自举
 *   page_allocator   → 基于 phymem_segment 视图建立 page[] 数组
 */

class page_allocator {
public:
    // ========================================================================
    // 初始化
    // ========================================================================
    static int init();

    // ========================================================================
    // 空闲区间探针（不修改 page.state，调用者须随后 pages_set）
    // ========================================================================

    /**
     * @brief 瞬态端探针：从 scan_top_base 向下扫（默认）
     *
     * @param page_count  需求的连续页框数
     * @param align_log2  对齐指数 (12=4KB, 21=2MB, 默认 12)
     * @return phyaddr_t  物理基址；0 失败
     */
    static phyaddr_t available_meminterval_probe(uint64_t page_count, uint8_t align_log2 = 12);

    /**
     * @brief 保持端探针：从 scan_down_base 向上扫
     */
    static phyaddr_t available_meminterval_probe_keep(uint64_t page_count, uint8_t align_log2 = 12);

    // ========================================================================
    // 类型设置（唯一写 page.state 的入口）
    // ========================================================================
    static int pages_set(mem_interval interval, page_state_t state);

    // ========================================================================
    // 查询
    // ========================================================================
    static uint64_t free_page_count();
    static uint64_t total_page_count();
    static const page* get_mem_map();
    static phyaddr_t get_mem_map_pbase();

    /** @brief DRAM 物理地址上界（所有 freeSystemRam 区间的最高末尾） */
    static phyaddr_t dram_top();

    // ========================================================================
    // 自裁接口（Phase 4.5 专用）
    // ========================================================================
    static void relinquish_mem_map(phyaddr_t* out_pbase, uint64_t* out_pcount);

private:
    struct phyinterval_t {
        uint64_t base;              // 区间起始物理地址
        uint64_t numof4kbpgs;       // 区间内页框数目
        uint64_t baseidx_in_memmap; // 区间起始页框在 mem_map 中的索引
    };

    // ---------- 内部状态 ----------
    static page*     mem_map;
    static uint64_t  mem_map_page_count;
    static phyaddr_t mem_map_pbase;
    static uint64_t  mem_map_bytes;
    static phyinterval_t* mem_map_intervals;//区间数组的成员随引索上升而
    static uint64_t       mem_map_intervals_count;
    static uint64_t       free_pages;
    static phyaddr_t       dram_top_addr;

    // 扫描光标
    static phyaddr_t scan_top_base;    // 瞬态端光标：向下扫描起始（推进式递减）
    static phyaddr_t scan_down_base;   // 保持端光标：向上扫描起始（推进式递增）

    // ---------- 内部方法 ----------
    // 根据物理地址找到所属的区间描述符
    static phyinterval_t* get_interval_by_addr(phyaddr_t addr);

    // 在单个区间内的扫描算法
    static phyaddr_t interval_top_to_bottom_ff_scan(
        phyinterval_t* iv, uint64_t page_count, uint8_t align_log2);
    static phyaddr_t interval_bottom_to_top_ff_scan(
        phyinterval_t* iv, uint64_t page_count, uint8_t align_log2);

    // 设置区间描述符对应页框的状态（内部实现）
    static int interval_set(mem_interval interval, page_state_t state);
};

static_assert(sizeof(page) == 1, "struct page must be exactly 1 byte");
