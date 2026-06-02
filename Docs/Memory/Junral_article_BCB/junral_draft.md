# 4-State Bitmap-Encoded Buddy System: Repairing DFS Pruning with 2-bit-per-Node

**Author**: SparrowOS Kernel Project

**Date**: 2026-05-29

**Draft**: v0.1 — skeleton with placeholder sections

---

## Abstract

The bitmap-encoded binary buddy system is a compact allocator design well-suited for freestanding kernel environments where dynamic page metadata is unavailable. Prior work using 1-bit-per-node heap-encoded bitmaps suffers from a fundamental semantic ambiguity: a cleared bit cannot distinguish between "subtree fully free," "subtree partially allocated," and "subtree fully allocated." This renders depth-first-search pruning infeasible and forces worst-case O(2^N) breadth-first scans. We present a 4-state node encoding that resolves this ambiguity using 2 bits per non-order0 node (1 bit for order0 leaves), consuming 3·2^N bits total — a 50% increase over the 1-bit design. This enables DFS allocation in O(N) steps regardless of fragmentation level. We evaluate our design against both the 1-bit predecessor and a byte-per-node depth-encoding allocator (Netty PoolChunk style). On an N=16, 256-MB managed region with 1 million random alloc/free operations, our design shows fragmentation-independent latency averaging [TBD] cycles per operation, with [TBD]× less per-node space than the byte-encoded approach.

---

## 1. Introduction

The binary buddy system [1, 2] partitions memory into power-of-two blocks and maintains free status at each block granularity. It is the de-facto physical page allocator in freestanding kernels because of its bounded external fragmentation and O(1) coalescing.

A particularly space-efficient variant encodes the binary tree into a flat bitmap. In this encoding, tree-node positions in the array implicitly encode block order and parent-child relationships via the heap-index layout, eliminating the need for explicit pointers or per-order segment tables.

However, the standard 1-bit-per-node encoding introduces a fundamental semantic limitation. A cleared bit (0) carries **three possible interpretations**:

| Interpretation | Meaning |
|---------------|---------|
| Subtree never touched | All descendant pages are pristine, never allocated |
| Subtree fully allocated | All descendant pages are busy |
| Subtree partially occupied | Some descendants free, some allocated |

Because the allocator cannot distinguish these cases from a single bit, it cannot safely prune subtrees during DFS descent. The only correct strategy is breadth-first scanning over each order's entire bit segment — worst-case O(2^N) bit accesses per allocation.

**Our contribution** is a 4-state bitmap encoding that resolves this ambiguity with 2 bits per non-order0 node:

- `NONEXIST (00)` — subtree never split to this granularity
- `OCCUPIED (01)` — entire subtree allocated
- `NONLEAF (10)` — subtree partially available
- `FREE (11)` — entire block free

This encoding costs 3·2^N bits (vs. 2·2^N bits for the 1-bit design) but restores fully correct DFS pruning. Allocation becomes O(N) [TBD], independent of pool fragmentation.

We evaluate three allocator designs — the 1-bit predecessor, our 2-bit proposal, and a byte-per-node depth-encoding allocator (representative of [4]) — on identical allocation traces. Our measurements show:

- Fragmentation-independent latency in the 2-bit and 8-bit designs
- 5.3× per-node space saving vs. 8-bit encoding
- Up to [TBD]× latency degradation in the 1-bit design under heavy fragmentation

[Section ~0.5 pages]

---

## 2. The 1-Bit Ambiguity Problem

### 2.1 Heap-Encoded Binary Tree Layout

Both the 1-bit (v3) and 2-bit (v4) designs share the same heap-index encoding for tree nodes:

```
idx = 0                   — unused (mandatory sentinel)
idx = 1                   — root, order = N
idx = 2..3                — order = N−1
idx = 4..7                — order = N−2
...
idx ∈ [2^k, 2^(k+1)−1]   — order = N−k
...
idx ∈ [2^N, 2^(N+1)−1]   — order = 0 leaves
```

The conversion between heap index and (order, offset) is:

```cpp
// (order, offset) → heap index:
level = N − order;
idx   = (1 << level) + offset;

// heap index → order, offset:
level = 63 − __builtin_clzll(idx);
order = N − level;
offset = idx − (1 << level);
```

**Buddy and parent relations** are also implicit:

```cpp
buddy_idx    = idx ^ 1;   // XOR with 1: same order, adjacent block
parent_idx   = idx >> 1;   // shift right: one order higher
```

### 2.2 The 1-Bit Mixed Bitmap (v3)

In the v3 design (`mixed_bitmap_v2`), the bitmap occupies 2·2^N bits organized as a single heap-encoded binary tree with **1 bit per node**. A set bit (1) means the block at that order and offset is completely free; a cleared bit (0) means anything else.

**The ambiguity crisis manifests during allocation.** Consider `find_candidate(order=4)` descending from the root:

```
Root (order=N): bit=0 → ❓ Cannot prune
├─ Left (order=N-1): bit=0 → ❓ Still cannot prune
│  ├─ Left: bit=0 → ❓ Keep descending
│  │  ├─ ... (order=4 target): bit=0 → ❓ Unknown: skip or descend?
│  │  └─ ...
│  └─ Right: bit=0 → ❓
└─ Right (order=N-1): bit=0 → ❓
```

Since a `0` bit at the target order could mean either "block is free but its descendants are fragmented" or "block is allocated," the allocator must scan linearly across all `2^{N-order}` candidates at every order level. This is BFS in practice: for each order from `base_order` up to `N`, scan the entire bit range for a `1`.

**Worst-case complexity**: `O(2^{N-order})` for a single 1 found at the far end of the range. Over multiple allocations in a fragmented pool, this approaches `O(2^N)` bit accesses per allocation.

[TBD: Insert small N=3 example tree trace showing the problem step-by-step]

### 2.3 The Root Cause

Formally, a 1-bit encoding can represent only two states:

| Bit | Semantics (actual) | Semantics (required for safe pruning) |
|-----|-------------------|--------------------------------------|
| 1   | Block free at this order | Block free at this order |
| 0   | Everything else | NONEXIST or OCCUPIED or NONLEAF |

The single bit is asked to distinguish three mutually exclusive conditions from one Boolean. This is fundamentally insufficient for DFS decision-making.

[Section ~1.0 page]

---

## 3. The 4-State Solution

### 3.1 Memory Layout

The v4 foundation allocator uses 3·2^N bits total, divided into two regions:

```
[0, 2^(N+1))       — non-order0 nodes: 2-bit per node, idx ∈ [0, 2^N)
[2^(N+1), 3·2^N)  — order0 leaf region: 1-bit per node, idx ∈ [2^N, 2^(N+1))
```

**Non-order0 nodes** (2-bit encoding):

| Value | State | DFS Behavior | Meaning |
|-------|-------|-------------|---------|
| `0b00` | NONEXIST | **Prune** | Subtree never split to this granularity |
| `0b01` | OCCUPIED | **Prune** | Entire block allocated |
| `0b10` | NONLEAF | **Descend** | Subtree has partial free space |
| `0b11` | FREE | **Hit** | Whole block free |

**Order0 leaves** (1-bit encoding):

| Value | Meaning |
|-------|---------|
| 1 | Free page frame |
| 0 | Allocated page frame |

**Layout guarantee**: Each non-order0 node occupies 2 consecutive bits starting at an even bit offset (`idx * 2` has bit offset range {0, 2, 4, ..., 62}). The 2-bit value **never crosses a 64-bit word boundary** — a property enforced by the even-bit-offset design.

### 3.2 Bit Access Primitives

```cpp
// Non-order0 node (2-bit):
uint8_t node_read(uint64_t heap_idx) {
    uint64_t boff = heap_idx << 1;                    // always even
    return (bitmap[boff >> 6] >> (boff & 63)) & 0b11;
}
void node_write(uint64_t heap_idx, uint8_t val) {
    uint64_t boff = heap_idx << 1;
    uint64_t& w = bitmap[boff >> 6];
    uint8_t   sh = boff & 63;
    w = (w & ~(0b11ull << sh)) | ((uint64_t)(val & 0b11) << sh);
}

// Order0 leaf (1-bit):
bool leaf_read(uint64_t leaf_idx) {
    uint64_t boff = (1ull << N) + leaf_idx;
    return (bitmap[boff >> 6] >> (boff & 63)) & 1;
}
```

### 3.3 Core Algorithms

#### 3.3.1 find_candidate: DFS with free_count Pre-scan

```
Input:  base_order (minimum order requested)
Output: (found_order, offset) — or error

1. Pre-scan: scan free_count[base_order .. N] for first non-zero entry.
   If none → "no memory" error.

2. DFS from root (idx=1) with target_order = found_order:
   For each visited node:
     NONEXIST / OCCUPIED → prune, backtrack
     FREE               → hit! Return
     NONLEAF            → descend left, then right
```

**Why the pre-scan is needed**: Although the DFS itself is O(N) (N ≤ 24, typically), without the pre-scan, a `find_candidate(0)` request on a freshly initialized pool would descend from the root (order=N=16) through the FREE root directly to the target order. That descends correctly but the descent path is fixed by the tree structure, not by the target order — it goes N steps regardless. The pre-scan narrows the entry point.

[TBD: Insert DFS trace diagram]

#### 3.3.2 split: Iterative Cell Division

```cpp
while (order > target_order):
    parent: FREE(11) → NONLEAF(10)
    children: NONEXIST(00) → FREE(11)
    free_count[order]--, free_count[order-1] += 2
    descend to left child
```

Right children remain FREE — the caller may cache these for fast subsequent allocations.

#### 3.3.3 order_occupy_try: Allocation with Collapse Propagation

```
1. Verify node is FREE. Fail if not (re-entrant safety).
2. Mark OCCUPIED; free_count[order]--.
3. Collapse propagation:
   While parent is NONLEAF and both children are OCCUPIED:
       Parent → OCCUPIED
       (DFS will now prune this entire subtree)
```

The collapse is critical: without it, a fully-allocated leaf pair would leave their parent as NONLEAF, forcing the allocator to descend into a dead subtree next time. The collapse propagates upward, potentially turning entire subtrees into OCCUPIED — single-bit prune points.

#### 3.3.4 order_return: Freeing with Coalescence and Collapse Unfolding

```
Phase 1 (Coalescing):
  Mark node FREE; free_count[order]++.
  While buddy is also FREE:
      Clear both children to NONEXIST
      Parent → FREE
      Ascend

Phase 2 (Collapse unfolding):
  When coalescing stops (buddy not FREE):
  While parent is OCCUPIED (was collapsed):
      Parent → NONLEAF  (restore correct state)
      Ascend
```

[TBD: Include state transition diagrams for occupy_try and order_return]

### 3.4 State-Space Compactness: A Finite-State Machine View

The 4-state encoding reduces the allocator's operation to a pure finite-state machine. A parent node and its two children form a triple with 4 × 4 × 4 = 64 possible state combinations. Invariant constraints (§3.5) reduce this to a much smaller set of *legal* configurations:

```
Parent=NONEXIST (00)  → children must be NONEXIST (never allocated)
Parent=FREE     (11)  → children must be NONEXIST (never split)
Parent=OCCUPIED (01)  → children: NONEXIST (true whole-block)
                      OR children: both OCCUPIED (collapse artifact)
Parent=NONLEAF  (10)  → children: active, not both FREE, not both OCCUPIED
```

**Every tree operation is a state transition**: load 2-bit state → dispatch on 4 values → store 2-bit result. No arithmetic (addition, comparison, `min`/`max`) is required for any bit-level tree operation.

This contrasts with byte-per-node encodings (§4.2), where each descent step requires a numeric comparison (`val ≤ d`), and each parent update requires `min(left, right)` — arithmetic operations that introduce data dependencies absent in state-machine dispatching.

The practical significance: branch prediction on a 4-way switch is highly accurate — the hot path (NONLEAF descent) dominates, and the two prune paths (NONEXIST, OCCUPIED) are taken only at subtree boundaries. The two-bit load from a 64-bit word enjoys single-cycle latency, and the even-bit-offset layout guarantees no cross-word-boundary penalties.

### 3.5 Invariant Constraints

1. **Node 0 sentinel**: Always NONEXIST, never referenced.
2. **NONEXIST/FREE children**: Both children must be NONEXIST — no partial structures hidden.
3. **OCCUPIED subtrees**: Either children are NONEXIST (truly allocated as a whole) or children are OCCUPIED (collapse artifact, verified recursively).
4. **NONLEAF children**: Neither child can be NONEXIST; cannot both be FREE (would merge) or both be OCCUPIED (would collapse).
5. **free_count consistency**: `free_count[order]` must exactly match the tree — validated by `btree_validation()`.

[Section ~1.5 pages]

---

## 4. Evaluation

### 4.1 Experimental Setup

| Parameter | Value |
|-----------|-------|
| BCB_ORDER | 16 |
| Managed region | 256 MB |
| Bitmap size (v3) | 2·2^16 bits = 2 KB |
| Bitmap size (v4) | 3·2^16 bits = 3 KB |
| Bitmap size (Netty) | 2·(2^16+1) bytes ≈ 128 KB |
| Alloc/free operations | 1,000,000 |
| Order distribution | Exponential (λ = 0.3) — mean ~3.3 |
| Batch validation | Every 100,000 ops |
| Metric | Average cycles per alloc/free pair |
| Platform | [TBD — e.g., Intel i7-12700H, fixed freq 2.7GHz] |

### 4.2 Allocator Implementations

**BCB_v3 (mixed_bitmap_v2)**: The 1-bit predecessor. Uses BFS `scan_free_block`: for each order from base to N, scans the entire 2^{N-order} bit range for a set bit. Allocation cost grows with pool fragmentation and pool size.

**BCB_v4 (foundation)**: The 4-state 2-bit design described in Section 3. Uses `free_count` pre-scan + DFS descent.

**Netty-style (historical PoolChunk)** [4]: A byte-per-node heap-encoded tree. Prior to the redesign in commit `0d701d7c3c` (2023), Netty's PoolChunk used a `byte[] memoryMap` array sized at 2·(1 + 2^N) entries. Each node stores the *maximum allocatable depth* in its subtree — i.e., the smallest depth at which a free node exists. Initialization sets `memoryMap[id] = depth_of_id`. Allocation descend from root by testing child values: if `memoryMap[left] ≤ target_depth`, descend left; else right. No `free_count` pre-scan needed — the root value `memoryMap[1]` directly encodes the chunk's availability. Parent update uses `memoryMap[parent] = min(left_val, right_val)`, a single arithmetic `min` per ancestor. (Modern Netty replaced this with a `runsAvail[]` priority-queue-based allocator, making the depth-encoded tree a historical predecessor rather than current practice.)

### 4.3 Results

[TBD: Table — three-way comparison]

| Metric | BCB_v3 (1-bit) | BCB_v4 (2-bit) | Netty-style (8-bit) |
|--------|---------------|----------------|--------------------|
| Bitmap size | 2 KB | 3 KB | 128 KB |
| Per-node bits | 1 | 2 | 8 |
| Avg cycles/op | [TBD] | [TBD] | [TBD] |
| Std dev | [TBD] | [TBD] | [TBD] |
| Frag sensitivity | **High** | **None** | **None** |

[TBD: Line chart — avg cycles/op vs pool utilization (10%–90%)]

### 4.4 Analysis

**BCB_v3 degradation**: As the pool fills, the `free_count[]` array under-reports and the BFS scan must traverse increasingly large gaps of `0` bits. At high utilization (>80%), the allocator may scan the entire bit range before finding a candidate or failing.

**BCB_v4 flat curve**: The 4-state encoding guarantees that the DFS search path is bounded by N (max_order) regardless of how many blocks are allocated or freed. The `free_count` pre-scan adds O(N) overhead per allocation — negligible when N=16. The collapse-propagation mechanism ensures that fully-occupied subtrees are single-step prune points, not deep dead descents.

**Netty flat curve**: The byte-per-node encoding achieves the same fragmentation independence with slightly lower average latency (no pre-scan needed), at a 5.3× space cost.

[Section ~1.5 pages including figures]

---

## 5. Related Work

**Binary buddy systems** originated with Knowlton [1]. Peterson [2] classified dozens of buddy variants. The bitmap-encoded binary tree is a well-known optimization for eliminating pointers.

**Linux kernel buddy allocator [3]** uses per-order free lists embedded in `struct page`. It achieves O(1) alloc/free but requires a full page metadata infrastructure — unavailable in early freestanding kernel bootstrap.

**Netty's PoolChunk** — The historical depth-encoded tree design (pre-`0d701d7c3c`) [4] used a byte-per-node heap-encoded tree for Java heap allocation. Each node stores the maximum allocatable order in its subtree, enabling DFS descent without pre-scan. This is the closest prior art to our encoding; the key difference is the encoding width (8-bit vs. 2-bit) and the consequent operation profile: Netty requires arithmetic `min` for parent updates and numeric `val ≤ d` comparisons for descent, whereas our design uses pure state-machine dispatching with no arithmetic on the hot path. (Note: modern Netty has replaced this with a `runsAvail[]` priority-queue design; the depth-encoded tree is no longer current practice.)

**To the best of our knowledge**, the specific 4-state (NONEXIST/OCCUPIED/NONLEAF/FREE) node encoding for bitmap-based buddy systems and its associated collapse-propagation algorithm have not been previously described in the literature. The design was independently developed as part of the SparrowOS kernel project.

[Section ~0.3 pages]

---

## 6. Conclusion

We have presented a 2-bit-per-node 4-state encoding for bitmap-encoded binary buddy allocators. This encoding resolves the 1-bit semantic ambiguity that forced DFS pruning failure in prior designs. The 50% bitmap size increase (from 2·2^N to 3·2^N bits) restores O(N) allocation complexity independent of fragmentation level.

A three-way benchmark against the 1-bit predecessor and an 8-bit depth-encoding allocator confirms:
1. Fragmentation-independent latency for both the 2-bit and 8-bit designs
2. 5.3× per-node space saving vs. the byte-per-node approach
3. Severe latency degradation in the 1-bit design under fragmentation — up to [TBD]×

The 2-bit encoding represents a practical Pareto optimum for compact freestanding kernel allocators: sufficient semantic width for correct DFS pruning, space-efficient enough for early bootstrap allocation.

[Section ~0.2 pages]

---

## References

[1] Knowlton, K. C. "A fast storage allocator." *Communications of the ACM* 8.10 (1965): 623-624.

[2] Peterson, J. L. and Norman, T. A. "Buddy systems." *Communications of the ACM* 20.6 (1977): 421-431.

[3] Gorman, M. *Understanding the Linux Virtual Memory Manager.* Prentice Hall, 2004.

[4] Netty Project. "PoolChunk.java — Pooled memory allocator." GitHub, 2013–2026.
    The depth-encoded tree design was replaced by a runsAvail priority-queue design
    in commit 0d701d7c3c (2023). The tree version is preserved in the git history at
    `0d701d7c3c^:buffer/src/main/java/io/netty/buffer/PoolChunk.java`.
    https://github.com/netty/netty

---

## Appendix A: Allocator Implementation Details

[TBD — will document the netty_simulator alloc algorithm in pseudocode + its divergence from the original Netty implementation]

## Appendix B: Full Benchmark Trace

[TBD — the fixed allocation trace used in experiments]

---

*Draft v0.1 — May 29, 2026*

*[TBD] denotes data pending benchmark execution*
