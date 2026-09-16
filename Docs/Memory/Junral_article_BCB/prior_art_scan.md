# BCB 论文 prior-art 扫描报告

**日期**: 2026-09-13
**目标**: 判定 `junral_draft.md` (*4-State Bitmap-Encoded Buddy System: Repairing DFS Pruning with 2-bit-per-Node*) 的生态位是否已被完全占据
**检索轴**: 伙伴系统 / 位图 / 堆编码（隐式二叉树）/ DFS 遍历与剪枝 / 2-bit 节点状态
**检索源**: Google Scholar、OpenAlex、Crossref、Bing、GitHub（repo search）
**检索代理**: mihomo 127.0.0.1:7892（直连不可用）

---

## 0. 结论（TL;DR）

**没有一篇“标题级”完全重合的论文**（"bitmap-encoded buddy" 0 命中，"4-state buddy" 无对口）。
**但论文的核心 novelty claim（“4-state、2-bit-per-node 编码此前未被描述”）基本站不住**：
四条支柱——(a) 每节点 2 bit、(b) AND/OR 子树状态、(c) 可剪枝的 top-down 搜索、(d) 四状态分类——**分别且成组地**在 1995–2024 的文献里反复出现，且其中一篇（EPFL 2023）就是**同一个生态位：freestanding kernel 的页帧分配器**。

**当前可安全主张的 novelty 需要收缩与重构**（见 §4）。

---

## 1. 直接命中（Direct Hits，按威胁度排序）

### ★★★★★ iBuddy —— 四状态位图（四状态 claim 直接撞车）
- Park, Choi, Lee, Noh. *"iBuddy: Inverse Buddy for Enhancing Memory Allocation/Deallocation Performance on Multi-core Systems."* **IEEE Trans. Computers, 2014.**
- 原文: *"Each buddy space in the iBuddy system is in one of **four states**: assigned, unassigned with all frames used, unassigned with partial frames used, and unassigned with all frames freed."*
- 并且: *"the bitmap of the iBuddy system indicates **not only the state of the corresponding buddy group, but also that of the corresponding page frame**."*
- 撞车点: 位图驱动的伙伴结构 + **四状态**分类，与本文 NONEXIST/OCCUPIED/NONLEAF/FREE 的分类学几乎一一对应（且同属“**page frame / 物理内存**”语境）。差异仅在：iBuddy 的动机是多核释放性能，不是 DFS 剪枝；且其状态描述在“buddy space”粒度。

### ★★★★★ Desboeufs, Bugnion, Castes —— kernel 帧分配器 + 1-bit→2-bit + OR 树（同生态位）
- *"An Operating System Kernel Frame Allocator in Rust."* EPFL, **2023**（semester project；Bugnion 是 EPFL 系统组）。
- 原文链条几乎逐句复刻本文动机：
  - *"The buddy allocation technique can be illustrated with a full binary tree, with node values indicating whether a node is free (1 for free, 0 for used) … a parent block is free if its two children are also free (**logical AND tree**)."*
  - *"Top level does not provide any information about available 8Kb and 4Kb blocks … go through each 4Kb block to find the first one that is free."*（≈本文 §2 的 1-bit 歧义 → BFS）
  - *"In order to know which child is free, **each node has two bits instead of one**."*
  - *"Our solution … is to create an **OR tree** rather than an AND tree, which means that we have a 1 if at least one child is free."*（≈本文 4-state 的 NONLEAF 信息位）
  - 用 BSF/BSR 找首个空闲位；x86-64；freestanding。
- 撞车点: **同为 freestanding kernel 的物理页帧分配器**，同为“1-bit AND 树无法定位特定 order 的空闲块 → 加 OR 信息（2 bit）→ 自顶向下可剪枝搜索”。
- 差异: 它的 2 bit 是“左子/右子是否 free”（child-bit 方案），不是本文的节点状态机；且未做 1-bit/2-bit/8-bit 三方横评。

### ★★★★☆ Chang & Gehringer —— AND 树 + OR 树 + 非回溯搜索（本文 2-bit 的直系祖先）
- *"A High Performance Memory Allocator for Object-Oriented Systems."* **IEEE Trans. Computers, 1996** (DOI 10.1109/12.485574, ~89 引用).
- 原文: *"**Two binary trees formed by anding and oring** propagate information about the allocation status of blocks and subblocks. They implement a **nonbacktracking search for the address of the first free block** that is large enough to satisfy a request."* 外加 bit-flipper 消除内部碎片。
- 撞车点: 这就是本文“2-bit = AND 位(全空可用) + OR 位(子树有空)”的等价物，且“非回溯搜索”= DFS 剪枝。**本文所谓“修复 1-bit 歧义”这个思路本身就是 1996 年的设计。**

### ★★★★☆ Ngan & Gehringer —— 硬件伙伴分配器 AND/OR 树（1995）
- *"The Design of a Hardware Memory Allocator Based on the Buddy System."* NCSU, **1995**.
- and-tree / or-tree 动态连接 + “flipper tree”。1996 那篇的直系前身。

### ★★★★☆ SysAlloc —— 2 bit/节点，三状态（2015）
- Xue & Thomas. *"SysAlloc: A Hardware Manager for Dynamic Memory Allocation in Heterogeneous Systems."* **FPL 2015** (DOI 10.1109/fpl.2015.7293959).
- *"Each node in the allocation tree contains **two bits** … There are **three states** for a node: Empty (00), Partially full (10), [Full] …"*，基于 or-gate tree。
- 撞车点: **2-bit/节点 + 多状态（3 态）**已在 2015 出现。本文的“第 4 态 NONEXIST”相对 SysAlloc 只是把“从未分裂”从“全占用”里拆出来——而两者对 DFS 都是 prune，功能冗余。

### ★★★★☆ Wang et al. —— HLS 硬件 DMM：OR-gate 树 + AND 树 + 2 bit/节点 + DFS-like
- *"A Scalable, Efficient, and Robust Dynamic Memory Management Library for HLS-based FPGAs."* **IEEE/ACM ICFPT 2024**；以及 *"High-Performance and Resource-Efficient Dynamic Memory Management in High-Level Synthesis."* **DAC 2024**。
- 原文: *"a modified buddy tree that integrates an **OR-gate tree and an AND-tree** … Each node in this tree **consists of two bits**"*；*"alloc … leads a **DFS-like search**"*。
- 撞车点: 2-bit/节点 + 可剪枝 DFS 搜索，2024 年still在发。

### ★★★☆☆ Li / Mohanty / Kavi —— software-hardware 混合页分配器 AND/OR 树（2006）
- *"A Page-Based Hybrid (Software-Hardware) Dynamic Memory Allocator."* **IEEE Computer Architecture Letters, 2006.** 使用 AND-tree 与 OR-tree。

### ★★★☆☆ Sadeghi et al. —— 硬件 OR-GATE 树、两级（2023）
- *"High-Performance Memory Allocation on FPGA with Reduced Internal Fragmentation."* **IEEE Access, 2023.** 两级 OR-GATE 树。

### ★★★☆☆ NBBS —— 每节点多 bit 位图（并发伙伴）
- Marotta, Ianni, Pellegrini, Scarselli. NBBS, **IEEE CLUSTER 2018 / IEEE Trans. Computers 2021**.
- *"each node in the tree **embeds a bitmap with 5 bits**"*（节点自身 + 子节点状态）。
- 撞车点: “伙伴树 + 每节点多 bit 状态位图”已被用于生产级分配器；只是动机是 lock-free 并发。

---

## 2. 次级 / 相邻（Adjacent，非直接撞车但需在 Related Work 覆盖）

- **DMMX** — Chang, Srisa-an, Lo. *"DMMX: Dynamic Memory Management Extensions."* JSS 2002. bitmap-based allocator，complete binary tree，改造版 buddy。
- **Chang & Gehringer 1996 的 OR-gate 树** 亦见 **"A high performance memory allocator for object-oriented systems"** 之外：Mohanty, *"A Hardware Assisted High Performance PHK Memory Manager"* 2006。
- **LLFree** — Wrenger et al. **USENIX ATC 2023**. 现代页帧分配器，bitmap tree（3 级、per-order 位图）。相邻：现代“位图树页分配器”，但非 2-bit/节点。
- **Tree Bitmap** — Eatherton, Varghese, Dittia. SIGCOMM CCR 2004. *"two bitmaps per node"*（IP 路由表，异域但“每节点两个位图”同构）。
- **TLSF** — Masmano et al. Software: Practice & Experience 2008. bitmap + segregated fit，常数时间；不同范式。
- **形式化验证** — Jiang et al. ICECCS 2019（quad-tree + bitmap 规格）；Feng et al. 2019（Zephyr RTOS buddy + 辅助 quad-tree）。
- **PIM-malloc** — IEEE 2026，buddy 树 metadata bitmap 遍历。
- **Rust-based Buddy 物理内存分配器形式化验证** — Wang et al., TASE/LNCS 2026。
- **PIM/GPU 侧** — AlignMalloc（2025）用 OR-tree 位图。
- **经典** — Knowlton 1965；Peterson & Norman 1977；Wilson et al. *"Dynamic Storage Allocation: A Survey and Critical Review"* 1995（综述，必须引）。
- **GitHub 生态** — "buddy allocator bitmap" 有 ~18 个 repo（francesco-fortunato、PlusPlusUltra、Restioson 等），说明“位图 buddy”是常见教学/工程写法（多数为 1-bit）。

---

## 3. 论文 novelty claim 逐条核销

| 论文主张 | 状态 | 依据 |
|---|---|---|
| 1-bit 位图语义歧义导致 DFS 不可行 | **已被充分讨论** | EPFL 2023 逐句同论；1996 AND 树已隐含 |
| 2 bit/节点、子树状态（空/部分/占用） | **已有** | Ngan 1995; Chang&Gehringer 1996; SysAlloc 2015; Wang 2024 |
| 可剪枝的 top-down 搜索（DFS/非回溯） | **已有** | Chang&Gehringer 1996; Desboeufs 2023; Wang 2024 |
| **四状态**分类（NONEXIST/OCCUPIED/NONLEAF/FREE） | **大幅重合** | **iBuddy 2014 四状态**；SysAlloc 三态是其子集 |
| “此前未被描述” | **不成立** | 上述；只能收窄为“该特定命名/打包 + 软件内核场景组合” |
| 与 Netty PoolChunk（8-bit depth）三方横评 | 可能仍较新（工程性） | 未见“1-bit vs 2-bit vs byte-per-node 空间-延迟 Pareto”横评 |
| 3·2^N 位的紧凑打包 | 工程细节，非概念新 | — |
| collapse propagation / unfold 簿记 | 等价于 AND 树维护，概念不新 | Chang&Gehringer 1996 的 AND 树即逐层“坍缩” |

> 另注（2026-09-13 复查订正）：`research_discovery.md` 里的 **DeepFirst vs ShallowFirst 碎片吸引子** 维度，**不是空白**。通用命题「分配/放置策略 → 碎片」自 1970s 起被反复研究（Robson 1977；CACM 1975；fragmentation index 仿真 1979），伙伴系统碎片本身亦有专门分析（Acta Informatica《Memory fragmentation in buddy methods for dynamic storage allocation》；50% 规则；ISPASS 2001；generalised buddy 2001）。DeepFirst(左优先/低地址优先) vs ShallowFirst ≈ **address-ordered vs 非 address-ordered 放置**之争——已知结论。
> 仍可能未被精确覆盖的，仅是该**具体旋钮**：堆编码位图二叉树中的 DFS 下降顺序 + 可调深度 K，及其与 O(N)/O(2^N) 搜索成本、缓存层补偿的耦合。属**窄缝**，非空白；投稿前必须对照上述策略文献做定位。

---

## 4. 建议（尽快发的情况下）

1. **必须改 Related Work 并软化措辞**：删/改 *"To the best of our knowledge … have not been previously described"*。至少补引：Ngan&Gehringer 1995、Chang&Gehringer 1996、iBuddy 2014、SysAlloc 2015、Desboeufs 2023、Wang 2024、Wilson 1995 综述。
2. **重定位贡献**：从“提出新编码”改为
   - (a) 把它落到**纯软件、freestanding 内核、flat bitmap** 的工程实现；
   - (b) 给出 **1-bit / 2-bit / 8-bit(Netty-style) 的空间-延迟 Pareto 横评**（这块确实少见）；
   - (c) 加上 **DeepFirst/ShallowFirst 碎片吸引子** 维度，但**必须**先与 Robson 1977 / CACM 1975 / Acta Informatica buddy 碎片 / address-ordered 放置等经典策略文献划清界限（见 §3 复查订正）：能写的只是「位图树 DFS 顺序 × K × 搜索成本 × 缓存补偿」这层交互，不能再宣称这是一个新现象。
3. **优先级**：论文已公开在 GitHub，同日上 **arXiv 预印本**钉死时间戳，避免被抢/被指出撞车时无法主张独立发现。
4. **口径**：承认 AND/OR 树谱系，把 NONEXIST vs OCCUPIED 的第 4 态定位为“为软件簿记/校验保留的区分”，而非“为剪枝必需”——避免被审稿人用 SysAlloc/iBuddy 直接打脸。

---

## 5. 检索方法与局限

- 命中查询（节选）: `bitmap buddy allocator 2-bit node DFS`、`"buddy allocator" "depth-first search"`、`"buddy allocator" "two bits"`、`"four states" buddy allocator`、`"buddy system" "AND tree" "OR tree"`、`buddy allocator bitmap "state machine"`、`"bitmap-encoded buddy"`（0）、`"NONEXIST" "OCCUPIED" "NONLEAF"`（0）。
- **未覆盖**（后续可补）: CNKI/万方中文库；Google Patents/专利；IEEE/ACM 全文（仅摘要+片段）；DBLP（被 Anubis 挡）；纯 Web 博客（Netty、Zephyr 实现文档）。
- 结论稳健性: “无标题级重合”可信；“核心 claim 已被预见”证据强（≥6 篇跨 1995–2024）。

---
*Raven 🐦‍⬛ · 2026-09-13 · prior-art scan for Junral_article_BCB*
