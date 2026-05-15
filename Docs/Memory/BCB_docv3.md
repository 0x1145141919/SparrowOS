# FreePagesAllocator::BuddyControlBlock (BCB) 文档 v3

## 1. 引言

`BuddyControlBlock`（简称 BCB）是 FreePagesAllocator 中的核心类，用于实现基于位图（bitmap）的二进制伙伴分配器（binary buddy allocator）。它管理一段连续的物理内存段（segment），以 4KB 页为最小单位，支持 power-of-2 大小的块分配和释放。

v3 的核心变化是用 `mixed_bitmap_v2` 替换了原来的 `mixed_bitmap_t` + `order_bases[]` 设计。新位图使用**堆编码二叉树**（heap-encoded binary tree），节点在数组中的位置天然编码了 order 和父子关系，不再需要 `order_bases` 数组。

## 2. 数据结构变更

### 2.1 改前 (v2)

```
class BuddyControlBlock {
    mixed_bitmap_t* order_freepage_existency_bitmaps;  // 指针, 需 new
    uint64_t order_bases[DESINGED_MAX_SUPPORT_ORDER];   // 各 order 位图偏移
};
```

位图为**分段线性布局**：每个 order 的位图段在数组中连续排列，通过 `order_bases[]` 索引。
- `order_bases[0] = 0`
- `order_bases[order+1] = order_bases[order] + 2^(max_order - order)`

### 2.2 改后 (v3)

```
class BuddyControlBlock {
    mixed_bitmap_v2 bcb_bitmap;  // 内嵌, 无需 new
    // order_bases[] 已删除
};
```

#### mixed_bitmap_v2

继承自 `bitmap_t`，使用 1-indexed 完全二叉树（数组堆编码）在扁平位图中编码伙伴系统：

```cpp
class mixed_bitmap_v2 : bitmap_t {
    uint8_t out_order = 0;       // 树深度 (max_order)
public:
    KURD_t online(vaddr_t bitmap_va, uint8_t out_order);
    KURD_t offline();

    // 在 [1, 1 << (1 + out_order - order)) 范围扫第一个 1 bit
    uint64_t scan_free_block(uint8_t& order);

    // 通过 <order, offset> 安全位操作 (替代 raw bit_get/bit_set)
    void bit_set0(uint64_t offset, uint8_t order);   // 清除 (标记分配/分裂)
    void bit_set1(uint64_t offset, uint8_t order);   // 设置 (标记空闲)
    bool bit_get(uint64_t offset, uint8_t order);    // 读取
};
```

### 2.3 编码方式

```
位图大小 = 1 << (1 + out_order) bits

索引规则:
  bit[0] = 0                      (强制, SIMD 填充边界)
  bit[1]                = order k 节点 (k = max_order)
  bit[2..3]             = order k-1 节点
  bit[4..7]             = order k-2 节点
  ...
  bit[2^k .. 2^(k+1)-1] = order 0 节点

正向公式: idx = 2^(out_order - order) + offset
逆向公式: level = 63 - __builtin_clzll(idx)
          order = out_order - level
          offset = idx - (1ULL << level)

伙伴关系: buddy = offset ^ 1    (同一 order 内)
父节点:   parent_offset = offset >> 1  (高一个 order)
```

## 3. 接口变更对照

所有 BCB 核心算法原通过 `order_bases[o] + idx` 访问位图，现改为 `<order, offset>` 对访问：

### 3.1 cache_pick

```cpp
// v2:  raw 位图索引
if (order_freepage_existency_bitmaps->bit_get(order_bases[order] + idx))

// v3:  <order, offset> 对
if (bcb_bitmap.bit_get(idx, order))
```

### 3.2 free_page_without_merge

```cpp
// v2:
order_freepage_existency_bitmaps->bit_set(order_bases[order] + in_bcb_idx, true);
statistics.free_count[order]++;
cache_insert(order, in_bcb_idx);

// v3:
bcb_bitmap.bit_set1(in_bcb_idx, order);
statistics.free_count[order]++;
cache_insert(order, in_bcb_idx);
```

### 3.3 split_page

```cpp
// v2:
order_freepage_existency_bitmaps->bit_set(order_bases[splited_order] + splited_idx, false);
statistics.free_count[splited_order]--;
free_page_without_merge(1 + (splited_idx << 1), splited_order - 1);
statistics.free_count[splited_order - 1]++;

// v3:
bcb_bitmap.bit_set0(splited_idx, splited_order);
statistics.free_count[splited_order]--;
free_page_without_merge(1 + (splited_idx << 1), splited_order - 1);
statistics.free_count[splited_order - 1]++;  // 额外 +1 平衡分配时的 --
```

### 3.4 conanico_free

```cpp
// v2:
if(!order_freepage_existency_bitmaps->bit_get(order_bases[current_order] + buddy_idx))
    break;
order_freepage_existency_bitmaps->bit_set(order_bases[current_order] + buddy_idx, false);
order_freepage_existency_bitmaps->bit_set(order_bases[current_order] + current_idx, false);

// v3:
if(!bcb_bitmap.bit_get(buddy_idx, current_order)) break;
bcb_bitmap.bit_set0(current_idx, current_order);
bcb_bitmap.bit_set0(buddy_idx, current_order);
```

### 3.5 allocate_buddy_way — 扫描路径

```cpp
// v2: 用 find_free_in_interval 分 order 段扫描
uint64_t found_idx = order_freepage_existency_bitmaps->find_free_in_interval(
    order_bases[i], 1ULL << (max_supprt_order - i));
order_freepage_existency_bitmaps->bit_set(order_bases[order] + (found_idx << (i - order)), false);

// v3: 用 scan_free_block 统一扫描后转 <order, offset>
uint8_t found_order = i;
uint64_t found_idx = bcb_bitmap.scan_free_block(found_order);
// 转换 heap-index → <order, offset>
uint64_t level = 63 - __builtin_clzll(found_idx);
uint64_t found_offset = found_idx - (1ULL << level);
// 分裂 + 分配
bcb_bitmap.bit_set0(leftmost_offset, order);
```

### 3.6 corebcb_mixedbitmap_base_acclaim — 初始化

```cpp
// v2:
order_freepage_existency_bitmaps = new mixed_bitmap_t(1ull << (max_supprt_order + 1), bitmap_base_addr);
order_freepage_existency_bitmaps->mixedbitmap_base_specify(bitmap_base_addr);
order_bases[0] = 0;
for(uint8_t order = 0; order < max_supprt_order; order++){
    order_bases[order + 1] = order_bases[order] + (1ULL << (max_supprt_order - order));
}

// v3:
bcb_bitmap.online(bitmap_base_addr, max_supprt_order);
// order_bases 已删除 — 无需计算
```

### 3.7 free_pages_flush — 校验

```cpp
// v2:
if(order_freepage_existency_bitmaps->bit_get(order_bases[i] + j)) actual++;

// v3:
if(bcb_bitmap.bit_get(j, i)) actual++;
```

## 4. 消除的字段

| 字段 | 大小 | 说明 |
|------|------|------|
| `mixed_bitmap_t* order_freepage_existency_bitmaps` | 8B (指针) | 替换为内嵌 `mixed_bitmap_v2` |
| `uint64_t order_bases[24]` | 192B | 删除, 位置编码 order |
| 构造时 `new mixed_bitmap_t` | — | 内嵌成员在 BCB 构造时自动构造 |

## 5. 不动的部分

- `size_to_order()` — 算法不变
- `free_buddy_way()` — 入口/出口不变
- `can_alloc()` — 只读 `free_count[]`
- `BCB_statistics` 结构 — 不变, 含 `free_count[]`
- 缓存机制 — `cache_insert/pick` 签名不变, 内部 bitmap 访问更新
- `REPLAY_MODE` 验证 — 框架保留, 位图操作从新接口继承
- 锁/`dirty_count`/`is_addr_belong_to_this_BCB` — 不变
- `first_BCB_bitmap[]` 备用数组 — 保留

## 6. 发现的缺陷与修复（2026-05-14）

### 6.1 allocate_buddy_way 缓存路径缺陷

**症状**：压力测试（BCB_test 1M cycles）中 `free_pages_flush` 报告 order 1 的 `free_count` 持续偏差,
expected > actual, 表示释放计数虚增。

**根因**：缓存命中的代码路径在从 `split_page` 返回后, 未将 `cached_idx` 从 source order `i` 换算到 target order `order`：

```cpp
// ❌ 错误：cached_idx 是 order i 的偏移, 不是 order
bcb_bitmap.bit_set0(cached_idx, order);
phyaddr_t res_addr = base + (cached_idx << (order + 12));

// ✅ 修复：
uint64_t alloc_idx = (i > order) ? (cached_idx << (i - order)) : cached_idx;
bcb_bitmap.bit_set0(alloc_idx, order);
// 物理地址仍由原始 cached_idx 在 order i 上决定
phyaddr_t res_addr = base + (cached_idx << (i + 12));
```

该缺陷在从 `mixed_bitmap_t`（分段线性）迁移到 `mixed_bitmap_v2`（堆编码）时引入,
参考提交 `808f40e` 之前的 `FreePagesAllocator.cpp` 中对应路径的正确写法（`cached_idx << (i - order)` 和 `cached_idx << (i + 12)`）。

### 6.2 kpoolmemmgr_t::HCB_v3::internal_free 折叠计数缺陷

**症状**：Phase 1 测试中 `flush_free_count` 报告 order 0~2 的 `order_free_count` 偏差。

**根因**：折叠循环中 `order_free_count[cur_ord] -= 2` 时,
其中一个计数是刚释放的块（从未被计入 free count）, 减去 2 导致下溢/计数漂移。

**修复**：将 `free_page_without_merge`（设位+计数+入缓存）提前到折叠循环之前,
与 FreePagesAllocator::BuddyControlBlock::conanico_free 保持一致的流程。

## 7. 与 v2 文档的主要差异

| 方面 | v2 (mixed_bitmap_t) | v3 (mixed_bitmap_v2) |
|------|---------------------|---------------------|
| 位图布局 | 分 order 段, `order_bases[]` 索引 | 堆编码二叉树, 位置隐式编码 order |
| 位图访问 | `bit_get(order_bases[o] + idx)` | `bit_get(idx, order)` 用 <order, offset> 对 |
| 扫描 | `find_free_in_interval(base, len)` | `scan_free_block(order)` |
| 初始化 | `new mixed_bitmap_t` + `mixedbitmap_base_specify` + 循环算 order_bases | `online(va, max_order)` 一步 |
| order_bases[] | 192B 数组 + 初始化循环 | 已删除 |
| 伙伴定位 | `order_bases[o] + (idx ^ 1)` | `idx ^ 1` 直接异或 |
| 内存管理 | 外部分配, 指针成员 | 内嵌成员, 自动管理 |
