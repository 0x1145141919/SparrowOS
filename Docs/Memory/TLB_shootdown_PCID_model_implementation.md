# TLB Shootdown + PCID 模型实现 (v4)

日期：2026-06-07
状态：落地

---

## 一、架构概览

### 两层 TLB 失效

| 层级 | 管理对象 | 失效粒度 | 路径 |
|------|---------|---------|------|
| **内核空间** (PCID 0) | `KspacePageTable` | 全核广播 | `Kspace_phyaddr_direct_unmap` → `broadcast_invalidate_tlb` |
| **用户空间** (PCID 1~5) | `AddressSpace` | 按位图定点 | `utlb_invalidate_ipis` → 逐核 `utlb_invalidate` |

### PCID 分配

| PCID | 归属 | 管理 |
|------|------|------|
| 0 | `gKernelSpace` | 固定，不可换出 |
| 1 ~ 5 | 用户 AddressSpace | per-core LRU 动态分配 |

---

## 二、PCID 三态模型

### 状态定义

```
下线 (offline):    无 TLB，无 PCID 槽
缓存 (cached):     有 TLB，PCID 槽保留（CR3 未加载）
在线 (online):     有 TLB，PCID 槽保留，CR3 已加载
```

### 合法转换

```
offline → online:  CR3 加载、分配 PCID 槽、tlb_bitmap 置位
cached  → online:  CR3 加载（TLB 命中）、tlb_bitmap 置位（已置）
online  → cached:  CR3 切换走，TLB 保留，tlb_bitmap 不动
cached  → offline: PCID 被回收、INVPCID type=1 清 TLB、tlb_bitmap 清零
```

### TLB 存在性位图

每个 `AddressSpace` 持有 `tlb_holding_bitmap[64]` (4096 bits)：

```cpp
// AddresSpace.h — 私有成员
uint64_t tlb_holding_bitmap[(MAX_PROCESSORS_COUNT + 63) / 64];
// = uint64_t[64] for MAX_PROCESSORS_COUNT = 4096
// bits[x] = 1 代表 core x 上有此 AS 的 TLB (cached 或 online)
```

位图通过 CAS 无锁操作：

```cpp
// AddresSpace.cpp
bool AddressSpace::tlb_on_set() {
    interrupt_guard irq_guard;              // 防本核中断重入
    uint32_t id = fast_get_processor_id();
    uint32_t w = id / 64;
    uint64_t b = 1ULL << (id % 64);
    uint64_t expected, desired;
    do {
        expected = __atomic_load_n(&tlb_holding_bitmap[w], __ATOMIC_RELAXED);
        if (expected & b) return false;     // 已设置
        desired = expected | b;
    } while (!__atomic_compare_exchange_n(
        &tlb_holding_bitmap[w], &expected, desired,
        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return true;
}

bool AddressSpace::tlb_on_clear() {
    // 同上，desired = expected & ~b
}
```

---

## 三、per-core PCID 槽管理

### 数据结构

```cpp
// GS_complex.h
struct pcid_entry_t {
    void*    addrSpace;                           // AddressSpace*
    uint64_t last_accees_microsecond_timestamp;   // LRU 时间戳
};

struct pcid_complex_t {
    pcid_entry_t entries[6];   // 索引 0→PCID 0 (kernel), 1..5→用户
    uint8_t      now_using_pcid_idx;   // 当前 CR3 加载的 PCID 索引
};
```

### shift_addresSpace — AS 切换状态机

```cpp
// AddresSpace.cpp
void shift_addresSpace(AddressSpace *new_address_space) {
    // 切到 gKernelSpace → PCID 0 直切
    if (new_address_space == gKernelSpace) {
        new_address_space->unsafe_load_pml4_to_cr3(0);
        return;
    }

    // 扫描 slots[1..5]：查缓存 + 找空闲 + 找 LRU 受害者
    for (int i = 1; i <= 5; i++) {
        if (entries[i].addrSpace == new_address_space) → cached_idx
        else if (entries[i].addrSpace == nullptr)       → free_idx
        else                                             → victim_idx (LRU)
    }

    if (cached_idx >= 0) {
        // cached → online: 只切 CR3，不清 TLB
        entries[cached_idx].last_accees_microsecond_timestamp = now;
        pcc->now_using_pcid_idx = cached_idx;
        new_address_space->tlb_on_set();
        new_address_space->unsafe_load_pml4_to_cr3(cached_idx);
        return;
    }

    // offline → online: 可能需要 LRU 逐出
    slot = free_idx ?? victim_idx;
    if (逐出) {
        INVPCID type=1 (清除此 PCID 全 TLB)
        victim_as->tlb_on_clear();
    }
    entries[slot] = { new_address_space, now };
    now_using_pcid_idx = slot;
    tlb_on_set();
    unsafe_load_pml4_to_cr3(slot);
}
```

---

## 四、TLB Shootdown 投递

### uspace_tlb_shutdown_infopak

```cpp
// AddresSpace.h
struct uspace_tlb_shutdown_infopak {
    AddressSpace*             target_space;
    seg_to_pages_info_pakage_t tlb_pak;   // 待失效的地址范围分段
};
```

### utlb_invalidate — IPI handler（远端核执行）

```cpp
extern "C" uint64_t utlb_invalidate(uspace_tlb_shutdown_infopak* pak) {
    // 扫描本地 pcid_complex.entries[1..5]，找此 AS 的 PCID 槽
    for (int i = 1; i <= 5; i++) {
        if (entries[i].addrSpace != pak->target_space) continue;

        // 按 tlb_pak 逐条目逐页 INVPCID type=0
        for (int e = 0; e < 5; e++) {
            auto& entry = pak->tlb_pak.entryies[e];
            for (uint64_t j = 0; j < entry.num_of_pages; j++) {
                vaddr_t vaddr = entry.vbase + j * entry.page_size_in_byte;
                INVPCID type=0 (PCID=i, vaddr=vaddr)
            }
        }
        return (i == now_using_pcid_idx) ? 1 : 2;
    }
    return 3;   // 脱靶
}
```

返回值：1=上线, 2=缓存中, 3=脱靶

### utlb_invalidate_ipis — 按位图投递（发起端执行）

```
快照 = tlb_holding_bitmap（无锁拷贝，snapshot 兼任完成位图）
本核有 TLB → utlb_invalidate() 直调，清 snapshot 自身位

while snapshot 不全零:
    if 50ms deadline → panic
    for pid with bit set in snapshot:
        ret_ipi_send(utlb_invalidate, pak, pid)
        lo=1/3/4 → snapshot 清位（完成）
        lo=2(BUSY) → 下轮重试
    if 整轮无进展 → pause 8×
```

设计要点：
- **snapshot 复用为完成位图**：成功一个清一个位，全零即完毕
- **50ms 硬上限 → panic**：正确设计的系统不应超时
- **BUSY 重试**：目标核 slot 被占，等下一轮
- **超时/不存在**：直接标记完成（无法到达的核不阻塞）

---

## 五、内核空间 TLB broadcast

`KspacePageTable` 的 `disable_VMentry` 后跟 `broadcast_invalidate_tlb`：

```
broadcast_invalidate_tlb(pak):
  本核直调 remote_invalidate_seg(pak)
  512B done_bitmap，self 标记完成

  while confirmed < nproc:
    if 50ms deadline → panic
    for pid in 0..nproc:
      if done → continue
      ret_ipi_send(remote_invalidate_seg, pak)
      lo=1 → bit 标记完成
      lo=2/3 → 下轮重试
    if 整轮无进展 → pause 8×
```

锁范围改进：

```
旧: GMlock ──→ PTE clear ──→ TLB broadcast ──→ poll ──→ GMlock 释放
                                    ↑ 640ms 上限
新: GMlock ──→ PTE clear ──→ GMlock 释放
                         └──→ broadcast_invalidate_tlb (50ms 上限, 无锁)
```

---

## 六、IPI v3 Slot 释放修复

### 问题

```cpp
// 旧：非原子 16B 写
complex->local_ipi_complex = 0;    // 编译器可拆为两个 8B store
```

其他核的 `cmpxchg16b` 可能看到 torn value → 误判 BUSY。

### 修复

```cpp
// 全部改为 cmpxchg16b 原子释放
__uint128_t zero = 0;
__uint128_t current = complex->local_ipi_complex;
cmpxchg16b(&complex->local_ipi_complex, &current, &zero);
```

---

## 七、中断向量锁 per-core 化

### 旧：全局锁

```cpp
static spinlock_cpp_t dispatch_lock;     // 所有核争一把锁
```

### 新：per-core 锁

```cpp
// GS_complex.h
struct gs_complex_t {
    interrupt_token_t tokens[256];
    spinlock_cpp_t    tokens_lock;        // 保护 tokens[] 读写
};
```

锁模式：

```
分配/释放: spinlock_interrupt_about_guard l(cx->tokens_lock)
             扫描 + 写入/清零              ← 锁内
          离开 scope 自动释放

分发路径: spinlock_interrupt_about_guard l(self->tokens_lock)
             local_tok = self->tokens[vec]   ← 锁内拷贝
          离开 scope 自动释放
           local_tok.func(&local_tok)        ← 锁外调用
```

---

## 八、广播关机

```cpp
// kinit.cpp
static uint64_t ipi_shutdown_func(void*) {
    cli; wbinvd; hlt;        // 三指令收工
    __builtin_unreachable();
}

extern "C" void broadcast_shutdown() {
    for pid = 0..nproc-1 (skip self):
        if 50ms global deadline exceeded → break
        fly_ipi_send(ipi_shutdown_func, pid)   // best-effort
    self: cli; wbinvd; hlt
}
```

---

## 九、PhyAddrAccessor 缓存改造

### LFU → LRU

```cpp
// 旧
uint8_t lfu_freq[16];    // 频次累加，马太效应
// 新
uint64_t lru_tick[16];   // 单调时间戳，淘汰最久未访问
uint64_t access_clock;   // 全局时钟
```

### Cache miss 实现

1. `phybase = addr & ~(1GB-1)` — 1GB 对齐
2. `slot = cache_evict_lru()` — 选最久未访问槽
3. 如果有旧映射 → `Kspace_phyaddr_direct_unmap(cache_tb[slot])`
4. `vbase = Kspace_pinterval_alloc_and_map({vpn=0, ppn=phybase>>12, npages=1GB>>12, access=UC})`
5. `cache_tb[slot] = updated` + `cache_touch(slot)`

---

## 十、文件改动清单

```
新文件:
  Docs/Memory/TLB_shootdown_PCID_model_implementation.md   (本文)

修改的文件:
  src/include/memory/AddresSpace.h
      - tlb_on_set / tlb_on_clear
      - uspace_tlb_shutdown_infopak struct
      - utlb_invalidate / utlb_invalidate_ipis extern "C" 声明
      - shift_addresSpace extern "C" 声明
      - 清除旧的 static utlb_invalidate member 声明

  src/include/arch/x86_64/abi/GS_complex.h
      - spinlock_cpp_t tokens_lock
      - get_gs_base() — RDGSBASE 内联函数

  src/include/memory/phyaddr_accessor.h
      - LFU → LRU（lfu_freq → lru_tick + access_clock）
      - 移除 cache_vaddr[]

  src/memory/arch/x86_64/AddresSpace.cpp
      - tlb_on_set / tlb_on_clear 实现
      - utlb_invalidate / utlb_invalidate_ipis 实现
      - shift_addresSpace 实现

  src/memory/PhyAddrAccessor.cpp
      - LRU evict 实现
      - Cache miss: Kspace_pinterval_alloc_and_map / Kspace_phyaddr_direct_unmap

  src/memory/out_surfaces.cpp
      - broadcast_invalidate_tlb (TLB shootdown 从 KspacePageTable 剥离)

  src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp
      - dispatch_lock (全局) → per-core tokens_lock
      - idt_vec_demux_entry / fred_vec_demux_hw_dispatch 锁保护 token 读取
      - IPI v3 slot 释放改为 cmpxchg16b 原子释放

  src/arch/x86_64/boot/kinit.cpp
      - create_first_kthread: broadcast_exself_fixed_ipi → 逐核 fly_ipi_send
      - ipi_shutdown_func + broadcast_shutdown

  src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm
      - cacheline_wait 纯汇编实现 (UMONITOR + UMWAIT)
```

---

## 十一、遗留决策

| 问题 | 当前选择 | 远期方向 |
|------|---------|---------|
| TLB 失效路线 | 地址级精准同步（ret_ipi_send） | — |
| 超时放弃 | 50ms deadline → panic | 多核工业级可改入队模型 |
| PCID | PCID 0 固定内核，1~5 LRU | — |
| AMD 兼容 | 暂缓 | 无硬件 |
| INVPCID type 选择 | type=0 逐页 / type=1 全清 | 当前正确（CR4.PCIDE 待启用） |
| CR4.PCIDE | 当前=0 | 需 CPUID 检测后置位 |
| broadcast_shutdown 超时 | 50ms 后跳过剩余，自救 | — |
