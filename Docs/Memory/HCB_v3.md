# kpoolmemmgr_t::HCB_v3 文档

## 1. 概述

`HCB_v3`（Heap Control Block v3）是 kpoolmemmgr_t 的内核堆管理器实现，使用基于混合位图（mixed_bitmap_v2）的二进制伙伴分配器（binary buddy allocator）。

### 1.1 设计目标

- **BCB 理念移植**: 将 FreePagesAllocator::BuddyControlBlock 的 BCB（Buddy Control Block）算法移植到堆管理场景
- **连续管理**: 管理连续的虚拟内存区间，以 32B（BYTES_PER_ORDER0）为最小粒度
- **缓存加速**: 每个 order 维护一个固定大小缓存（PER_ORDER_CACHE_COUNT=8），cache-first-then-scan
- **多堆架构**: 通过 kpoolmemmgr_t 的 multi_heap_enable 支持 per-processor 堆

### 1.2 参数

| 参数 | 值 | 说明 |
|------|-----|------|
| HCB_DEFAULT_SIZE | 2MB | 每个 HCB 管理的堆大小 |
| BYTES_PER_ORDER0 | 32B | Order 0 块大小（含 16B meta + 16B payload） |
| MAX_ORDER | 16 | 最大 order（2MB → 32B 共 16 级） |
| PER_ORDER_CACHE_COUNT | 8 | 每 order 缓存条目数 |
| PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2 | 4 | 每 CPU 专用堆数 2^4 = 16 |

## 2. 数据结构

### 2.1 HCB_v3

```cpp
class HCB_v3 {
public:
    bool valid = false;

    // ─── 统计 (public) ───
    uint64_t order_free_count[MAX_ORDER + 1];  // 各 order 当前空闲块数
    uint64_t stat_alloc, stat_free, stat_alloc_fail;
    uint64_t stat_coalesce, stat_split;
    uint64_t stat_cache_hit, stat_scan;

private:
    mixed_bitmap_v2 bcb_bitmap;             // 二叉树位图
    BuddyCache      caches_[MAX_ORDER + 1]; // 每 order 缓存
    vaddr_t         vbase_;                 // 堆虚拟基址
    phyaddr_t       data_pbase;             // 物理基址（仅内核）
    uint32_t        total_size_;            // 堆总大小
    uint8_t         max_order_;             // 实际 max order
    spintrylock_cpp_t hcb_lock;             // 外部自旋锁
};
```

### 2.2 buddy_meta

每个分配块的元数据结构（16B，位于块起始处，对齐到 16B）：

```cpp
struct alignas(16) buddy_meta {
    uint32_t data_size;        // 用户请求的数据大小
    uint8_t  flags;            // alloc_flags_t 压缩
    uint64_t magic;            // MAGIC_ALLOCATED = 0xDEADBEEFCAFEBABE
};
```

### 2.3 mixed_bitmap_v2

与 FreePagesAllocator::BuddyControlBlock::mixed_bitmap_v2 相同的堆编码二叉树位图，
详见 `BCB_docv3.md` §2.3。

但 kpoolmemmgr_t::mixed_bitmap_v2 的 `online()` 额外包含物理页分配和页表映射（在 TEST_MODE 下跳过）。

## 3. 核心算法

### 3.1 分配链路

```
alloc(user_size)
  → size_to_order(user_size + 16)         // 计算所需 order
  → internal_alloc(order)                  // cache-first-then-scan
     ├─ cache_pick(order~MAX_ORDER)        // 试所有大于等于 target 的缓存
     │   └─ 找到后: split_page(命中块, 缓存 order → target order)
     │                → bit_set0(左子路径, order) + order_free_count[order]--
     └─ scan_free_block(order)             // 位图全扫描
         └─ 找到后: split_page 同上
  → 写 buddy_meta(data_size, flags, magic)
  → 返回 payload 地址 (block_va + 16)
```

### 3.2 释放链路

```
free(ptr)
  → meta_from_ptr(ptr)                    // 读 buddy_meta
  → 校验 magic
  → size_to_order(meta->data_size + 16)   // 恢复 alloc 时的 order
  → ptr_to_offset(ptr) → order_off        // 计算块偏移
  → internal_free(order_off, order)
     ├─ double-free check
     ├─ free_page_without_merge(初始块)    // 设位 + 计数 + 入缓存
     └─ 向上折叠（参考 conanico_free）
        ├─ buddy 空闲 → 清子块 (count -= 2)
        │   → free_page_without_merge(父块)
        └─ 否则退出
  → meta->magic = 0
```

### 3.3 分裂 (split_page → internal_split)

```
split_page(offset, from_order, to_order)
  for o = from_order down to to_order+1:
    bit_set0(当前块, o)                    // 父块不再空闲
    order_free_count[o]--
    free_page_without_merge(右子块, o-1)   // +1（三位一体）
    order_free_count[o-1]++                // 额外 +1
    cur_off <<= 1                          // 向左子递归
```

### 3.4 折叠 (internal_free → conanico_free)

```
internal_free(offset, order)
  free_page_without_merge(offset, order)   // 初始标记
  while 可以折叠:
    bit_set0(左子, o) & bit_set0(右子, o)  // 清子块
    order_free_count[o] -= 2
    cur_off >>= 1; cur_ord++
    check parent not already free
    free_page_without_merge(父块, o+1)     // 标记父块
```

## 4. 锁模型

锁由**外部**（kpoolmemmgr_t 层）管理，HCB_v3 内部方法不锁：

| 函数 | 锁策略 |
|------|--------|
| kalloc | per-CPU HCB: `spintrylock_try_guard`（锁忙跳过）; first_linekd_heap: `spintrylock_spin_guard` |
| kfree / realloc / clear | 目标 HCB: `spintrylock_spin_guard`（单目标，自旋等待） |

## 5. KURD 模块错误树

HCB_v3 的 KURD 部署遵循五层错误模型：

```
module_code: MEMORY
  in_module_location: LOCATION_CODE_KPOOLMEMMGR_HCB_V3 = 7
    event_code:
      0 = EVENT_CODE_ONLINE
      1 = EVENT_CODE_OFFLINE
      2 = EVENT_CODE_ALLOC
      3 = EVENT_CODE_FREE
      4 = EVENT_CODE_REALLOC
      5 = EVENT_CODE_INTERNAL_ALLOC
      6 = EVENT_CODE_INTERNAL_FREE
```

公共原因（所有事件共享）：

| 组 | 编码 | 原因 |
|----|------|------|
| COMMON_FAIL_REASONS | 0 | BAD_ADDR（非堆内地址 / 不满足 16B 对齐） |
| COMMON_FAIL_REASONS | 1 | ADDR_NOT_THIS_HEAP |
| COMMON_FATAL_REASONS | 0 | METADATA_DESTROYED（magic 校验失败） |

事件私有原因（≥ 32）：

| 事件 | Result | 编码 | 原因 |
|------|--------|------|------|
| ALLOC | FAIL | 32 | SIZE_IS_ZERO |
| ALLOC | FAIL | 33 | SIZE_TOO_LARGE（≥ 2MB） |
| INTERNAL_ALLOC | FAIL | 32 | NO_AVALIABLE_BUDDY |
| INTERNAL_FREE | FATAL | 32 | DOUBLE_FREE_DETECT |
| INTERNAL_FREE | FATAL | 34 | MERGE_BUT_ALREADY_FREE |

模板函数：

```cpp
kurd_default_success()  // domain=CORE_MODULE, module=MEMORY, location=HCB_V3, result=SUCCESS, level=INFO
kurd_default_error()    // result=FAIL, level=ERROR
kurd_default_fatal()    // result=FATAL, level=FATAL
```

三阶段构造：

```cpp
// 1. 取模板
KURD_t success = kurd_default_success();
KURD_t error   = kurd_default_error();

// 2. 注入 event_code
success.event_code = EVENT_CODE_ALLOC;
error.event_code   = EVENT_CODE_ALLOC;

// 3. 叶子处填 reason
error.reason = ALLOC_RESULTS::FAIL_REASONS::REASON_CODE_SIZE_IS_ZERO;
```

传递原则：子接口的原始 KURD 直接向上透传，不拦截翻译。

## 6. 用户态测试

### 6.1 Phase 1 — 单 HCB 测试

- 文件: `src/tests/test_kpoolmemmgr/test_main.cpp`
- 编译: `make test_kpoolmemmgr.x` (在 `src/tests/`)
- 测试: buddy 算法正确性（single / various sizes / split & coalesce / realloc / clear）
- 压测: 1M 次 alloc/free 循环，每 100K 次执行 flush_free_count 校验

### 6.2 Phase 2 — 多 HCB 多线程测试

- 位置: 同一 test_main.cpp，`run_multi_heap_test()`
- 流程:
  1. 设置 logical_processor_count = 4
  2. kpoolmemmgr_t::Init() + multi_heap_enable()
  3. 启动 4 个 pthread，每个调用 kalloc/kfree 模拟 per-CPU 热点堆访问
  4. 结束后遍历所有 HCB 调用 flush_free_count 验证

### 6.3 BCB 测试

- 文件: `src/tests/BCB_test.cpp`
- 编译: `make BCB_test` (在 `src/tests/`)
- 测试 FreePagesAllocator::BuddyControlBlock 的 split_page / conanico_free / allocate_buddy_way

### 6.4 垫片层 (shims)

| 文件 | 作用 |
|------|------|
| bsp_kout_stub.cpp | kio::kout 用户态实现 |
| panic_stub.cpp | Panic::panic → abort |
| 编译时 -DTEST_MODE | ifdef guard 切换内核/用户态路径 |
