#pragma once
#include <stdint.h>
#include <abi/src_loc.h>
#include <memory/memory_base.h>
#include <abi/bcb_handoff.h>

struct bcb_state {
    phyaddr_t base;
    uint8_t   max_order;
    uint64_t  region_va;        // 本 BCB 位图区虚拟地址（UEFI 恒等映射，== 物理地址）
    uint64_t  free_leaves;      // = free_count[0]
    uint64_t  scan_hint_off_;   // 本 BCB 内 first-fit 游标
};
struct bcb_juvenile_init_config{
    // ── 输入视图：显式传入，不依赖 basic_allocator 隐式全局状态 ──
    phymem_segment* segs;
    uint64_t        segs_count;

    // ── strategy 链条（移植自 FreePagesAllocator::strategy_t / second_stage_init_strategy） ──
    enum init_strategy : uint8_t {
        INIT_STRATEGY_BEST_ALIGN_FIT = 0,   // 贪心最大对齐，不额外 split
        INIT_STRATEGY_MATCH_THREAD   = 1,   // 按 CPU 数 / thread_coefficient 切分
    };
    init_strategy   strategy;              // 分桶策略
    uint64_t        thread_coefficient;    // MATCH_THREAD 时有效：每线程建议主 BCB 个数

    // ── MATCH_THREAD 时有效：逻辑 CPU 数 ──
    uint64_t        logical_processor_count;

    // ── 位图池来源：池 = 3bit × 总空闲页 的一块连续物理区 ──
    //   CALLER           ：调用方预置 region_pbase_（0 = plan-only 只算 plan/预算；
    //                       非 0 = 在调用方提供的池上 full 铺叶 + 池自保护）
    //   BASIC_ALLOCATOR  ：生产路径。plan_and_setup 内部经 basic_allocator 挖池
    //                      并 pages_set 标记，一次调用即 full（KERNEL_MODE 构建）
    enum pool_source : uint8_t {
        POOL_SOURCE_CALLER            = 0,
        POOL_SOURCE_BASIC_ALLOCATOR   = 1,
    };
    pool_source     pool;

    // 预设 strategy（segs/segs_count 需调用方自行填充；池默认 CALLER 注入）
    static constexpr bcb_juvenile_init_config BEST_FIT() {
        return { nullptr, 0, INIT_STRATEGY_BEST_ALIGN_FIT, 1, 0, POOL_SOURCE_CALLER };
    }
    static constexpr bcb_juvenile_init_config DEFAULT_THREAD() {
        return { nullptr, 0, INIT_STRATEGY_MATCH_THREAD, 1, 0, POOL_SOURCE_CALLER };
    }
};

// ════════════════════════════════════════════════════════════════
// init_bcb_juvenile — 纯静态类：全静态成员，无实例。
//
// 状态就是一份全局账本（bcbs_/descs_/位图池）。位图可穿越至 kernel.elf，
// 内核端收养后自行决断每个被占叶子的命运（BFS 回收页表、归还 init 镜像等）。
// ════════════════════════════════════════════════════════════════
class init_bcb_juvenile {
public:
    static bcb_state*  bcbs_;          // 内部控制器数组（base 升序）
    static uint64_t    bcb_count_;
    static phyaddr_t   region_pbase_;  // 位图池物理基址（恒等映射）；0 = plan-only
    static uint64_t    region_size_;   // 位图池预算：3bit × 总空闲页（上取整到页）
    static uint64_t    scan_hint_bcb_; // 跨 BCB first-fit 游标
    static bcb_desc_t* descs_;         // 堆上升序描述数组，get_descs/get_desc_count 暴露

    // 生命周期：按传入 config 的纯视图（segs）算 plan → 铺叶子（全空闲）→ 活
    // 排他性（init 镜像/header/loaded files/low-1MB/位图区）由调用方标记，
    // 分配器只管理 free/occupied，不引入 reserved 语义
    static loc_code_t plan_and_setup(bcb_juvenile_init_config* config);   // 0 成功；失败返回 SRC_LOC()

    // 分配 count 页连续空闲，基址对齐 2^align_log2 字节（align_log2 ∈ [12,30]）
    static phyaddr_t alloc(uint64_t count, uint8_t align_log2, loc_code_t* err);   // 失败 0

    // 归还
    static loc_code_t free(phyaddr_t base, uint64_t count);

    // 排他性标记：把 [base, base+byte_size) 覆盖的叶子置占用。
    // 调用方在建好后标记 init 镜像/header/loaded files/低1MB 等（纯视图快照里它们
    // 仍是 freeSystemRam，plan 会覆盖它们）。分配器只管理 free/occupied。
    static loc_code_t mark_used(phyaddr_t base, uint64_t byte_size);

    // 查询
    static uint64_t free_page_count();
    static uint64_t total_page_count();

    // 交接
    static const bcb_desc_t* get_descs();   // 堆上升序描述数组
    static uint64_t get_desc_count();

private:
    // 叶子位图访问（复用 layout 常量，位偏移 = 2^(N+1) + offset）
    static bool     leaf_test(uint64_t bcb_idx, uint64_t off);
    static void     leaf_set (uint64_t bcb_idx, uint64_t off, bool free);
    // 带对齐 run 扫描：跨 BCB，物理对齐（start 物理地址 ≡ 0 mod 2^align_log2）
    static phyaddr_t scan_aligned_run(uint64_t count, uint8_t align_log2, loc_code_t* err);
    static int64_t  bcb_for_addr(phyaddr_t addr);     // 二分
};
