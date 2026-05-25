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
| kernel_anonymous | 分配器描述符 / slab cache 指针 | 内核动态分配 |
| kernel_persist / kernel_pinned / free / dma | 无意义（忽略） | 仅 state 字段有效 |

---

## 四、48 字节扩展元数据 (page_cache_node_t)

### 4.1 只有部分页面需要元数据

| page_state | 所需结构 | 理由 |
|---|---|---|
| free | 仅有 state | 只需知道可用 |
| kernel_persist | 仅有 state | 固定不可移动，无需管理 |
| kernel_pinned | 仅有 state | 锁定不可换出 |
| dma | 仅有 state | DMA 缓冲，非 LRU 管理 |
| **user_file** | **page_cache_node_t** | 需要 LRU / refcount / dirty / owner |
| **user_anonymous** | **page_cache_node_t** | 需要 LRU / refcount / map_count / owner |
| **kernel_anonymous** | **page_cache_node_t** | 需要 refcount / owner |

### 4.2 布局推导

```
字段              大小    用途
─────────────────────────────────────
meta (page_v2)     8B     state + owner + order
next               8B     LRU 链表后继
prev               8B     LRU 链表前驱
flags              8B     PAGE_DIRTY / PAGE_LOCKED / ... (8 个定义位)
offset_of_file     8B     文件块索引 / VMA 内偏移
refcount           4B     引用计数
map_count          4B     PTE 映射数
                   ───
                   48B
```

### 4.3 order ≥ 3 的下界

为什么 `page_cache_node_t` 只用于 order ≥ 3（≥ 8 页 = 32KB）的页块？

```
元数据开销 = sizeof(page_cache_node_t) / (pages * 4096)
order 0 (1 页):   48 / 4096   = 1.17%
order 3 (8 页):   48 / 32768  = 0.15%
order 9 (512 页): 48 / 2M     = 0.0023%
```

order 0~2 的页面太小，摊 48B 元数据浪费。
单页匿名映射（`mmap` 一个 4KB）不做 cache 管理，直接分配释放。

### 4.4 字段取舍决策过程

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

物理地址由本节点在 mem_map 中的索引推算：

```
phyaddr = mem_map_pbase + mem_map_idx * 4096
```

VM_DESC 中偏移 `vaddr_offset` 的虚拟区间，映射到本页的物理内存。
区间范围由 order 决定：

```
virtual range:
    [VM_DESC.start + vaddr_offset * 4096,
     VM_DESC.start + vaddr_offset * 4096 + (1 << (12 + meta.order)))
```

---

## 五、迁移计划

### 5.1 mem_map 数组类型变化

```
旧: mem_map = page[]        (1B/entry)
新: mem_map = page_v2[]     (8B/entry)
```

mem_map 的内存分配量需要放大 8 倍。

### 5.2 page_cache_node_t 的存储

对于 `user_file` / `user_anonymous` / `kernel_anonymous` 页块，
head page 的 `mem_map[base_idx]` 存储 `page_v2`，后续的 `page_cache_node_t` 剩余字段
（next/prev/flags/offset/refcount/map_count）存在连续区域。

两种实现选择（待定）：
- **内联**：直接紧随 `page_v2` 之后占 40B（5 个连续 mem_map 槽）
- **外挂**：单独分配 `page_cache_node_t` 数组，mem_map 存指针

### 5.3 访问接口迁移

```
旧: mem_map[idx].state = page_state_t::free;
新: mem_map[idx].fields.state = page_state_t::free;
```

所有 mem_map 访问者需要更新。

---

## 六、与其他子系统的接口

| 子系统 | 需要的 page 信息 | 提供的 page 信息 |
|---|---|---|
| FreePagesAllocator (BCB) | state=free 的页框 | 管理空闲区间 |
| LRU kthread | page_cache_node_t::next/prev/refcount/flags | 回收候选页 |
| swap (文件/块设备) | owner(inode*) + offset + dirty | 回写 / 换出 |
| TLB flush (per-AS IPI) | owner(VM_DESC*) + pgd | 刷新页表 |
| COW (page fault handler) | map_count | 决定 copy-on-write |
