# page_v2 与 page_cache_node_t 设计演进记录

日期: 2026-05-25  
涉及文件: `src/include/memory/memory_base.h`

---

## 一、起点：struct page 的局限

原有设计：

```cpp
struct page {
    page_state_t state;        // 1 byte
};
static_assert(sizeof(page) == 1);
```

mem_map 是 `page[]` 数组，每个物理页框对应 1 字节，只记录页框状态。

**无法满足的需求：**
- swap 换出需要知道页面的 owner 和文件偏移
- LRU 回收需要双向链表节点
- PTE 映射数决定 COW / 共享策略
- refcount 决定是否可以释放

---

## 二、设计目标

| 需求 | 优先级 | 约束 |
|------|--------|------|
| 页面状态编码 | ★★★ | 至少 8 种状态（当前已有 7 个 enum 成员） |
| owner 指针（inode / VM_DESC / ...） | ★★★ | 找到回写目标 / 逆向映射 |
| 页框阶数（order） | ★★★ | 大页 / 透明页支持 |
| LRU 链表节点 | ★★★ | next/prev 指针 |
| refcount | ★★★ | 安全释放 |
| map_count | ★★★ | COW / 共享判定 |
| flags (dirty, locked, ...) | ★★★ | 控制换出、回写行为 |
| swap 位置 | ★ | 被否定——见下文 |

**关键决策：swap 字段不应该在 page 里。**

原因是：swap out 后该物理页框被归还 free pool，`mem_map[idx]` 的整个元数据
被下一个分配者覆盖。swap entry 存在 PTE 的 non-present 编码里，或在 owner 的 page tree 里——这是 owner 的责任，不是 page 的责任。
（感谢指正。）

---

## 三、64-bit 紧凑编码 (page_v2)

### 3.1 位域分配

```cpp
struct page_v2 {
    struct {
        uint64_t state:4;           // page_state_t, 最多 16 种
        uint64_t vaddr_compact:52;  // 内核指针 [55:4], 16B 对齐约束
        uint64_t order:6;           // 页框阶 (0=4KB, 3=32KB, ...63)
        uint64_t reserved:2;
    } fields;
    uint64_t raw;
};
```

### 3.2 为什么 vaddr_compact 是 52 位

- 内核虚拟地址在 SparrowOS 中固定 16 字节对齐（bits [3:0] = 0）
- 所以指针编码时去掉低 4 位：`compact = ptr >> 4`
- 解码时左移还原：`ptr = (void*)(compact << 4)`
- 52 位覆盖了 56 位虚拟地址空间（48 位实际 + 第 4 级页表空间），绰绰有余

### 3.3 vaddr_compact 的语义

`vaddr_compact` 不一定是虚拟地址——它是 owner 对象的指针。
因为不是所有页面都有一个固定虚拟地址关联。

| page_state | vaddr_compact 编码的指针 | 语义 |
|---|---|---|
| user_file | `inode*` | 文件所有者 |
| user_anonymous | `VM_DESC*` | 所属 VMA 描述符 |
| kernel_anonymous | 暂定 | 内核动态分配的，允许触发页错误的，会有handler兜底的VM_DESC下的内存区间 |
| kernel_persist / kernel_pinned / free / dma | 无意义（忽略） | 仅 state 字段有效 |

---

## 四、64 字节扩展元数据 (page_cache_node_t)

### 4.1 只有部分页面需要元数据

| page_state | 所需结构 | 理由 |
|---|---|---|
| free | 仅有 state | 只需知道可用 |
| kernel_persist | 仅有 state | 固定不可移动，无需管理 |
| kernel_pinned | 仅有 state | 锁定不可换出 |
| dma | 仅有 state | DMA 缓冲，非 LRU 管理（NVMe admin 缓冲区/HMB） |
| **user_file** | **page_cache_node_t** | 需要 LRU / refcount / dirty / owner |
| **user_anonymous** | **page_cache_node_t** | 需要 LRU / refcount / map_count / owner |
| **kernel_anonymous** | **page_cache_node_t** | 需要 refcount / owner |

### 4.2 布局推导

```
字段              大小    用途                     mem_map 偏移
──────────────────────────────────────────────────────────────
meta (page_v2)     8B     state + owner + order     [base + 0]
next               8B     LRU 链表后继              [base + 1]
prev               8B     LRU 链表前驱              [base + 2]
flags              8B     PAGE_DIRTY / PAGE_LOCKED  [base + 3]
offset_of_file     8B     文件块索引 / VMA 内偏移     [base + 4]
refcount           4B     引用计数                   [base + 5]（低4B）
map_count          4B     PTE 映射数                 [base + 5]（高4B）
reserved[0]        8B     预留                       [base + 6]
reserved[1]        8B     预留                       [base + 7]
                   ───
                   64B
```

### 4.2a 为什么 pad 到 64B 而不是停在 48B

order=3 的最小块是 8 页，对应 8 个 mem_map 连续槽位（8 × 8B = 64B）。
48B 只占 6 个槽，剩下 2 个槽若是碎片会平添逻辑成本——每次分配都需要区分
这块 metadata 占了多少页框——是 6 个还是 8 个。
统一 pad 到 64B（8 槽），开销仅 0.05%（16B / 32KB），换来规律性和 16B 的 future 扩展空间。

### 4.3 字段取舍决策过程

| 候选字段 | 纳入？ | 理由 |
|---|---|---|
| refcount | ✅ | 必须。没有 refcount 不能安全 free/swap/COW |
| map_count | ✅ | 必须。决定是否共享、COW 策略 |
| dirty/locked 等 flags | ✅ | 必须。控制换出行/写回行为 |
| owner 指针 | ✅ | 通过 vaddr_compact 编码，零额外开销 |
| offset_of_file | ✅ | 文件内定位 / VMA 内偏移 |
| swap_location | ❌ | swap out 后元数据消亡，swap entry 在 PTE 中 |
| lru_tick / generation | ❌ | Clock 算法不需要 tick（load/store 不更新） |
| private_data | ❌ | 用途不明确，暂时不加，以后可复用 reserved 位 |

### 4.5 匿名页语义

`state=user_anonymous` 时：

```
vaddr_compact → VM_DESC*  （所属 VMA 描述符）
offset_of_file → vaddr_offset = (vaddr - VM_DESC.start) >> 12
```

物理地址通过 all_pages_arr 的 phyinterval_t 链表推算（见 all_pages_arr.h）：

```
mem_map 不是按 PFN 线性索引的。它由 phyinterval_t 描述若干个自由物理内存区间。
每个区间记录:
  base              — 物理基址
  baseidx_in_memmap — mem_map 数组中的基索引
  numof4kbpgs       — 区间内的 4KB 页数

mem_map[idx] → phyaddr:
    for each interval:
        if idx ∈ [interval.baseidx_in_memmap, interval.baseidx_in_memmap + interval.numof4kbpgs):
            offset = idx - interval.baseidx_in_memmap
            phyaddr = interval.base + offset * 4096
            break

phyaddr → mem_map[idx]:
    for each interval:
        if phyaddr ∈ [interval.base, interval.base + interval.numof4kbpgs * 4096):
            offset = (phyaddr - interval.base) / 4096
            idx = interval.baseidx_in_memmap + offset
            break
```

VM_DESC 中偏移 `vaddr_offset` 的虚拟区间，映射到本页的物理内存。
区间范围由 order 决定：

```
virtual range:
    [VM_DESC.start + vaddr_offset * 4096,
     VM_DESC.start + vaddr_offset * 4096 + (1 << (12 + meta.order)))
```

---



---

## 五、两个世界共存

### 5.1 旧世界：struct page（不动）

`struct page { page_state_t state; }`（1B）长久保留。

当前所有 mem_map 使用者（page_allocator / all_pages_arr / FreePagesAllocator）
继续用旧的 page[] 数组和 `.state` 访问。它们只关心页框状态，不需要 metadata。

```cpp
// 旧接口不变，一切照旧
page* mem_map = ...;
mem_map[idx].state = page_state_t::free;
```

### 5.2 新世界：page_v2 / page_cache_node_t（蓝图）

`page_v2` 和 `page_cache_node_t` 是平行于旧体系的新类型定义。
新模块（LRU kthread / swap 层 / per-AS TLB shootdown）使用新类型，不改旧代码。

已知 mem_map 当前是 `page[]`（1B/entry），不修改。
新世界的 page_v2 数组可以另建，或把 page_cache_node_t 嵌入到物理页框头（待定）。

```cpp
// 新类型定义已就绪，仅蓝图，old world 不受影响
// 切换时机：等新模块上线时独立分配数组
```

## 六、与其他子系统的接口

| 子系统 | 需要的 page 信息 | 提供的 page 信息 |
|---|---|---|
| FreePagesAllocator (BCB) | state=free 的页框 | 管理空闲区间 |
| LRU kthread | page_cache_node_t::next/prev/refcount/flags | 回收候选页 |
| swap (文件/块设备) | owner(inode*) + offset + dirty | 回写 / 换出 |
| TLB flush (per-AS IPI) | owner(VM_DESC*) + pgd | 刷新页表 |
| COW (page fault handler) | map_count | 决定 copy-on-write |
