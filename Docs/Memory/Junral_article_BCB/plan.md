# BCB_foundation 短文项目规划

## 项目定位

**标题方向**: *4-State Bitmap-Encoded Buddy System — Repairing DFS Pruning with 2-bit-per-Node*

**篇幅**: 短篇技术报告 / workshop paper，预估 5 页

**核心叙事**: BCB_v3 (1-bit mixed_bitmap_v2) 的模糊语义导致分配退化到 O(2^N) BFS → BCB_v4 (2-bit 4-state) 修复了这个问题，DFS O(N) 且碎片无关 → 与 Netty PoolChunk (8-bit) 在空间-延迟 tradeoff 曲线上做横评

**Three-way benchmark**: BCB_v3 / BCB_v4 / Netty-style 在同一测试框架下的分配延迟对比

---

## 目录结构

```
Junral_article_BCB/
├── plan.md                    ← 本文件，项目规划
├── junral_draft.md            ← 最终论文本体（md 格式）
├── junral_draft.docx          ← 转换为 .docx 后输出
└── refs/                      ← 引用文献 PDF（可选）

# 实验代码存放于内核 src/tests/，而非本项目目录
# kernel/src/tests/benchmark_triple.h/cpp <- 三路对比框架 (新增)
# kernel/src/tests/netty_simulator.h/cpp  <- Netty 模拟器 (新增)
# kernel/src/tests/BCB_foundation_test.cpp <- 已有, 保留
```

---

## 论文结构（草案）

### 1. Introduction (0.5p)
- Buddy system 是 freestanding kernel 里物理页分配器的事实标准
- bitmap-encoded 变体在空间效率和排序稳定性上有优势
- 但 1-bit-per-node 的语义歧义导致 DFS 不可行 → BFS O(2^N)
- 本文贡献：4-state 编码 + 跨 allocator 的 benchmark 横评

### 2. The 1-Bit Ambiguity Problem (1.0p)
- v2 (mixed_bitmap_v2) 的 heap-encoded 1-bit 设计
- 核心缺陷：`bit=0` 不能区分 "子树全空"、"有碎片"、"已分配"
- 形式化分析：O(2^N) BFS worst-case 的量化展示
- N=3 小树跟踪例子

### 3. The 4-State Solution (1.5p)
- 内存布局：3×2^N bits 两区设计
- 非 order0 节点：`NONEXIST(00) / OCCUPIED(01) / NONLEAF(10) / FREE(11)`
- order0 叶子：1-bit
- 核心算法：
  - find_candidate: free_count 预扫 + DFS 剪枝
  - split: 迭代分裂，右路保留供缓存
  - order_occupy_try: 占用 + 坍缩传播
  - order_return: 折叠合并 + 展开恢复
- 不变约束与 btree_validation

### 4. Evaluation (1.5p)
- **三路对比设计**：
  - BCB_v4 (foundation, 2-bit)
  - BCB_v3 (mixed_bitmap_v2, 1-bit) — 基线
  - Netty-style (byte-per-node depth-encoding, 8-bit) — 对照

- **基准配置**：
  - BCB_ORDER=16，管理 256MB
  - 固定分配需求集（pre-generated trace）：1M 次 alloc/free
  - 指数分布 order_dist(λ=0.3)，混合 small/medium allocs

- **度量维度**：
  1. 平均分配延迟（cycles/op）— 主指标
  2. 分配延迟的标准差 — 稳定性
  3. 碎片化退化曲线 — 空闲池占用率 10%~90% 下的延迟
  4. 位图空间开销 — 理论值 + 实测
  5. 总执行时间（wall clock）

- **预期结果**：
  - BCB_v3: 随碎片化退化显著，高占用率时延迟飞涨
  - BCB_v4: 碎片无关，flat 曲线
  - Netty: 同碎片无关，绝对延迟更低，但空间 5.3×

### 5. Related Work (0.3p)
- Knowlton 1965, Peterson 1977: 经典 binary buddy
- Linux buddy: per-order free list (O(1) but no tree encoding)
- Netty PoolChunk: byte-per-node, O(log N)
- jemalloc/tcmalloc: 不同路径的 allocator 设计

### 6. Conclusion (0.2p)
- 2-bit 4-state 是 bitmap-encoded buddy 的 Pareto 最优解
- 独立发现，非源于 Netty 的改编

---

## 实验基础设施

### 正在工作的测试框架 (`BCB_foundation_test.cpp`)

```cpp
// w_alloc  & w_free 已实现
// stress test: 1M cycles, exponential_distribution(0.3)
// 每 100K ops 打一次区间延迟，做 btree_validation
```

### 需要新增

| 组件 | 存放路径 | 预估代码行 |
|------|---------|-----------|
| `netty_simulator.h/cpp` | `kernel/src/tests/` | ~200 |
| `benchmark_runner.h/cpp` | `kernel/src/tests/` | ~150 |
| trace 生成器 | `kernel/src/tests/` (嵌入 benchmark_runner) | ~50 |
| CMakeLists.txt 更新 | `kernel/CMakeLists.txt` | 微调 |

### Netty 算法 YY
- heap-encoded 二叉树，`byte[] memoryMap`
- 每节点值 = 该子树最大可分配 order
- alloc: DFS 下降，取 `min(child_val(左), child_val(右))` 选路
- occupy: 节点设为 `max_order+1`，冒泡更新祖先
- free: 节点恢复初始化值，冒泡更新祖先 `val = min(children, init)`
- 不需要 free_count、坍缩/展开等额外机制

---

## 近期 to-do（按优先级）

1. [ ] **撰写 junral_draft.md 初稿** — 先搭骨架，留空实验数据位置
2. [ ] **实现 netty_simulator.h/cpp** — 200 行，算法落实
3. [ ] **实现 benchmark_runner** — 三路封装，trace 复用
4. [ ] **跑数据填表填图** — 1M ops × 3 backends
5. [ ] **校对、去冗余、补 Related Work** — 短文要精炼
6. [ ] **md → docx 转换** — 工具链验证

---

## 决策记录

| 日期 | 决策 |
|------|------|
| 2026-05-29 | 确定为短文，5 页左右 |
| 2026-05-29 | 三路对比：BCB_v3 / BCB_v4 / Netty-style |
| 2026-05-29 | 单一分配集 benchmark，不展开多数据集 |
| 2026-05-29 | Docx 作为最终格式，md 作为编辑态 |
| 2026-05-29 | 不跟 Linux buddy allocator 做 benchmark 对比（范式不同） |
| 2026-05-29 | 参考文献诚实标注，承认独立发现 |
