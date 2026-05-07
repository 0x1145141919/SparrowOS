#pragma once
#include <stdint.h>
#include "abi/boot.h"
#include "memory/memory_base.h"
#include "pages_alloc.h"  // mem_interval

/**
 * ============================================================================
 * page_allocator — init.elf 阶段页级物理分配器
 * ============================================================================
 *
 * basic_allocator 的正式继任者。
 *
 * 生命周期: basic_allocator::Init() → (标记 init image/header/loaded files)
 *            → page_allocator::init() → 后续业务 → kernel.elf 接管
 *
 * 设计原则:
 *   • init() 无参数，内部通过 basic_allocator::get_pure_memory_view() 获取
 *     物理内存视图，在其上建立 page* mem_map 数组和 phyinterval_t 索引。
 *   • alloc() 使用 roving-pointer 线性扫描 mem_map，首次适配连续空闲页。
 *     不修改 page.state（仅 pages_set 可写 state）。
 *   • pages_set() 是唯一修改 page.state 的入口。
 *     当设置 freeSystemRam 时等价于释放。
 *
 * mem_map 索引空间:
 *   由 phyinterval_t 数组描述物理区间 → mem_map 索引转换关系。
 *   不是 flat 的 paddr >> 12 映射。
 *   mem_map_intervals 按物理地址升序排列，支持二分/线性查找。
 *
 * x86 低 1MB 特殊处理:
 *   即使 UEFI 报告有 DRAM 在低 1MB，所有物理地址 < 0x100000 的页
 *   均标记为 reserved，防止内核误分配 BIOS 数据区。
 *
 * 与 basic_allocator 的关系:
 *   basic_allocator → EFI 描述符链表，无堆阶段的自举
 *   page_allocator  → 基于 phymem_segment 视图建立 page 数组，堆上运行
 *   basic_allocator::get_pure_memory_view() 是两者间的移交接口
 */

class page_allocator {
public:
    // ========================================================================
    // 初始化
    // ========================================================================

    /**
     * @brief 初始化 page_allocator
     *
     * 无参数，内部与 basic_allocator 耦合。
     * init() 之前必须完成:
     *   basic_allocator::Init() 成功
     *   通过 basic_allocator::pages_set() 标记好 init image、header、loaded files
     *
     * 内部步骤:
     *   1. basic_allocator::get_pure_memory_view() 获取 phymem_segment 数组
     *   2. 扫描，计算 mem_map_page_count 和 mem_map_intervals_count
     *   3. basic_allocator::pages_alloc() 分配 page* mem_map 连续页框
     *   4. new 分配 phyinterval_t* mem_map_intervals
     *   5. 初始化 page.state:
     *      - freeSystemRam 段 → free
     *      - 其他 → reserved 或对应状态
     *      - x86 低 1MB 覆盖 → reserved
     *   6. 填充 mem_map_intervals 索引
     *   7. 自引用 pages_set 保护 mem_map / mem_map_intervals 占用的页
     *
     * @return int OS_SUCCESS(0) 成功, 负值错误码
     */
    static int init();

    // ========================================================================
    // 分配
    // ========================================================================

    /**
     * @brief 分配连续物理页框
     *
     * 从 last_scan_cursor 位置开始，roving-pointer 线性扫描 mem_map，
     * 首次适配找到连续 page_count 个 page_state_t::free 页。
     *
     * 不修改 page.state（调用者必须随后调用 pages_set 确认类型）。
     * 物理连续性受 phyinterval_t 边界约束：不会跨区间分配。
     *
     * @param page_count  需求的连续页框数
     * @param align_log2  对齐指数 (12=4KB, 21=2MB, 0=不约束, 默认 12)
     * @return phyaddr_t  分配的物理基址；0 表示分配失败
     */
    static phyaddr_t alloc(uint64_t page_count, uint8_t align_log2 = 12);

    /**
     * @brief 设置物理区间内存类型
     *
     * 唯一能修改 page.state 的入口。
     * 设置的区间可以跨越多个 phyinterval_t。
     * 当 new_type == freeSystemRam 时等价于释放。
     *
     * @param interval  物理区间 (start, size)，必须 4KB 对齐
     * @param new_type  目标内存类型
     * @return int  OS_SUCCESS(0) 成功, 负值错误码
     */
    static int pages_set(mem_interval interval, PHY_MEM_TYPE new_type);

    // ========================================================================
    // 查询
    // ========================================================================

    static uint64_t free_page_count();
    static uint64_t total_page_count();
    static const page* get_mem_map();
    static phyaddr_t get_mem_map_pbase();

    /**
     * @brief idx_to_paddr — mem_map 索引 → 物理地址
     *
     * 通过 mem_map_intervals 查找索引所属区间，计算对应物理地址。
     */
    static phyaddr_t idx_to_paddr(uint64_t idx);

    static void dump_free_regions();

private:
    // ========================================================================
    // 内部类型
    // ========================================================================

    /**
     * @brief 物理区间索引
     *
     * 对应 kernel.elf 中 all_pages_arr::phyinterval_t。
     * mem_map[baseidx_in_memmap .. baseidx_in_memmap + numof4kbpgs - 1]
     * 管理物理地址 [base, base + numof4kbpgs * 4096) 的页状态。
     */
    struct phyinterval_t {
        uint64_t base;               ///< 物理基址
        uint64_t numof4kbpgs;        ///< 连续 4KB 页数
        uint64_t baseidx_in_memmap;  ///< mem_map 中的起始索引
    };

    // ========================================================================
    // 静态成员
    // ========================================================================

    /// mem_map: 每物理页一个 page 状态字节
    static page*     mem_map;
    static uint64_t  mem_map_page_count;
    static phyaddr_t mem_map_pbase;

    /// mem_map_intervals: freeSystemRam 区间索引（堆上分配，按 base 升序）
    static phyinterval_t* mem_map_intervals;
    static uint64_t       mem_map_intervals_count;
    static uint64_t       free_pages;

    /// allocate 的 roving pointer（值 = mem_map 索引）
    static uint64_t       last_scan_cursor;

    // ========================================================================
    // 内部方法
    // ========================================================================

    /**
     * @brief paddr_to_idx — 物理地址 → mem_map 索引
     * @param[in]  paddr 物理地址（4KB 对齐）
     * @param[out] out_idx mem_map 索引
     * @return 0 成功, -1 未找到（超出管理范围）
     */
    static int paddr_to_idx(phyaddr_t paddr, uint64_t* out_idx);

    /**
     * @brief 查找索引所属 phyinterval_t
     * @param[in]  idx  mem_map 索引
     * @param[out] out_iv_idx phyinterval_t 在数组中的下标
     * @return 0 成功, -1 未找到
     */
    static int idx_to_interval(uint64_t idx, uint64_t* out_iv_idx);

    /**
     * @brief PHY_MEM_TYPE → page_state_t 转换
     */
    static page_state_t phy2page(PHY_MEM_TYPE type);

    /**
     * @brief 批量设置 page.state（已假定参数合法、区间在范围内）
     */
    static void apply_type(phyaddr_t base, uint64_t page_count, page_state_t state);
};

static_assert(sizeof(page) == 1, "struct page must be exactly 1 byte");
