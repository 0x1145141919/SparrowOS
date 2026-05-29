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

    // ═══ 纯虚分配接口（各派生类自己实现） ═══
    virtual void init(vaddr_t bitmap_va, uint8_t max_order_val) = 0;
    virtual uint64_t find_candidate(uint8_t& base_order,
                                    KURD_t& kurd) = 0;
    virtual KURD_t split(uint8_t order, uint64_t offset,
                         uint8_t target_order) = 0;
    virtual KURD_t order_occupy_try(uint8_t order,
                                    uint64_t offset) = 0;
    virtual uint8_t order_return(uint8_t order, uint64_t offset,
                                 KURD_t& kurd) = 0;

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

protected:
    // ─── 公共数据结构 ───
    uint64_t* bitmap          = nullptr;
    uint8_t   max_order       = 0;
    uint64_t  free_count[ORDER_COUNT] = {0};

    // ─── 位图访问（派生类共享） ───
    uint8_t  node_read(uint64_t heap_idx) const;
    void     node_write(uint64_t heap_idx, uint8_t val);
    bool     leaf_read(uint64_t leaf_idx) const;
    void     leaf_write(uint64_t leaf_idx, bool free);

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
