#pragma once
#include <stdint.h>
#include "abi/boot.h"
#include "memory/memory_base.h"
#include "init/pages_alloc.h"   // mem_interval / phyaddr_t / basic_allocator（pure view 来源）

static_assert(sizeof(page) == 1, "page 必须恰为 1 字节：mem_map 缓冲字节数 == 条目数，穿越尺寸由它决定");

/**
 * ════════════════════════════════════════════════════════════════
 * page_allocator_v2 — init.elf 阶段"可穿越"页级物理分配器
 *
 * 定位：从历史 page_allocator（02d0b75 删除）迭代而来。init.elf 侧唯一页级分配器，
 * 分配结果直接写进 mem_map（page[] 状态数组，1B/页）；mem_map 的**最终状态**整体
 * 作为资产穿越给 kernel.elf（pages_arr）。
 *
 * 与 BCB 路线（init_bcb_juvenile + bcb_handoff 协议）的根本区别：
 *   穿越的是"物理事实"（每页在做什么），不是"分配器拓扑"（plan/位图布局/幼成年仪式）。
 *   kernel 拿到状态数组后自行决定怎么组织自己的分配器——拓扑回归各世界私有。
 *
 * 与历史 page_allocator 的差异（刻意为之）：
 *   1. 单光标自下而上 first-fit —— 不再分瞬态端/持久端，scan_top_base 删除。
 *   2. 终态不 relinquish 清零自刎，而是暴露缓冲供 phase_3b 登记 pages_arr 资产。
 *   3. pages_set 支持跨区间（旧版限单区间）——穿越后状态是权威账本，漏标即错。
 *
 * 穿越契约（最小，基于纯洁视图的唯一性）：
 *   纯洁视图（basic_allocator 的 pure_mem_view）是唯一物理内存描述表：一次生成、
 *   升序固定、Init 后不可变（pages_set 改的是 EFI 链表，不碰它）。一切区间引用都是
 *   它的下标；kernel 侧 phymem_segments 是它的逐字拷贝 → 下标两世界解析一致。
 *   - phymem_segments[]              ← 纯洁视图逐字拷贝（一等字段，原样穿越）
 *   - free_segs_descriptors_table[]  ← free_seg_descriptor_t{ in_pure_memview_idx,
 *                                      baseidx_in_memmap }（一等字段，索引式，无需重定位）
 *   - pages_arr mem 资产             ← mem_map 状态数组缓冲（vm_interval，kmmu 映射）
 *   - fpa_bitmaps mem 资产           ← 纯预留池（零状态语义；池预算 3bit×总空闲页，
 *                                      调用方 free_ram_explore + pages_set(reserved) 预留并清零）
 *   不变量：phymem_segments 与纯洁视图同序同内容（禁止过滤/重排）；
 *           free_segs_descriptors 只覆盖 freeSystemRam 子集，1:1 对应纯洁视图条目。
 *
 * 状态语义：穿越只判 free 与 非-free。init 分配占用统一 pages_set 提交 reserved，
 * 调用方如需语义态可事后覆写。kernel_persisit/kernel_anonymous 已从 page_state_t
 * 删除——不再有"持久/匿名"双态区分，账本只表达占用事实。
 *
 * 生命周期（单 BSP、无锁、单调）：
 *   phase 2    init() → 后续 alloc()/pages_set()
 *   phase 3b   把 mem_map 缓冲 + fpa_bitmaps 池登记为 mem 资产（本类只暴露 getter）
 *   phase 4.5  自裁归还：init 镜像/header/init 堆页 经 pages_set(_, free) 翻回空闲
 *              （替代旧 init_bcb_juvenile::free；"归还"在状态数组上就是翻状态）
 *   kernel     收养数组 → 状态冻结为权威账本
 * ════════════════════════════════════════════════════════════════
 */
class page_allocator_v2 {
public:
    // ═══════════════════════════════════════════════════════════
    // 生命周期
    // ═══════════════════════════════════════════════════════════
    /**
     * @brief 建立 mem_map（逻辑复刻历史 page_allocator::init）：
     *   1. 从 basic_allocator 取 pure view，仅统计 freeSystemRam 区间/总页数
     *   2. basic_allocator::pages_alloc 挖 mem_map 缓冲（total_pages × 1B，4KB 对齐）
     *   3. 堆上建 free_seg_descriptor_t[]（区间数组）
     *   4. 填区间 + page.state（free；低 1MB 段内 reserved）
     *   5. 置 scan_base 初值（首个 freeSystemRam 区间基址，≥1MB）
     *   6. dram_top = freeSystemRam 物理上界
     *   7. 自引用保护：mem_map 缓冲 / 区间数组 / init 镜像 → reserved
     *   8. 重算 free_pages
     * @return int：0 成功；负值 = 阶段失败（参数/挖缓冲/无空闲段），沿用历史 int 约定
     *               （是否升格 loc_code_t/SRC_LOC 待设计方裁决）
     */
    static int init();

    // ═══════════════════════════════════════════════════════════
    // 分配 —— 唯一分配入口，单光标自下而上 first-fit
    // ═══════════════════════════════════════════════════════════
    /**
     * @brief 分配 page_count 页连续空闲物理内存，基址对齐 2^align_log2 字节。
     *
     * 语义（替代历史双光标 probe/probe_keep 的 probe_keep）：
     *   从 scan_base 起跨区间自下而上 first-fit；状态==free 是唯一判据，
     *   对齐前跳/连续页校验在单个 free_seg_descriptor_t 内完成（区间不跨界分配）。
     *   成功后 scan_base 单调推进到 result + (page_count<<12)，不回落。
     *
     * 已标记占用（reserved 等非 free 状态）的页经 state 检查自然跳过，
     * 无需额外排除表——mem_map 即真理。
     *
     * 已知接受的行为：scan_base 以下被翻回 free 的页（phase_4.5 自裁）
     * 不会被重新扫描利用。init 阶段分配单调、末期才归还，可接受。
     *
     * @param page_count 连续空闲页数（>0）
     * @param align_log2 对齐指数 ∈ [12, 30]（<12 提升到 12）
     * @return phyaddr_t 成功返回物理基址；失败返回 0（无空闲连续段 / 未 init）
     */
    //static phyaddr_t alloc(uint64_t page_count, uint8_t align_log2 = 12);
    //勘探相应需求的内存，找到则返回物理基址
    static phyaddr_t free_ram_explore(uint64_t page_count, uint8_t align_log2 = 12);
    // ═══════════════════════════════════════════════════════════
    // 状态写入 —— mem_map 的唯一写入口
    // ═══════════════════════════════════════════════════════════
    /**
     * @brief 把 [interval.start, +size) 覆盖的页框状态置为 state 并维护 free_pages。
     *
     * 与历史契约的区别（刻意）：旧版要求区间必须完全落在单个 phyinterval_t 内，
     * 跨区间返回 -1。现改为**遍历相交区间**——因为 pure view 里 init 镜像/header/
     * loaded files 仍是 freeSystemRam 且可能跨区间，而穿越后状态是权威账本，
     * 漏标一处 kernel 就会把活页当空闲。kernel 侧 all_pages_arr::simp_pages_set
     * 已是同样语义，两侧对齐。
     *
     * "归还"也是它：pages_set(_, page_state_t::free) 即 free，不再单设 free()。
     *
     * @return int：0 成功；-1 参数非法（未对齐/零长/区间穿越内存空洞）
     */
    static int pages_set(mem_interval interval, page_state_t state);

    // ═══════════════════════════════════════════════════════════
    // 查询
    // ═══════════════════════════════════════════════════════════
    static uint64_t free_page_count();    // 当前 state==free 的页数（诊断/日志）
    static uint64_t total_page_count();   // 条目数 == 总 freeSystemRam 页数（穿越推导用）
    static phyaddr_t dram_top();          // freeSystemRam 物理上界（phase_3b 恒等窗口尺寸用）

    // ═══════════════════════════════════════════════════════════
    // 穿越视图（替代历史 relinquish_mem_map：不清零、不撕状态，只读暴露）
    // ═══════════════════════════════════════════════════════════
    /**
     * @brief mem_map 缓冲物理基址（phase_3b 登记 pages_arr mem 资产的 ppn 来源）。
     *        缓冲自身在 init() 内已 self-mark 为 reserved，穿越后 kernel 不会分配它。
     */
    static phyaddr_t get_mem_map_pbase();

private:

    // ---------- 内部状态（纯静态，无实例，单 BSP 无锁） ----------
    static page*      mem_map;               // page[] 状态数组本体（穿越资产）
    static uint64_t   mem_map_page_count;    // 条目数 == 总 freeSystemRam 页数
    static phyaddr_t  mem_map_pbase;         // 缓冲物理基址（basic_allocator 分配）
    static uint64_t   mem_map_bytes;         // = mem_map_page_count × sizeof(page)
    static free_seg_descriptor_t* mem_map_intervals; // 区间数组（升序，不穿越）
    static uint64_t       mem_map_intervals_count;
    static uint64_t       free_pages;        // 记账（pages_set 维护）
    static phyaddr_t      dram_top_addr;     // freeSystemRam 物理上界
    static phyaddr_t      scan_base;         // 单光标：自下而上，单调推进

    // ---------- 内部方法 ----------
    // 根据物理地址定位所属区间（线性扫描；区间数少，够用）
    static free_seg_descriptor_t* get_interval_by_addr(phyaddr_t addr);

    // 单区间内自下而上 first-fit（状态==free 判据 + 对齐 + 连续页校验）
    static phyaddr_t interval_bottom_to_top_ff_scan(
        free_seg_descriptor_t* iv, uint64_t page_count, uint8_t align_log2);

    // 单区间内按 [start_idx, +page_count) 条目置状态 + free_pages 记账
    //（pages_set 跨区间的内层切片）
    static int interval_set(free_seg_descriptor_t* iv, uint64_t start_idx,
                            uint64_t page_count, page_state_t state);
};
