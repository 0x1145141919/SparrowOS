#pragma once
#include <stdint.h>
#include <stddef.h>
#include "abi/os_error_definitions.h"

// vaddr_t — 内核虚拟地址类型，在 memory_base.h 中定义
// 这里只做前向声明以避免引入过多依赖
typedef uint64_t vaddr_t;
namespace infrastructure_location_code{
    constexpr uint8_t LOCATION_CODE_BCB_FOUNDATION = 1;
    namespace BCB_foundation{
        namespace common_fatal_reasons{//上界0x20
            static constexpr uint8_t BTREE_VIOLATION = 0x1;
            static constexpr uint8_t FREE_COUNT_VIOLAITON = 0x2;
        };
        namespace common_fail_reasons{//上界0x20
            static constexpr uint8_t ORDER_OUT_OF_RANGE = 0x1;
            static constexpr uint8_t TARGET_NOT_FREE = 0x2;
        }
        constexpr uint8_t INIT=0;//这个函数一般要进行重写
        constexpr uint8_t FIND_CANDIDATE=1;
        namespace FIND_CANDIDATE_RESULTS::fail_reasons{ 
            static constexpr uint8_t no_more_candidate = 0x20;
        }
        constexpr uint8_t SPLIT=2;
        namespace SPLIT_INTERVAL_RESULTS::fail_reasons{ 
            static constexpr uint8_t TARGET_NOT_FREE = 0x20;
        }
        constexpr uint8_t OCCUPY_TRY=3;
        namespace OCCUPY_TRY_RESULTS::fail_reasons{ 
            static constexpr uint8_t TARGET_NOT_FREE = 0x20;
        }
        constexpr uint8_t ORDER_RETURN=4;
        constexpr uint8_t TREE_VALIDATION=5;
    }
}
// ════════════════════════════════════════════════════════════════
// v4 伙伴系统底座 — BuddyControlBlock_foundation
//
// 比 v2 (mixed_bitmap_v2) 的改进：
//   - 非 order0 节点每节点 2bit → 4 状态编码 (00/01/10/11)
//   - order0 叶子每节点 1bit
//   - 总位图开销 3×2^N bits（vs v2 的 2×2^N bits）
//   - DFS 分配而非线性 BFS → O(N) vs O(2^N) 位图访问
//   - free_count 由底座内蕴维护
//
// 详细设计见 Docs/Memory/BCB_foundation_v4draft.txt
// ════════════════════════════════════════════════════════════════

class BuddyControlBlock_foundation {
public:
    static constexpr uint8_t  ORDER_COUNT = 65;
    static constexpr uint64_t INVALID_OFFSET = ~0ULL;
    static constexpr uint8_t  ERROR_MARK = 0x40;  // base_order 错误编码起始

    // ─── 非 order0 节点 (2-bit) 状态 ───
    enum node_state_t : uint8_t {
        NODE_NONEXIST = 0b00,  // 不存在（从未分裂至此粒度）
        NODE_OCCUPIED = 0b01,  // 存在且为占用叶
        NODE_NONLEAF  = 0b10,  // 存在且为非叶节点（子树有可用）
        NODE_FREE     = 0b11,  // 存在且为空闲叶（整块空闲）
    };

private:
    // ─── 位图存储 ───
    uint64_t* bitmap                        = nullptr;

    uint8_t  max_order;                                           // N
    uint64_t free_count[ORDER_COUNT];             // 各 order 空闲块数

    // ─── 底层位图访问 ───
    uint8_t  node_read(uint64_t heap_idx) const;
    void     node_write(uint64_t heap_idx, uint8_t val);
    bool     leaf_read(uint64_t leaf_idx) const;     // leaf_idx ∈ [2^N, 2^(N+1))
    void     leaf_write(uint64_t leaf_idx, bool free);

    // ─── heap 索引辅助 ───
    uint8_t  heap_idx_order(uint64_t idx) const;
    uint64_t order_offset_to_idx(uint8_t order, uint64_t offset) const;
    void     idx_to_order_offset(uint64_t idx, uint8_t& order,
                                 uint64_t& offset) const;

    // ─── 内部实现 ───
    uint64_t dfs_find_free(uint64_t idx, uint8_t target_order) const;
    KURD_t   split_internal(uint64_t idx, uint8_t order,
                            uint8_t target_order);
    KURD_t   occupy_internal(uint64_t idx, uint8_t order);
    uint8_t  coalesce_internal(uint64_t idx, uint8_t order, KURD_t& kurd);
    // btree_validation 内部递归：校验子树合规性 + 计数空闲块
    // 返回 true=合规, false=违规
    bool     validate_subtree(uint64_t idx, uint8_t order,
                              uint64_t count[]) const;

public:
    BuddyControlBlock_foundation() = default;

    // ─── 初始化 ───
    // bitmap_va: 外部提供缓冲区，大小至少 3·2^max_order bits
    void init(vaddr_t bitmap_va, uint8_t max_order_val);

    // ─── 接口（文档六章）───
    // find_candidate: DFS 只读，找到 ≥ base_order 的空闲叶
    //   返回 offset，base_order 输出实际找到的 order
    //   失败时 base_order 编码为 0x40+ 错误
    uint64_t find_candidate(uint8_t& base_order, KURD_t& kurd);

    // split: 将一个空闲叶分裂到 target_order
    //   右保留空闲（可供缓存），左路递归缩小 order
    KURD_t split(uint8_t order, uint64_t offset, uint8_t target_order);

    // order_occupy_try: 0b11 → 0b01（或 leaf 1→0），不重入安全
    //   成功返回空 KURD(SUCCESS)，失败返回含原因 KURD
    KURD_t order_occupy_try(uint8_t order, uint64_t offset);

    // order_return: 归还 + 向上折叠，返回最终 order
    //   错误时返回 [0x40..0xFF]，调用方用 is_error_kurd 判断
    uint8_t order_return(uint8_t order, uint64_t offset,KURD_t&kurd);

    // order_exist_check: O(1) free_count 预判
    bool order_exist_check(uint8_t order) const {
        return (order < ORDER_COUNT &&
                free_count[order] > 0);
    }

    // is_free: 快速检查 (order,offset) 处是否为空闲叶节点（一次位图读）
    bool is_free(uint8_t order, uint64_t offset) const;

    // btree_validation: 从根遍历完整校验二叉树合规性 + free_count 一致性
    //   规则1: NODE_OCCUPIED/NODE_NONEXIST/NODE_FREE 节点的子树
    //          所有非 order0 后代必须 NODE_NONEXIST, order0 后代必须 0
    //   规则2: NODE_NONLEAF 节点的左右孩子不能有 NODE_NONEXIST
    //          递归到 order=1 停止
    //   校验后执行 free_count 比较
    KURD_t btree_validation();

    // 获取 raw free_count（调试）
    uint64_t get_free_count(uint8_t order) const {
        return (order < ORDER_COUNT)
               ? free_count[order] : 0;
    }
    uint8_t get_max_order() const { return max_order; }
};
