#pragma once
#include "util/BuddyControlBlock_foundation.h"

// ════════════════════════════════════════════════════════════════
// BCB_fnd_ShallowFirst
//
// 仅实现 5 个纯虚分配接口，其余继承自基类
// 无防御性校验，无 btree_validation（从基类继承 final 版本）
// ════════════════════════════════════════════════════════════════

class BCB_fnd_ShallowFirst : public BuddyControlBlock_foundation {
private:
    uint64_t dfs_find_free(uint64_t idx, uint8_t target_order) const;
    void     split_internal(uint64_t idx, uint8_t order,
                            uint8_t target_order);
    void     occupy_internal(uint64_t idx, uint8_t order);
    uint8_t  coalesce_internal(uint64_t idx, uint8_t order);

public:
    BCB_fnd_ShallowFirst() = default;

    void init(vaddr_t bitmap_va, uint8_t max_order_val) override;
    uint64_t find_candidate(uint8_t& base_order,
                            KURD_t& kurd) override;
    KURD_t split(uint8_t order, uint64_t offset,
                 uint8_t target_order) override;
    KURD_t order_occupy_try(uint8_t order,
                            uint64_t offset) override;
    uint8_t order_return(uint8_t order, uint64_t offset,
                         KURD_t& kurd) override;
};
