# FPA 污染模型 (v3)

## 1. 概述

`FreePagesAllocator`（FPA）是内核物理内存分配器的顶层管理组件，负责管理多个 `BuddyControlBlock`（BCB）实例。FPA 层本身不直接管理内存块的空闲状态，而是将具体工作委托给 BCB，自身专注于 BCB 的选择、状态机管理、污染标记和失败重试。

**污染模型**解决的核心问题：init.elf 分配的**瞬态区间**（log_buffer、symtable_file、initramfs_file、kernel_entry_stack）在移交给 kernel.elf 时，其物理页在 `all_pages_arr` 的 `phymem_segment[]` 中被标记为 `free`，但实际内容仍有有效数据。若 BCB 在重建空闲池时将这些段纳入管理并分配出去，会导致踩踏活跃数据。

**解决手段**：通过 per-BCB 的 `dirty_count` 标记，将瞬态区间所在的 BCB 标记为"脏"，禁止分配；当外部模块处理完数据后（搬走或丢弃），再清除脏标记，归还空闲。

## 2. 状态机

FPA 是一个有限状态机，只有两个状态：

```
       Init()
          │
          ▼
   ┌─────────────┐
   │   SEED      │  BCB 已构造完成，位图已初始化
   │             │  仅 interval_pollute 可用
   └──────┬──────┘
          │ activate()
          │ (显式切换，不可逆)
          ▼
   ┌─────────────┐
   │   ACTIVE    │  alloc/free/interval_clean 可用
   └─────────────┘
```

```cpp
enum fpa_state_t : uint8_t {
    FPA_STATE_SEED,    // Init 完成，仅 interval_pollute 可用
    FPA_STATE_ACTIVE   // activate() 后，alloc/free/interval_clean 可用
};
```

### 2.1 各状态下接口的可调用性

| 接口 | SEED | ACTIVE | 行为 |
|------|------|--------|------|
| `Init()` | — | — | 调用一次，成功后进入 SEED |
| `interval_pollute()` | ✅ | ❌ | ACTIVE 下调 → `Panic::panic(... CALL_VIOLATION)` |
| `activate()` | ✅ | — | ACTIVE 下调 → 静默无操作返回 |
| `alloc()` | ❌ | ✅ | SEED 下调 → `Panic::panic(... CALL_VIOLATION)` |
| `free()` | ❌ | ✅ | SEED 下调 → `Panic::panic(... CALL_VIOLATION)` |
| `interval_clean()` | ❌ | ✅ | SEED 下调 → `Panic::panic(... CALL_VIOLATION)` |

所有状态违规的 panic 使用相同的 reason 代码：
```cpp
MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR::CALL_VIOLATION_RESULTS_CODE::FATAL_REASONS_CODE::CALL_VIOLATION
```

## 3. 核心数据结构

### 3.1 全局状态

```cpp
class FreePagesAllocator {
    static fpa_state_t state;          // 当前状态 (SEED/ACTIVE)
    static BuddyControlBlock* BCBS;    // BCB 数组，按 base 单调递增排序
    static uint64_t BCB_count;         // BCBS 元素数量
    static fpa_stats* statistics_arr;  // Per-CPU 统计数组
    static uint64_t* processors_preffered_bcb_idx;  // Per-CPU 偏好 BCB 索引
    static all_pages_arr::free_segs_t* memory_crumbs;  // 碎片池
};
```

### 3.2 BuddyControlBlock 污染字段

```cpp
class BuddyControlBlock {
public:
    uint64_t dirty_count;  // 0=干净, >0=脏（禁止分配）
    // ...
};
```

- `dirty_count == 0`：BCB 干净，正常服务分配请求
- `dirty_count > 0`：BCB 脏，该 BCB 的部分物理页被外部借用
  - `can_alloc()` 返回 false → alloc 跳过此 BCB
  - `free()` 检测到 dirty → `Panic::panic(... CALL_VIOLATION)`

### 3.3 Per-CPU 统计

```cpp
struct fpa_stats {
    uint64_t alloc_count;
    uint64_t bcb_scan_total;
    uint64_t bcb_scan_max;    // 单次 alloc 中扫描的 BCB 数峰值
    uint64_t alloc_fail;
    uint64_t lock_try_fail;
    uint64_t free_count;
};
```

### 3.4 分配参数

```cpp
struct buddy_alloc_params {
    uint64_t numa;             // NUMA 节点（未支持，必须为 0）
    uint64_t try_lock_always_try:1;   // 无限重试标志
    uint64_t must_down_4gb:1;         // 强制在 4GB 以下分配
    uint8_t align_log2;               // 对齐 log2（默认 12=4KB）
};

// 预定义常量
constexpr buddy_alloc_params BUDDY_ALLOC_DEFAULT_FLAG{...};
constexpr buddy_alloc_params BUDDY_ALLOC_ALWAYS_TRY{...};
constexpr buddy_alloc_params BUDDY_ALLOC_DOWN_4GB{...};  // 4GB 限制
```

### 3.5 RAII 守卫 (util/lock.h)

```cpp
// 阻塞守卫：构造时 lock()，析构时 unlock()
class spintrylock_spin_guard {
    spintrylock_cpp_t& lock_ref;
public:
    explicit spintrylock_spin_guard(spintrylock_cpp_t& lock);
    ~spintrylock_spin_guard();
};

// 尝试守卫：构造时 try_lock()，析构时 unlock()（若持锁）
class spintrylock_try_guard {
    spintrylock_cpp_t* lock_ref;  // 非空=持锁
public:
    explicit spintrylock_try_guard(spintrylock_cpp_t* lock);
    bool is_locked() const;
    ~spintrylock_try_guard();
};
```

## 4. 初始化流程

### 4.1 mem_init 时序链

```
mem_init():
  PhyAddrAccessor::Init(identity_map_window)   // 物理地址访问器
  all_pages_arr::Init(&pages_arr)               // 页面状态数组
  pesisitent_properties_set()                   // 标记内核持久页
  FreePagesAllocator::Init(BEST_FIT, &FPA_bitmaps)  // FPA → SEED
  fpa_properties_deal()                         // pollute + activate
  KspacePageTable::Init()                       // 新 PML4
  KImage_map_rebuild()                          // 内核 ELF 段
  kimg_affiliate_property_map1()                // 附属区间映射
  gKernelSpace = new AddressSpace
  gKernelSpace->second_stage_init()             // PML4 分配 (DOWN_4GB)
  gKernelSpace->unsafe_load_pml4_to_cr3()       // CR3 切换
  properties_modify_stage1()                    // 瞬态区间转生
  kpoolmemmgr_t::multi_heap_enable()
```

### 4.2 FPA::Init()

```cpp
KURD_t Init(strategy_t strategy, vm_interval* VM_intervals_bcbs_bitmap);
```

1. 从 `all_pages_arr::free_segs_get()` 获取空闲段
2. 按 2 的幂次切分每个段，生成 BCB 计划条目 (`BCB_plan_entry`)
3. 分流：order ≥ 10 (4MB) → BCB 候选，order < 10 → `memory_crumbs` 碎片池
4. 策略处理：`BEST_ALIGN_FIT` 或 `MATCH_THREAD`
5. 按 base 排序构造计划
6. 从位图池分配每个 BCB 的 `mixed_bitmap_t` 内存
7. placement-new 构造 BCB 对象
8. 初始化 per-CPU 统计 + 偏好索引
9. `state = FPA_STATE_SEED`

### 4.3 fpa_properties_deal() — 污染 + 激活

```cpp
void fpa_properties_deal() {
    // 标记所有瞬态区间为脏
    FreePagesAllocator::interval_pollute(legacy_mmu_interval);
    FreePagesAllocator::interval_pollute(symtable_file 区间);
    FreePagesAllocator::interval_pollute(initramfs_file 区间);
    FreePagesAllocator::interval_pollute(log_buffer 区间);
    FreePagesAllocator::interval_pollute(kernel_entry_stack 区间);
    // 激活 FPA（不可逆）
    FreePagesAllocator::activate();
}
```

## 5. 污染-释放机制

### 5.1 interval_pollute

```cpp
static void interval_pollute(phymem_segment seg);
```

**查找逻辑**：
1. 二分 BCBS 找到 base ≤ seg.start 的最后一个 BCB
2. 从该 BCB 向前遍历，对每个与 seg 有交集的 BCB 加锁后 `++dirty_count`
3. 遇到 `BCB.base ≥ seg_end` 时停止

**调用约束**：仅在 SEED 态可调用；ACTIVE 态下调 → `Panic::panic(CALL_VIOLATION)`

**实现**：
```cpp
{ spintrylock_spin_guard _g(bcb.lock);
  ++bcb.dirty_count;
}
```

### 5.2 interval_clean

```cpp
static void interval_clean(phymem_segment seg);
```

**语义**：将 seg 区间涉及的 BCB 的 `dirty_count` -1。

**安全保护**：
- 若 `dirty_count` 已为 0 → `[PANIC] underflow` + `Panic::warn()` + 跳过
- 这是 pollute/clean 不配对的检测手段

**调用约束**：仅在 ACTIVE 态可调用；SEED 态下调 → `Panic::panic(CALL_VIOLATION)`

### 5.3 can_alloc() 的脏检查

```cpp
bool can_alloc(uint8_t order) {
    if (dirty_count != 0) return false;   // 脏 BCB 跳过
    for (uint8_t o = order; o <= max_supprt_order; ++o)
        if (statistics.free_count[o] > 0) return true;
    return false;
}
```

`dirty_count` 的读取不持锁（尽力而为），但实际时序安全：
- pollute 在 SEED 态完成，此时无 alloc
- activate 后不再 pollute
- `can_alloc` 返回 true 到正式加锁分配之间，不可能有外部 pollute

### 5.4 FPA::free() 的脏检测

```cpp
{ spintrylock_spin_guard _g(bcb.lock);
  if (bcb.dirty_count != 0) {
      // 极为严重的时序错误 → Panic::panic
      KURD_t violation_kurd = default_fatal();
      violation_kurd.event_code = EVENT_CODE_FREE;
      violation_kurd.reason = CALL_VIOLATION;
      Panic::panic(behaviors, "[FATAL][FPA::free] freeing into dirty BCB",
                   nullptr, nullptr, violation_kurd);
  }
  KURD_t bcb_kurd = bcb.free_buddy_way(base, size);
  // ...
}
```

在 ACTIVE 态下，alloc 不可能从脏 BCB 分配（被 `can_alloc` 过滤），若 free 仍遇到 `dirty_count > 0`，说明存在极端时序错误（如 BCB 从未被 pollute 但被误标记、或 free 了错误的地址）。

### 5.5 典型用法 (properties_modify_stage1)

```cpp
KURD_t properties_modify_stage1() {
    // --- initramfs ---
    phymem_segment initramfs = {initramfs_file.pbase, initramfs_file.size};
    initramfs_file.pbase = FreePagesAllocator::alloc(...);
    PhyAddrAccessor::paddr_memcpy(initramfs_file.pbase, initramfs.start, initramfs.size);
    uint64_t vbase = phyaddr_direct_map(&initramfs_file, &map_kurd);
    tlb_full_flush();
    FreePagesAllocator::interval_clean(initramfs);  // 归还旧区间

    // --- symboltable --- (同上)
    // --- log_interval --- (同上)
}
```

## 6. 分配/释放流程

### 6.1 alloc 流程

```cpp
phyaddr_t alloc(uint64_t size, buddy_alloc_params params,
                page_state_t interval_type, KURD_t& kurd);
```

1. **状态检查**：`state != ACTIVE` → `Panic::panic(CALL_VIOLATION)`
2. **参数校验**：size>0, numa==0, BCBS!=null
3. **Per-CPU 绑定**：获取 pid、统计指针、偏好 BCB 索引
4. **分配循环**：
   - 尝试偏好 BCB
   - 遍历所有 BCB（`attempt_one_bcb`）
   - 若全永久失败 → `NO_AVALIABLE_BCB`
   - 若遇到锁竞争 → 重试（最多 64 次，或由 try_lock_always_try 决定）

**attempt_one_bcb 内部**：
```
→ 4GB 检查 (must_down_4gb → 跳过超范围 BCB)
→ can_alloc(need_order) 检查
→ spintrylock_try_guard(&bcb.lock)
→ allocate_buddy_way(size, ...)   // 由 BCB 层实现
→ simp_pages_set(addr, ...)       // 标记页面状态
→ 更新 prefer_idx + 统计
→ 返回 addr
```

### 6.2 free 流程

```cpp
KURD_t free(phyaddr_t base, uint64_t size);
```

1. **状态检查**：`state != ACTIVE` → `Panic::panic(CALL_VIOLATION)`
2. **二分查找 BCB**（BCBS 按 base 排序）
3. **边界验证**：base 在 BCB 范围内
4. **加锁 + 脏检查**：`spintrylock_spin_guard` + `dirty_count != 0` → panic
5. **委托释放**：`bcb.free_buddy_way()`
6. **统计更新**：`per-CPU free_count++`

## 7. 4GB 限制 (must_down_4gb)

### 7.1 动机

AP 启动（`ap_start_pe` in `kernel_entry.asm`）在进入长模式前需要加载 CR3，CR3 指向的 PML4 物理地址必须在低 4GB 空间，否则 PAE 模式无法正确解码。

### 7.2 实现

```cpp
// buddy_alloc_params 新增字段
uint64_t must_down_4gb:1;

// alloc() attempt_one_bcb 中过滤
if (params.must_down_4gb) {
    const uint64_t bcb_span = 1ULL << (bcb.get_max_order() + 12);
    if (bcb.get_base() + bcb_span > 0x100000000ULL) {
        mark_permanent_fail(idx);
        return INVALID_ALLOC_BASE;
    }
}
```

### 7.3 在 AddressSpace 中的特殊处理

`gKernelSpace` 的 PML4 和所有页表页（PDPT/PD/PT）必须分配在 4GB 以下：

```cpp
// second_stage_init — PML4 分配
pml4_phybase = FreePagesAllocator::alloc(
    _4KB_SIZE,
    (this == gKernelSpace) ? BUDDY_ALLOC_DOWN_4GB : BUDDY_ALLOC_DEFAULT_FLAG,
    ...
);

// enable_VM_desc — 中间页表分配（同样检查）
entry_to_alloc_phybase = FreePagesAllocator::alloc(
    _4KB_SIZE,
    (this == gKernelSpace) ? BUDDY_ALLOC_DOWN_4GB : BUDDY_ALLOC_DEFAULT_FLAG,
    ...
);
```

### 11.2 释放错误

| 常量 | 含义 |
|------|------|
| `FAIL_REASON_CODE_BASE_NOT_BELONG` | 地址不属于任何 BCB |

### 11.3 状态违规 (CALL_VIOLATION)

| 常量 | 触发场景 |
|------|----------|
| `CALL_VIOLATION` | SEED 态调 alloc/free/clean；ACTIVE 态调 pollute；free 脏 BCB |

## 12. 配置常量

| 常量 | 值 | 含义 |
|------|-----|------|
| `min_bcb_order` | 10 | BCB 最小 order (4MB) |
| `try_fail_max` | 0x40 | 分配重试最大次数 |
| `INVALID_ALLOC_BASE` | ~0ULL | 分配失败返回地址 |
| `PAGELV4_KSPACE_BASE` | 0xFFFF800000000000 | 内核空间基址 |

## 13. 典型使用模式

### 13.1 瞬态区间转生 (move + clean)

```cpp
// 1. 标记旧区间为脏（SEED 态，fpa_properties_deal）
FreePagesAllocator::interval_pollute({old_pbase, size});

// 2. 激活 FPA
FreePagesAllocator::activate();

// 3. CR3 切换后的 ACTIVE 态
// 3a. 从 FPA 分配新物理页
uint64_t new_pbase = FreePagesAllocator::alloc(size, params, ...);
// 3b. 拷贝数据
PhyAddrAccessor::paddr_memcpy(new_pbase, old_pbase, size);
// 3c. 映射新页 + TLB 刷出
vaddr_t new_va = phyaddr_direct_map(&interval, ...);
tlb_full_flush();
// 3d. 归还旧区间
FreePagesAllocator::interval_clean({old_pbase, size});
```

### 13.2 直接丢弃 (clean only)

```cpp
FreePagesAllocator::interval_clean({discard_pbase, size});
// FPA 可对此区间所在的 BCB 重新分配（clean 后 dirty_count==0）
```

---

*版本: v3*  
*基于 commit 7cfa253*  
*最后更新: 2026-05-13*
