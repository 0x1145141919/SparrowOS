# BCB_v4 — FreePagesAllocator::BuddyControlBlock 升级记录

## 变更概览

从 BCB_v3 (mixed_bitmap_v2) 升级到 BCB_v4 (BuddyControlBlock_foundation)。

### 核心变更

| 维度 | BCB_v3 (master 9c82200) | BCB_v4 (当前) |
|------|------------------------|---------------|
| 位图编码 | heap-encoded, 1-bit/node | 非 order0 2-bit + order0 1-bit, 4-state |
| 位图大小 | 2 × 2^N bits | 3 × 2^N bits (+50%) |
| 分配算法 | BFS scan_free_block (O(2^N)) | DFS find_candidate (O(N)) |
| 分裂 | 递归 split_page + free_page_without_merge | 迭代 split_internal（底座内化） |
| 释放 | conanico_free 手动合并 | order_return 自动合并+坍缩展开 |
| 缓存验证 | 无（cache_pick 直接返回，过期条目导致 split 回退） | is_free 一次位图读验证 |
| free_count | BCB_statistics 手动维护 | 底座自动维护 |
| DESINGED_MAX_SUPPORT_ORDER | 24（数组越界 bug） | 25（修正） |

## 受影响的源文件

### src/include/memory/FreePagesAllocator.h

```
─ class mixed_bitmap_v2 : bitmap_t     (72 行, 删除)
─ mixed_bitmap_v2 bcb_bitmap            (成员, 删除)
─ conanico_free / free_page_without_merge / split_page  (声明, 删除)
─ is_reclusive_fold_success / top_fold  (声明, 删除)
─ BCB_statistics::free_count[]          (成员, 删除)
─ BCB_statistics::fold_count_success/fail (成员, 删除)

+ #include "util/BuddyControlBlock_foundation.h"
+ BuddyControlBlock_foundation fnd;      (成员, 新增)
+ DESINGED_MAX_SUPPORT_ORDER 24 → 25   (修正)
```

变化：约 _120 行替换 + 30 行新增_。

### src/memory/FreePagesAllocator_BCB.cpp

```
─ conanico_free 实现                   (~50 行, 删除)
─ free_page_without_merge 实现         (~10 行, 删除)
─ split_page 实现                      (~30 行, 递归, 删除)
─ is_reclusive_fold_success 实现       (~20 行, 删除)
─ top_fold 实现                        (~10 行, 删除)

corebcb_mixedbitmap_base_acclaim:
  bcb_bitmap.online() + free_page_without_merge
  → fnd.init()                        (重写)

allocate_buddy_way:
  旧: cache → scan_free_block → split_page → bcb_bitmap.bit_set0
  新: cache(is_free验证) → find_candidate → split → order_occupy_try
  + 缓存右兄弟链                       (重写, ~70 行)

free_buddy_way:
  旧: is_addr_belong → conanico_free → 手动 free_count
  新: is_addr_belong → order_return → 缓存折叠后块  (重写, ~20 行)

free_pages_flush:
  旧: 手动扫描位图校准 free_count
  新: fnd.btree_validation()           (简化, 5 行)

+ cache_pick 加入 is_free 验证         (新增, 8 行)
```

变化：约 _250 行替换 + 100 行新增_。

### src/memory/mixed_bitmap.cpp

清空（原为 FreePagesAllocator::BuddyControlBlock 嵌套的 mixed_bitmap_v2 实现，随类定义删除而移除）。

注：kpoolmemmgr_t 的独立 mixed_bitmap_v2 实现在 kpoolmemmgr_HCBv3.cpp 中，不受此文件影响。

## 架构变化

```
BCB_v3:
  FreePagesAllocator::BuddyControlBlock
    ├─ mixed_bitmap_v2 (内嵌类，自包含 bitmap)
    ├─ conanico_free / split_page / free_page_without_merge
    ├─ cache (24×8 无验证)
    ├─ statistics (含 free_count)
    └─ free_pages_flush (手动扫描)

BCB_v4:
  FreePagesAllocator::BuddyControlBlock
    ├─ BuddyControlBlock_foundation (共享底座)
    │   ├─ 4-state 位图
    │   ├─ free_count (自动维护)
    │   ├─ find_candidate / split / order_occupy_try / order_return
    │   └─ btree_validation
    ├─ cache (24×8, is_free 验证)
    └─ statistics (不含 free_count)
```

## 2026-05-24 修复

### can_alloc 精确阶检查 → 范围扫描

**问题：** `can_alloc(order)` 只检查 `free_count[order] > 0`，但 BCB 初始时只有
`free_count[max_order]=1`，低阶全 0。第一个 alloc（need_order=0）对所有 BCB 调用
`can_alloc(0)` 均返回 false，导致每个 BCB 被永久标记→`NO_AVALIABLE_BCB`。

```diff
 bool can_alloc(uint8_t order)
 {
     if (dirty_count != 0) return false;
-    return fnd.order_exist_check(order);
+    for (uint8_t o = order; o <= max_supprt_order; o++) {
+        if (fnd.order_exist_check(o)) return true;
+    }
+    return false;
 }
```

受影响的源文件：`src/memory/FreePagesAllocator_BCB.cpp`。

### FPA::Init 新增 BCB 信息打印

在 BCBS 构造完成后逐块打印每个 BCB 的索引、基址、order、管辖上界，
便于调试时排查 BCB 覆盖范围。

受影响的源文件：`src/memory/FreePagesAllocator.cpp`。

## 测试验证

| 测试 | 结果 |
|------|------|
| BCB_test (3 basic + 1M stress) | PASS (0 failures, 514ms) |
| BCB_foundation_test (6 basic + 1M stress) | PASS (0 failures, 428ms) |
| BCB_ORDER=24 (64GB) | PASS |
