# LRU / SWAP 设计演进记录

日期: 2026-05-26  
涉及文件: `memory_base.h` / `AddresSpace.cpp`  
状态: v0 草案（未实现）

---

## 一、背景与设计约束

### 1.1 page_cache_node_t 的制约

`page_cache_node_t` 占用 64 字节物理内存，存储在物理页框的 mem_map 槽位中。
由于 64 字节 = 8 个连续的 mem_map 槽（每槽 8B），一个 `page_cache_node_t`
只能挂载在 **order ≥ 3（32KB）** 的页框上。这是所有 swap 操作的最小粒度。

**推论**：swap 分配/回收槽单位 = 32KB，不可更小。

### 1.2 swap 设备的复用

实体机测试时直接复用 Linux 已就绪的 swap 分区（`nvme0n1p6`, 8GB）。
SparrowOS 不完全兼容 Linux 的 swap 格式——只复用磁盘布局的 header 部分
用于校验魔数和版本，数据分配规划完全独立设计。

### 1.3 关键时序问题

2026-05-26 下午讨论中逐步澄清的时序问题：

- **IPI 竞态**：全局 TLB shootdown 是全局事务，用全局锁串行即可。详见
  `Docs/arch/x86_64/Interrupt/IPI设计思路与演进记录.md`
- **PTE 读竞态**：Linux 用 `ptep_get_lockless` 无锁读 PTE，判断是否为 swap entry。
  读 PTE 不需要 PTL（x86 上单条写入原子性保证）。修改 PTE 需要 PTL。
- **页表锁层级**：Linux 分三层——mmap_lock（VMA 级）→ PTL（per-page-table-page）
  → page lock（PG_locked）。SparrowOS 初期只用 `AddressSpace::lock` 充当
  mmap_lock，不实现 per-PTE page table lock，待需要时再引入。
- **swap-in 抢锁竞态**：swap-in 拿到 pg_lock 时，free_buddy_way 不可能已经发生
  ——因为 swap cache 的 refcount 屏障保证了物理页仍存活。
  防御性检查应检查 refcount > 0，而非 meta.state != free。

---

## 二、page_cache_node_t 自旋锁决策

### 2.1 讨论过程

讨论了两种方案：

**方案 A：Linux 式 bit spinlock**

在 `flags` 字段（uint64_t，只用了低 8 bit）的 bit 0 实现位自旋锁，
复用现有字段，不额外占空间。用 `__atomic_test_and_set` 和 `__atomic_clear_bit`
实现 `page_trylock`/`page_lock`/`page_unlock`。

优点：不占额外空间，一条 `LOCK BTS` 指令完成加锁。
缺点：flags 上下文必须小心不覆盖 bit 0；语义不够直白。

**方案 B：独立 spinlock 字段（选中）**

`page_cache_node_t` 原 `reserved[2]`（16B）中取 1 字节放置 `spinlock_cpp_t pg_lock`，
剩余 15 字节保留。

优点：可读性好，lock/unlock 通过标准函数接口调用，无位操作开销，
flags 字段保持干净。
缺点：多占 1 字节空间（但 reserved 本就空闲，无实际成本）。

**最终决策**：资源足够时可读性最重要，选择方案 B。

### 2.2 改动后的布局

```cpp
struct page_cache_node_t{
    page_v2 meta;                //  8B
    page_cache_node_t* next;     //  8B
    page_cache_node_t* prev;     //  8B
    uint64_t flags;              //  8B
    uint64_t offset_of_file;     //  8B
    uint32_t refcount;           //  4B
    uint32_t map_count;          //  4B
    spinlock_cpp_t pg_lock;      //  1B  ← 取自 reserved[0]
    uint8_t  reserved_pad[15];   // 15B  ← reserved[2] 被拆
};
static_assert(sizeof(page_cache_node_t) == 64);
```

---

## 三、SWAP 磁盘布局

### 3.1 设计考虑

Linux swap 的分槽粒度为 `PAGE_SIZE`（4K）每个 slot 存在 swap_map 中占 1B。
SparrowOS 因 page_cache_node_t 制约，槽粒度必须是 32K。

**比特图 vs 引用计数数组**的讨论：

| 方案 | 每槽开销 | 8G swap 总开销 | 分配策略 |
|------|---------|---------------|---------|
| Linux 式 swap_map (1B/槽) | 8 bit | 2MB | 顺序扫描 1B 一次 |
| 比特图 (1bit/32K) | 1 bit | 32KB | 64-bit 扫描覆盖 2MB |

**决策**：使用比特图。32KB 全内存驻留完全可行，64-bit 一次扫描覆盖 2MB
对齐内存页的 cacheline 粒度。

### 3.2 磁盘结构

```
分区布局 (8G swap 为例):

页  0:       Linux swap header  (4K)
                └─ 只读 magic[10] == "SWAPSPACE2" 验证
                └─ version == 1
                └─ last_page 记录总页数

页  1..8:    SparrowOS bitmap  (8 页 = 32KB)
                └─ 1 bit 对应 1 个 32K 槽
                └─ 262,144 槽 ÷ 8 = 32,768 字节 = 32K
                └─ bit = 0: 空闲, bit = 1: 占用

页  9..N-1:  数据槽 (32K 粒度)
                └─ slot 0 = 偏移 9×4K = 36K
                └─ slot 1 = 偏移 36K + 32K
                └─ ...
```

### 3.3 bitmap 纯内存，不入盘

swap 是内存的延伸，断电即废。bitmap 只存在于运行时：
- swapon: `kzalloc(32KB)` 全零初始化，所有槽空闲
- swapoff: `kfree(bitmap)`
- 不读写磁盘，无同步策略，无 dirty 标记

与 Linux 的 `swap_map` swapon 时清零是同一道理。

### 3.4 接口设计（草案）

```cpp
// 物理设备抽象
struct swap_device {
    struct block_dev *bdev;      // 块设备
    uint64_t          slot_count; // 32K 槽总数
    uint8_t          *bitmap;     // 32KB 比特图（内存驻留，swapon kzalloc，swapoff kfree）
    spinlock_cpp_t    lock;
    // ... LRU 链表头 ...
};

// 核心操作
int  swap_alloc_slot(swap_device *sd);
void swap_free_slot(swap_device *sd, int slot);
int  swap_read_slot(swap_device *sd, int slot, void *buf);
int  swap_write_slot(swap_device *sd, int slot, void *buf);

// swapon / swapoff
KURD_t swap_device_init(swap_device *sd, struct block_dev *bdev);
KURD_t swap_device_fini(swap_device *sd);
```

---

## 四、page_cache_node_t 生命周期（SWAP 路径）

### 4.1 状态图

```
                  ┌──────────────────────────┐
                  │  正常使用 (LRU 活跃/非活跃) │
                  │  state = user_file 或     │
                  │  state = user_anonymous   │
                  └──────────┬───────────────┘
                             │ kswapd 选中回收
                             ▼
                  ┌──────────────────────────┐
                  │  换出管道中               │
                  │  PG_SWAPPING = 1         │
                  │  PG_WRITEBACK = 可能     │
                  │  → unmap_page (PTE→swap) │
                  │  → writeback (若 dirty)  │
                  └──────────┬───────────────┘
                             │ I/O 完成, PG_WRITEBACK 清除
                             ▼
                  ┌──────────────────────────┐
                  │  就绪释放                 │
                  │  refcount == 0           │
                  │  list_del(LRU)           │
                  │  meta.state = free       │
                  │  free_buddy_way(base)    │
                  └──────────┬───────────────┘
                             │ 此后 page_cache_node_t 被 buddy
                             │ 系统覆盖，不复存在
                             ▼
                  ┌──────────────────────────┐
                  │  mem_map 被下一任分配者覆盖 │
                  │  page_cache_node_t 消亡   │
                  └──────────────────────────┘
```

### 4.2 swap-in 抢回路径

```
swap-in: 看见 PTE = swap entry
  ├─ swap_cache_lookup(entry): 查阅 swap cache
  │   ├─ 命中: page 还在物理内存（writeback 中或刚完成未 free）
  │   │   └─ pg_lock(page) 等 I/O 完成
  │   │   └─ PTE 恢复 present, 清除 PG_SWAPPING
  │   │   └─ 从 swap cache 移除
  │   │   └─ page_cache_node_t 继续存活
  │   │
  │   └─ 未命中: page 已被 free_buddy_way 回收
  │       └─ 从磁盘读回数据 → 分配新 page_cache_node_t
  │       └─ PTE 恢复 present
  └─ 完成
```

### 4.3 安全保证

swap cache 的引用计数保证：
- swap_cache_add(page) → refcount++
- free_buddy_way 要求 refcount == 0
- swap-in 拿到 swap cache 命中的 page → refcount > 0 → free 不可能发生
- pg_lock 拿到后，page_cache_node_t 必然存活

防御性检查应当检查 `refcount > 0`，而非 `meta.state != free`。

---

## 五、LRU 回收流程（草案）

### 5.1 BCB 扩展

```cpp
// FreePagesAllocator::BuddyControlBlock 新增字段
struct BuddyControlBlock {
    // ... 现有 bitmap / cache / fnd / lock ...

    // ── LRU 链表 ──
    page_cache_node_t* lru_active_head;     // 活跃链表
    page_cache_node_t* lru_inactive_head;   // 非活跃链表
    uint64_t nr_active;
    uint64_t nr_inactive;
};
```

### 5.2 回收流程

```
kswapd kthread 流程:

for each BCB:
    if !bcb.lock.try_lock(): continue   // 不阻塞 alloc

    // 阶段 1: 活跃→非活跃旋转
    scan(active_list):
        for each page:
            if PAGE_REFERENCED:
                clear REFERENCED          // 二次机会
                move_to_tail(active)
            else:
                clear ACTIVE
                move_to(inactive)

    // 阶段 2: 回收非活跃页
    scan(inactive_list, target=N):
        for each page:
            if refcount > 0: continue
            if PAGE_REFERENCED:
                clear REFERENCED
                move_to_active(page)
                continue

            if PAGE_DIRTY and PAGE_UPTODATE:
                submit_writeback(page.owner, page.offset_of_file)
                set WRITEBACK
                continue                  // 等 I/O 完成后回 loop 重检

            // 可回收
            pg_lock(page)
            list_del(page)
            unmap_page(page)              // PTE → swap entry
            swap_cache_add(page)          // ref++
            swap_write(page_data)         // 写磁盘
            swap_cache_del(page)          // ref--
            atomic_set_state(page, free)
            pg_unlock(page)
            free_buddy_way(base, order)   // page_cache_node_t 消亡
            nr_inactive--

    bcb.lock.unlock()
    schedule()
```

### 5.3 LRU flag 定义

```cpp
// 在 page_cache_node_t::flags 中已占用的 bit
constexpr uint64_t PG_DIRTY      = 1ULL << 0;   // 写入过
constexpr uint64_t PG_WRITEBACK  = 1ULL << 1;   // 回写 IO 中
constexpr uint64_t PG_UPTODATE   = 1ULL << 2;   // 内容有效
constexpr uint64_t PG_SWAPPING   = 1ULL << 3;   // 换出管道中
constexpr uint64_t PG_REFERENCED = 1ULL << 4;   // 最近访问
constexpr uint64_t PG_ACTIVE     = 1ULL << 5;   // 活跃 LRU
constexpr uint64_t PG_RECLAIM    = 1ULL << 6;   // 回收进行中
// bit 7+ 空闲
```

---

## 六、与 IPI 框架的接口

### 6.1 全局 TLB shootdown

swap-out 路径中的 `unmap_page(page)` 需要广播 TLB shootdown
（因为被换出的页可能在任意 CPU 的 TLB 中，即使是内核态页）。

使用 IPI 三层架构的 Layer 2（固定向量 `IPI_TLB` + 全局锁串行）：

```
lock(global_ipi_lock)
    set pending_mask = 全 1
    sfence
    broadcast_all_exself_IPI(IPI_TLB)
    wait_all_acks(pending_mask == 0)
unlock(global_ipi_lock)
```

详见 `Docs/arch/x86_64/Interrupt/IPI设计思路与演进记录.md`。

### 6.2 per-AS TLB shootdown（暂缓）

用户空间的 per-AS TLB shootdown 依赖 swap 路径稳定后再设计。
前述 swap-in 路径中的 PTE 修改涉及 per-AS 页表，但初期用全局 TLB
shootdown 代替（安全但过度杀伤）。

---

## 七、未解决问题

1. **kswapd 唤醒条件**：当 free_pages 低于 watermark，或某 BCB 的 free 页
   低于 min 阈值时唤醒。具体阈值待定。

2. **multi-kswapd**：多核并行回收不同 BCB 是自然的，但单 BCB 不可并行。
   初期单 kswapd kthread 即可。

3. **跨 BCB 页面**：一个文件 inode 的缓存页可能散落在多个 BCB 上。
   kswapd 在一个 BCB 里回收其所属页，其他 BCB 里的同文件页不受影响。
   按物理分区回收的固有特性，不是 bug。

4. **PAGE_REFERENCED 谁设**：当前没有硬件访问位软更新。
   方案一：在缺页/扫描路径中由软件置位。
   方案二：kswapd 进入 inactive 扫描时设置 accessed 位，等待下次缺页指示。

5. ~~bitmap 同步可靠性~~：已解决。bitmap 是纯运行时结构，swapon 时自动清零，
   不存在

6. **swap 设备热插拔**：swapoff 时需确认所有关联 page_cache_node_t 已释放。
   设计还未考虑设备热移除路径。

7. **hibernation/resume**：断电后 swap 数据无效，bitmap 不持久不是问题。
   但未来若需 suspend-to-disk，则 bitmap + 元数据都需持久化——暂不考虑。

---

## 附录：讨论过程记录

### 2026-05-26 下午

- **16:21** — 讨论 Linux PTL 层级结构。确认 SparrowOS 初期用
  `AddressSpace::lock` 充当 mmap_lock，不引入 per-PTE lock。
- **16:26** — 讨论 swap-in 路径的取锁顺序：读 PTE 判 swap entry 不需要 PTL
  （`ptep_get_lockless`），VMA 锁由 fault handler 入口持有。
- **16:42** — 决策 `page_cache_node_t` 自旋锁方案。否决 Linux 式 bit spinlock，
  选择独立 `spinlock_cpp_t pg_lock` 吃 reserved[0]。
- **16:54** — 讨论 Linux swap 磁盘格式。确认复用 Linux swap header 做魔数校验，
  数据分配独立设计（bitmap 替代 byte array）。
- **17:13** — 讨论 page_cache_node_t 在 swap-out 后的生命周期。确认：
  free_buddy_way 是终结，之前 PG_SWAPPING + PG_WRITEBACK 标记管道中间态，
  之后不保留任何状态。
- **17:25** — 讨论 swap-in 抢锁竞态：refcount 屏障保证 free_buddy_way 不会
  在持 pg_lock 期间发生。
- **17:44** — 确认复用实体机 `nvme0n1p6` (8G swap 分区)。
- **17:55** — 决策 swap bitmap 方案：1 bit / 32K 槽，64-bit 扫描覆盖 2MB。
