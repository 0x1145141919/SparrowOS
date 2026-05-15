# HCB_v4 — kpoolmemmgr_t::HCB_v3 升级记录

## 变更概览

从 HCB_v3 (独立的 mixed_bitmap_v2) 升级到 HCB_v4 (BuddyControlBlock_foundation)。

### 核心变更

| 维度 | HCB_v3 (master 9c82200) | HCB_v4 (当前) |
|------|------------------------|---------------|
| 位图编码 | heap-encoded, 1-bit/node | 非 order0 2-bit + order0 1-bit, 4-state |
| 位图大小 | 2 × 2^N bits | 3 × 2^N bits (+50%) |
| 分配算法 | cache → BFS scan_free_block | cache → DFS find_candidate |
| 释放 | internal_split + free_page_without_merge + conanico_free | order_return + 自动坍缩展开 |
| free_count | HCB 成员数组手动维护 | 底座自动维护 |
| 缓存验证 | bcb_bitmap.bit_get（未区分占用/折叠失效） | fnd.is_free（单次位图读） |
| **BUG 修复** | 地址公式 `offset * 32` 不随 order 缩放 | `(offset << order) * 32` |

## BUG 修复：地址计算

这是原 HCB_v3 (master 9c82200) 就存在的 bug，被 v4 移植连带修复。

### alloc 侧

```cpp
// 旧 (v3) — 错误
addr = vbase_ + offset * BYTES_PER_ORDER0 + sizeof(buddy_meta);
// 例: order=3, offset=1 → vbase_ + 32 + 16 = vbase_ + 48
//     但 order-3 块应为 256 字节，offset=1 → vbase_ + 272

// 新 (v4) — 正确
addr = vbase_ + (offset << order) * BYTES_PER_ORDER0 + sizeof(buddy_meta);
// 例: order=3, offset=1 → vbase_ + (1<<3)*32 + 16 = vbase_ + 272 ✓
```

### free 侧

```cpp
// 旧 — ptr_to_offset 无 order 参数，返回 flat byte offset
uint64_t ptr_to_offset(void* ptr) const {
    return ((uint64_t)ptr - 16 - vbase_) / 32;  // 总是除以 32
}

// 新 — 接受 order 参数，按块大小缩放
uint64_t ptr_to_offset(void* ptr, uint8_t order) const {
    return ((uint64_t)ptr - 16 - vbase_) >> (order + 5);  // ÷ (32 << order)
}
```

### 影响

v3 下所有 cache 命中的分配（不同 size）全返回同一地址。测试"通过"是因为：
- 多次分配覆写同一 `buddy_meta`，`magic` 字段被后续 `MAGIC_ALLOCATED` 覆盖后仍然匹配
- `free` 检查 `meta->magic == MAGIC_ALLOCATED` ⇒ 通过 ⇒ 释放错误地址 ⇒ 后续校验不触发
- 实际上是 **隐蔽的数据损坏**

v4 修复后，不同 size 的 alloc 返回不同地址，`flush` 校验通过。实测验证了 11 种不同 size 的分配+释放均正确。

## 受影响的源文件

### src/include/memory/kpoolmemmgr.h

```
─ class mixed_bitmap_v2 : bitmap_t     (40 行, 删除)
─ mixed_bitmap_v2 bcb_bitmap            (成员, 删除)
─ internal_split / free_page_without_merge  (声明, 删除)
─ order_free_count[MAX_ORDER+1]        (成员, 删除)

+ #include "util/BuddyControlBlock_foundation.h"
+ BuddyControlBlock_foundation fnd;      (成员, 新增)
+ bitmap_pbase / bitmap_allocated_size  (成员, 从原 mixed_bitmap_v2 移植)
+ ptr_to_offset 签名: 增加 order 参数   (修改)
```

变化：约 _70 行替换 + 20 行新增_。

### src/memory/kpoolmemmgr_HCBv3.cpp

```
─ idx_from_order_offset / order_offset_from_idx / u64_scan_interval  (~30 行, 删除)
─ 全部 mixed_bitmap_v2:: 方法         (~90 行, 删除)
─ internal_split / free_page_without_merge  (~50 行, 删除)
─ scan_free_block                      (~15 行, 删除)

linktime_init / test_init / online:
  bcb_bitmap.init_existing() / online() → fnd.init()  (替换, 各处均改)

internal_alloc:
  旧: cache → scan_free_block → bit_set0 → 手动 order_free_count
  新: cache(is_free) → find_candidate → split → order_occupy_try
  + 缓存右兄弟链                       (重写, ~60 行)

internal_free:
  旧: conanico_free 手动合并
  新: fnd.order_return (自动合并+坍缩展开)  (重写, ~15 行)

flush_free_count:
  旧: 手动扫描位图
  新: fnd.btree_validation()           (简化, 5 行)

alloc 函数:
  addr 公式: offset*32 → (offset<<order)*32  (修复, 1 行)

cache_pick:
  加入 fnd.is_free 验证                 (修改, 8 行)

offline:
  bcb_bitmap.offline() → 直接清理 bitmap_pbase  (修改, ~25 行)
```

变化：约 _200 行替换 + 80 行新增_。

## 架构变化

```
HCB_v3:
  kpoolmemmgr_t::HCB_v3
    ├─ mixed_bitmap_v2 (独立的 bitmap 实现)
    ├─ internal_split / free_page_without_merge
    ├─ BuddyCache (简单环形缓冲)
    ├─ order_free_count[MAX_ORDER+1] (手动维护)
    └─ flush_free_count (手动扫描)

HCB_v4:
  kpoolmemmgr_t::HCB_v3 (同一种)
    ├─ BuddyControlBlock_foundation (共享底座)
    │   ├─ 4-state 位图
    │   ├─ free_count (自动维护)
    │   ├─ find_candidate / split / order_occupy_try / order_return
    │   └─ btree_validation
    ├─ BuddyCache (保留, is_free 验证)
    └─ order_free_count 已移除 (底座替代)
```

## 测试验证

| 测试 | 结果 |
|------|------|
| test_various_sizes（11 种 size） | PASS |
| test_realloc / test_clear | PASS |
| Stress 1M cycles | PASS (489K alloc, 484K free, 222ms) |
| btree_validation ×10 | ALL CLEAN |

## 与 BCB_v4 的关系

BCB_v4 和 HCB_v4 共用同一个 `BuddyControlBlock_foundation` 底座，但使用场景不同：

| 维度 | BCB_v4 (FreePagesAllocator) | HCB_v4 (kpoolmemmgr) |
|------|---------------------------|----------------------|
| 管理对象 | 物理页框 (4KB) | 堆内小块 (32B order-0) |
| max_order 典型值 | 16~24 | 16 |
| 缓存 | 24×8 多槽, 带 LRU 游标 | 每 order 8 项环形缓冲 |
| 上层建筑 | all_pages_arr, KspacePageTable | buddy_meta 元数据, alloc_flags, realloc |
