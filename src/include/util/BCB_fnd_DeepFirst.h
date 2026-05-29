#pragma once
#include "util/BuddyControlBlock_foundation.h"

// ════════════════════════════════════════════════════════════════
// BCB_fnd_DeepFirst
//
// 仅实现 5 个纯虚分配接口，其余继承自基类
// 保留 KURD 三阶段错误链 + 防御性校验
// ════════════════════════════════════════════════════════════════

class BCB_fnd_DeepFirst : public BuddyControlBlock_foundation {
private:
    uint64_t dfs_find_free(uint64_t idx, uint8_t target_order) const;
    KURD_t   split_internal(uint64_t idx, uint8_t order,
                            uint8_t target_order);
    KURD_t   occupy_internal(uint64_t idx, uint8_t order);
    uint8_t  coalesce_internal(uint64_t idx, uint8_t order,
                               KURD_t& kurd);

public:
    BCB_fnd_DeepFirst() = default;

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
