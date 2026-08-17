#pragma once
#include <stdint.h>
#include <stddef.h>
#include "abi/os_error_definitions.h"

typedef uint64_t vaddr_t;

// ════════════════════════════════════════════════════════════════
// v4 伙伴系统底座 — 基类
//
// 公共数据结构与常量由基类持有，所有派生类共享
// btree_validation 为 final 非虚函数保证不变量统一
// ════════════════════════════════════════════════════════════════

class BuddyControlBlock_foundation {
public:
    static constexpr uint8_t  ORDER_COUNT = 65;
    static constexpr uint64_t INVALID_OFFSET = ~0ULL;
    static constexpr uint8_t  ERROR_MARK = 0x40;

    enum node_state_t : uint8_t {
        NODE_NONEXIST = 0b00,
        NODE_OCCUPIED = 0b01,
        NODE_NONLEAF  = 0b10,
        NODE_FREE     = 0b11,
    };

    BuddyControlBlock_foundation() = default;
    virtual ~BuddyControlBlock_foundation() = default;

    // ═══ 纯洁初始化（实函数，派生类共享：两派生类实现完全一致，收敛到基类） ═══
    // 初始化后处于"全空闲成年态"：整区位图清零 + 根 NODE_FREE + free_count[max_order]=1。
    void pure_init(vaddr_t bitmap_va, uint8_t max_order_val);

    // ═══ 纯虚分配接口（各派生类自己实现） ═══
    virtual uint64_t find_candidate(uint8_t& base_order,
                                    KURD_t& kurd) = 0;
    virtual KURD_t split(uint8_t order, uint64_t offset,
                         uint8_t target_order) = 0;
    virtual KURD_t order_occupy_try(uint8_t order,
                                    uint64_t offset) = 0;
    virtual uint8_t order_return(uint8_t order, uint64_t offset,
                                 KURD_t& kurd) = 0;
    
    void inherit_init(vaddr_t bitmap_va, uint8_t max_order_val);
    // ═══ 共享只读/校验函数（实函数，不可覆写） ═══
    bool order_exist_check(uint8_t order) const {
        return (order < ORDER_COUNT && free_count[order] > 0);
    }

    bool is_free(uint8_t order, uint64_t offset) const;
    KURD_t btree_validation();    // non-virtual, 所有派生类共享
    uint64_t get_free_count(uint8_t order) const {
        return (order < ORDER_COUNT) ? free_count[order] : 0;
    }
    uint8_t get_max_order() const { return max_order; }

    

    // 收养：叶子已在全布局位图区写实，内部节点区清零；不重扫清零，只算 free_count[0]
    void init_from_leaves(vaddr_t bitmap_va, uint8_t max_order_val);

// 成年仪式：JUVENILE → ADULT，自底向上填内部节点 + 全 free_count 重算
    tmp_error_locator fold_up_from_leaves();

    uint64_t juvenile_alloc_order0(tmp_error_locator& kurd,uint64_t acquire_count);   // 扫order0的1bit位图，找到根据acquire_count要求的连续空闲页，找到则返回偏移量，并且维护order 0的freecount
    tmp_error_locator   juvenile_free_order0(uint64_t offset,uint64_t return_count); // 修改order0位图并且维护order 0 free_count

    // ── 状态查询（包装层分流用） ──
    bool is_juvenile() const { return state == STATE_JUVENILE; }
    bool is_adult()    const { return state == STATE_ADULT; }
protected:
    enum allocator_state : uint8_t { STATE_JUVENILE = 0, STATE_ADULT = 1 };
    allocator_state state = STATE_ADULT;   // 默认成年（现有 init() 全空闲成年态兼容）
    // ─── 公共数据结构 ───
    uint64_t* bitmap          = nullptr;
    uint8_t   max_order       = 0;
    uint64_t  free_count[ORDER_COUNT] = {0};

    // ─── 位图访问（派生类共享） ───
    uint8_t  node_read(uint64_t heap_idx) const;
    void     node_write(uint64_t heap_idx, uint8_t val);
    bool     leaf_read(uint64_t leaf_idx) const;
    void     leaf_write(uint64_t leaf_idx, bool free);

    bool order0_bit_test(uint64_t idx) const;
    void order0_bit_set(uint64_t idx,bool val);

    node_state_t higher_node_get(uint64_t idx,uint8_t order) const;
    void higher_node_set(uint64_t idx,uint8_t order,node_state_t val);
    // ─── heap 索引辅助 ───
    uint8_t  heap_idx_order(uint64_t idx) const;
    uint64_t order_offset_to_idx(uint8_t order,
                                 uint64_t offset) const;
    void     idx_to_order_offset(uint64_t idx, uint8_t& order,
                                 uint64_t& offset) const;

private:
    // 仅供 btree_validation 使用
    bool validate_subtree(uint64_t idx, uint8_t order,
                          uint64_t count[]) const;
};
