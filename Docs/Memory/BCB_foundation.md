# BuddyControlBlock_foundation — v4 伙伴系统底座

## 概述

BuddyControlBlock_foundation 是 SparrowOS 内核的通用伙伴系统底座，基于位图数据结构编码的二叉树，提供 O(N) DFS 分配语义。

它是 mixed_bitmap_v2 (BCB_v3) 的后继者，修复了 v2 的核心缺陷——只记录叶子节点存在性导致不可剪枝、分配退化到 O(2^N) BFS 扫描——通过引入 4 状态节点编码 + 分离的摘要位图实现 DFS 剪枝。

## 极简原理

v2 (mixed_bitmap_v2) 的位图每节点 1bit：
```
bit = 1 → 这个 order 的整块空闲
bit = 0 → 不确定：子树全空？有碎片？还是已分配？无法区分 → 不敢剪枝 → 只能 BFS 全量扫
```

v4 非 order0 每节点 2bit，状态精确：
```
0b00 不存在     → 直接剪枝（整个子树从未参与分配）
0b01 已占用     → 直接剪枝（整个子树已分配完）
0b10 有碎片     → DFS 下降
0b11 全空闲     → 命中
```

分配时最多走 O(N) 步（N = max_order，通常 ≤ 24），与碎片化程度完全无关。

---

## 内存布局

总位图大小：`3 × 2^N bits`（N = max_supprt_order）

```
[0, 2^(N+1))       非 order0 节点区 — 2-bit/node, 节点索引 i ∈ [0, 2^N)
[2^(N+1), 3·2^N)   order0 叶子区   — 1-bit/node, 叶子索引 j ∈ [0, 2^N)
```

### 节点索引（heap 编码）

```
i = 0      弃用（恒 0b00）
i = 1      order = N     (root)
i = 2~3    order = N−1
i = 4~7    order = N−2
...
i ∈ [2^k, 2^(k+1))      order = N−k
...
i ∈ [2^(N−1), 2^N−1]    order = 1
i ∈ [2^N, 2^(N+1)−1]    order = 0（叶子区，走 1-bit 路径）
```

### 索引转换

```cpp
// (order, offset) → heap index
level = N − order
idx   = (1 << level) + offset

// heap index → (order, offset)  
level = 63 − __builtin_clzll(idx)   // floor(log2(idx))
order = N − level
offset = idx − (1 << level)
```

### 位访问

非 order0 节点（2bit，idx < 2^N）：
```cpp
boff = idx * 2;
val = (bitmap[boff >> 6] >> (boff & 63)) & 0b11;
```

order0 叶子（1bit，idx ≥ 2^N）：
```cpp
boff = (1 << N) + idx;
val = (bitmap[boff >> 6] >> (boff & 63)) & 1;
```

注：`idx * 2` 的 bit 偏移恒为偶数（0, 2, 4, …, 62），2bit 永不跨 uint64_t 边界。

---

## 节点状态

### 非 order0 节点（2-bit）

| 值 | 状态     | DFS 行为 | 含义 |
|----|---------|---------|------|
| 0b00 | NONEXIST  | **剪枝** | 从未分裂至此粒度，子树不存在 |
| 0b01 | OCCUPIED  | **剪枝** | 整块已分配，子树全占 |
| 0b10 | NONLEAF   | **下降** | 子树中有空闲碎片或整块 |
| 0b11 | FREE      | **命中** | 整块空闲 |

### order0 叶子（1-bit）

| 值 | 含义 |
|----|------|
| 1  | 空闲页框 |
| 0  | 已占用页框 |

---

## 不变约束

### 约束 1：节点 0 弃用
`node[0]` 恒为 `0b00`，不被任何节点使用。

### 约束 2：NONEXIST 剪枝边界
非 order0 节点 `0b00` 意味着其整个子树（含所有 order0 后代叶框）从未参与过分配——DFS 剪枝的硬边界。

### 约束 3：FREE/OCCUPIED 的子树完整性
非 order0 节点为 `0b11` 时，其两个子节点必为 `0b00`（从未分裂至此粒度）。

### 约束 4：NONLEAF 的子树活跃性
非 order0 节点为 `0b10` 时，其两个子节点中至少有一个不是 `0b00` 或 `0b01`。但两者不能同为 `0b11`（否则父应为 `0b11`）或同为 `0b01`（否则父应为 `0b01`/坍缩）。

### 约束 5：order0 叶子独立性
order0 节点（1bit）不参与上述约束的后代检查，本身即为最细粒度。

### 约束 6：free_count 一致性
`free_count[order]` 必须始终与二叉树状态精确一致。这是 DFS 预跳 `find_candidate` 正确性的前提。

---

## 接口

### find_candidate — DFS 只读搜索

```cpp
uint64_t find_candidate(uint8_t& base_order, KURD_t& kurd);
```

DFS 从根出发，找到第一个 ≥ `base_order` 的 `NODE_FREE` 节点。**只读不写**。

流程：
1. `free_count` 预跳：从 `base_order` 到 `N` 找第一个非零 order，记为 `candidate_order`。无剩余 → 返回错误。
2. DFS 从根下降，先左后右：
   - `NONEXIST/OCCUPIED` → 剪枝
   - `FREE` → 命中，返回
   - `NONLEAF` → 下左，失败再下右
3. 返回：`kurd` 带成功/失败，返回值是该块的 offset，`base_order` 输出实际找到的 order。

### split — 分裂

```cpp
KURD_t split(uint8_t order, uint64_t offset, uint8_t target_order);
```

将一个 `NODE_FREE` 节点分裂到 `target_order`。**右路保留空闲**（供调用方缓存），左路递归减小 order。

内部循环（迭代，非递归）：
```
while (order > target_order):
    parent: 0b11 → 0b10
    left/right child: (0b00/leaf=0) → 0b11
    free_count: parent-1, child+2
    idx = left_child
```

### order_occupy_try — 占用

```cpp
KURD_t order_occupy_try(uint8_t order, uint64_t offset);
```

将 `NODE_FREE` 节点标记为 `NODE_OCCUPIED`（或叶子 1→0）。**可重入安全**（二次调用失败）。

**附带坍缩折叠**：占用后检查父节点——如果父是 `NONLEAF` 且两个子都是 `OCCUPIED`，则父坍缩为 `OCCUPIED`。继续向上传播。这确保 DFS 不会进入整棵死子树。

### order_return — 归还 + 合并

```cpp
uint8_t order_return(uint8_t order, uint64_t offset, KURD_t& kurd);
```

归还一个已占用的块，标记为空闲，然后向上合并（Coalescing）。返回折叠后的最终 order。

**两阶段流程**：
1. **Phase 1 - 合并**：标记 `FREE` → 检查 buddy 是否也 `FREE` → 是则合并（双子 → `NONEXIST`，父 → `FREE`）→ 继续上升
2. **Phase 2 - 坍缩展开**：合并中止后，检查父/祖先是否被坍缩为 `OCCUPIED`（其子树已有空闲，但标记还未更新）→ 展开为 `NONLEAF`，向上传播

### order_exist_check — 快速预判

```cpp
bool order_exist_check(uint8_t order) const;
```

O(1) free_count 查询，用于调用方快速过滤不可用的 BCB。

### is_free — 快速验证

```cpp
bool is_free(uint8_t order, uint64_t offset) const;
```

O(1) 位图读，检查 (order, offset) 处节点是否为 `NODE_FREE`。供上层缓存层做低成本命中验证。

### btree_validation — 全树一致性校验

```cpp
KURD_t btree_validation();
```

从根遍历完整校验二叉树 + `free_count` 一致性。

**规则**：
1. `NONEXIST/FREE` 节点 → 子树全空（非 order0 后代必须 `NONEXIST`，order0 后代必须 0）
2. `OCCUPIED` 节点 → 允许两种情况：(a) children `NONEXIST`（真正整块占用），(b) children 也是 `OCCUPIED`（坍缩产物，递归验证）
3. `NONLEAF` 节点 → children 不能有 `NONEXIST`；对于非 order0 不能同为 `FREE` 或同为 `OCCUPIED`；对于 order0 叶子不能同为 0 或同为 1
4. 遍历完成后比较 `free_count` 与位图实际计数的各 order 空闲块数量

---

## 关键算法

### DFS 分配路径

```
find_candidate(4):
  root NONLEAF → descend
    left NONLEAF → descend
      ... (left chain, all NONLEAF) ...
        order-4 node NONLEAF → descend
          order-3 node OCCUPIED → prune, backtrack
        order-4 right sibling FREE → FOUND! return
```

最多 N 步（通常 ≤ 24），不依赖碎片化程度。

### occupy 坍缩传播

```
occupy(order=1, offset=1):
  node → OCCUPIED, free_count[1]--
  
  向上检查:
    parent(order=2) = NONLEAF, buddy(order=1, offset=0) = OCCUPIED
    双子全 OCCUPIED → 坍缩: parent → OCCUPIED
    
    继续向上:
    grandparent(order=3) = NONLEAF, buddy(order=2, offset=0) = FREE
    兄弟不全是 OCCUPIED → 停止
```

坍缩使得大片全占用的子树在 DFS 时被一次 `OCCUPIED` 直接剪枝，不用逐个节点检查。

### free 展开传播

```
free(order=1, offset=0):
  node → FREE, free_count[1]++
  
  Phase 1: 合并
    buddy(order=1, offset=1) = FREE → 合并 → order=2 FREE
    继续合并 → root = FREE
  
  如果合并中途停止（buddy 不是 FREE）：
  Phase 2: 坍缩展开
    parent(order=2) = OCCUPIED（被坍缩的）→ 展开为 NONLEAF
    grandparent(order=3) 也 OCCUPIED → 展开为 NONLEAF（如果另一子也已非 OCCUPIED）
```

---

## 与 v2 差异

| 特性 | v2 (mixed_bitmap_v2) | v4 (foundation) |
|------|---------------------|-----------------|
| 每节点比特 | 1-bit | 非 order0 2-bit + order0 1-bit |
| 总位图大小 | 2 × 2^N bits | 3 × 2^N bits (+50%) |
| 节点语义 | 0 含义模糊 | 4 状态明确 |
| 分配算法 | BFS 扫描，O(2^N) | DFS 剪枝，O(N) |
| 分裂/合并 | BCB 层手动维护 | 底座内化 |
| free_count | BCB 各处散落维护 | 底座统一维护 |
| 子节点一致 | — | 占用坍缩 + 展开恢复 |

---

## KURD 错误树

```
module_code = INFRA (7)
└─ in_module_location = BCB_FOUNDATION (1)
   ├─ common_fatal_reasons   [0, 0x20)
   │   ├─ BTREE_VIOLATION      = 0x01  （树结构异常）
   │   └─ FREE_COUNT_VIOLAITON = 0x02  （free_count 不一致）
   ├─ common_fail_reasons     [0, 0x20)
   │   ├─ ORDER_OUT_OF_RANGE   = 0x01
   │   └─ TARGET_NOT_FREE      = 0x02（目标节点不是空闲）
   ├─ event INIT (0)
   ├─ event FIND_CANDIDATE (1)
   │   └─ fail: no_more_candidate (0x20)
   │   └─ fatal: FREE_COUNT_VIOLATION
   ├─ event SPLIT (2)
   │   └─ fail: TARGET_NOT_FREE (0x20)
   │   └─ fatal: BTREE_VIOLATION
   ├─ event OCCUPY_TRY (3)
   │   └─ fail: TARGET_NOT_FREE (0x20)
   │   └─ fatal: BTREE_VIOLATION
   ├─ event ORDER_RETURN (4)
   │   └─ fatal: BTREE_VIOLATION
   └─ event TREE_VALIDATION (5)
```

---

## 性能

BCB_ORDER=16、1M 次随机分配释放测试结果：

| 配置 | 平均 | vs 基线 |
|------|------|---------|
| Foundation 直调 | ~500 cyc/op | — |
| + BCB 缓存层 | ~580 cyc/op | +17% |
| + btree_validation | +5ms/次 | (仅诊断) |

分配失败时的空缓存扫描成本已通过 `is_free` 单次位图读验证控制到最低。

---

## 集成指南

调用方需：
1. 分配 `3 × 2^N bits` 的位图内存
2. 调用 `fnd.init(bitmap_va, max_order)` 初始化
3. 使用 `find_candidate + split + order_occupy_try` 组合完成分配
4. 使用 `order_return` 完成释放
5. 用 `btree_validation` 做诊断校验

BCB 层建议保留缓存（`cache_pick` 用 `is_free` 验证）和右兄弟链填充逻辑以提升时间局部性好的工作负载性能。
