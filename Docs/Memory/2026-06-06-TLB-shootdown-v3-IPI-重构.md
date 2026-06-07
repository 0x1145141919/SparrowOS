# 工作日志：TLB shootdown 重构 + IPI v3 整合 (2026-06-06)

## 背景

将 TLB shootdown 从 `KspacePageTable` 中剥离，改用 IPI v3 `ret_ipi_send` 逐核发送，
并修复 IPI v3 slot 释放的非原子写 bug。

---

## 一、IPI v3 slot 释放修复

**问题**：`ret_ipi_send` / `fly_ipi_send` 中 4 处 `complex->local_ipi_complex = 0` 是
非原子 16B 写入（编译器可拆为两个 8B store），另一个核的 `cmpxchg16b` 可能看到 torn value，
误判 slot 状态并返回 BUSY。

**修复**：全部改为 `cmpxchg16b(slot, &current, &zero)` 原子释放。超时路径用 retry loop
兜住 target 在 deadline 后瞬间消费的竞态。

改动文件：`src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp`

---

## 二、86B 既定事实确认

CPUID.05H 查得本机 SMALLEST_MONITOR_LINE_SIZE = LARGEST_MONITOR_LINE_SIZE = **64 字节**。
所有现代 x86-64 CPU（Core 2 至今）都返回 64，实际不会变。

- `local_ipi_complex` 已 `alignas(64)` + `padding[6]` 填满整条缓存线 ✅
- 同步核查了 Intel SDM 和 AMD 手册（40332 r4.00），AMD 格式一致但无 WAITPKG

---

## 三、`cacheline_wait` 纯汇编实现

将 `ipi_wait_lo` 中调用的 `cacheline_wait` 从 C++ inline asm 迁移到
`Sysdef_exception_entries.asm` 纯汇编。

```
cacheline_wait(addr):
  umonitor rdi
  xor eax, eax; xor edx, edx
  not rax; not rdx       ← EDX:EAX = ~0, 忽略指令级 deadline
  xor ecx, ecx           ← C0.2
  umwait ecx
  setc al                ← CF→AL (0=store 唤醒, 1=超时/其他)
  movzx eax, al
  ret
```

---

## 四、TLB shootdown 从 KspacePageTable 剥离

### 目的

`KspacePageTable::disable_VMentry` 原本包含：
1. PTE clear（需要 `GMlock`）
2. TLB broadcast + poll（不需要锁，但阻塞锁释放）

在 64 核上，每个核 10ms 的 ret_ipi_send 超时 → 640ms 的锁持有上限 → 全局锁放大灾难。

### 改动

| 旧 | 新 |
|----|----|
| `KURD_t disable_VMentry(interval)` | `seg_to_pages_info_pakage_t disable_VMentry(interval, KURD_t&)` |
| 内含 broadcast_exself_fixed_ipi + poll | **纯 PTE clear**，TLB 相关全部移出 |
| 依赖 `shared_inval_kspace_VMentry_info` 全局状态 | 无全局状态，pak 通过栈返回 |

`KspacePageTable` 现在只管理页表数据结构。

### `KspacePageTable::invalidate_seg` → `remote_invalidate_seg`

旧的 `invalidate_seg` 依赖全局 `shared_inval_VMentry_info_t`，新函数：
- 签名 `uint64_t remote_invalidate_seg(void* ptr)`，符合 `ret_ipi_send` 函数类型要求
- 接收 `seg_to_pages_info_pakage_t*`，逐条目 `invlpg`
- 无全局依赖，运行在 IPI handler 上下文中（IF=0）
- 返回 0=成功，非 0=错误

---

## 五、`broadcast_invalidate_tlb` 实现

新的全核 TLB shootdown 函数，部署在 `out_surfaces.cpp`。

### 算法

```
1. 本核直接调 remote_invalidate_seg(pak)
2. 远端核: while confirmed < nproc:
     a. 检查 50ms 全局 deadline → 超则 panic
     b. for pid in 0..nproc:
          - 512B bitmap 跳过已完成核
          - ret_ipi_send(remote_invalidate_seg, pak)
          - lo64==1 → bit 标记完成, confirmed++
          - lo64==2/3 → 跳过，下轮重试
     c. 整轮无进展 → pause 8 次避让
```

### 锁范围

```
旧: GMlock ──→ PTE clear ──→ TLB broadcast ──→ poll ──→ GMlock 释放
                                    ↑ 640ms 上限
新: GMlock ──→ PTE clear ──→ GMlock 释放
                         └──→ broadcast_invalidate_tlb (50ms 上限, 无锁)
```

`__wrapped_pgs_vfree` 和 `Kspace_phyaddr_direct_unmap` 均已适配。

---

## 六、INVPCID 研究

确认了 PCID 场景下的 TLB 失效方案：

| 指令 | 精度 | 适用 |
|------|------|------|
| INVLPG | 仅当前 PCID | 内核页表（PCID=0），暂未开 PCID |
| INVPCID type=0 | 指定 PCID + 虚拟地址 | 开 PCID 后替换 INVLPG |
| INVPCID type=1 | 整 PCID 全清 | AS 销毁 |
| INVPCID type=2/3 | 全 PCID | 大清洗 |

当前 `remote_invalidate_seg` 用 `INVLPG` 是正确的（CR4.PCIDE=0）。
开 PCID 后改 `INVPCID type=0` 即可。

---

## 七、遗留决策

| 问题 | 选择 | 理由 |
|------|------|------|
| TLB 失效路线 | **地址级精准同步**（ret_ipi_send） | 不丢失效，语义简单 |
| 超时放弃还是入队 | **50ms 全局 deadline → panic** | 正确设计的系统不应超时，入队复杂度暂不值得 |
| 惰性队列 per-core | 暂不实现 | 多核工业级 OS 时才需要（64+ 核，每秒万次 shootdown） |
| AMD 兼容 | 暂缓 | 没有硬件，写出来是盲猜 |
| PCID | 暂缓 | 用户 AS 切换优化，不影响当前内核页表路径 |

---

## 八、改动文件清单

```
新文件:
  src/memory/out_surfaces.cpp (new, 含 broadcast_invalidate_tlb)

修改的文件:
  src/memory/arch/x86_64/KspacMapMgr.cpp
  src/memory/arch/x86_64/KspacMapMgr_pagediretct_operate.cpp
  src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp
  src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm
  src/memory/out_surfaces.cpp
```

---

## 九、审核隐患记录 (2026-06-07)

**审核范围：** `out_surfaces.cpp` + `x86_vecs_deliver_mgr.cpp` + 本文档一致性

### P1 — 必须修

#### P1.1 `__wrapped_pgs_valloc` / `stack_alloc` 失败路径泄漏

**文件：** `out_surfaces.cpp`

- `__wrapped_pgs_valloc`：`FreePagesAllocator::alloc` 成功但后续 `vm_table->insert` 失败 → 不回滚已分配的物理页；`alloc_available_space` 成功但 `insert` 失败 → 不释放 vaddr
- `stack_alloc`：同上，`alloc_available_space` 成功但 `insert` 或 `enable_VMentry` 失败 → vaddr 不回滚

**建议修复方向：** 每步失败显式回滚之前已获取的资源，或用 RAII wrapper 管理临时资源。

#### P1.2 `broadcast_invalidate_tlb(nullptr)` 静默返回 success

**位置：** `out_surfaces.cpp` → `broadcast_invalidate_tlb` 入口

```cpp
if (!pak) return success;
```

传入 nullptr 时跳过所有核的 TLB 失效。如果调用方存在 bug，陈旧的 TLB 条目不会被清除，后续内存访问命中 stale TLB → 难以调试的数据损坏。

**建议修复方向：** `panic` 而非静默返回 success；或确保调用方保证 `pak` 永远有效（如传引用而非指针）。

#### P1.3 `idt_vec_demux_entry` handler 中 cmpxchg16b 回写失败无 fallback

**位置：** `x86_vecs_deliver_mgr.cpp` → `IPI_RETURNABLE` 处理路径

handler 用 `cmpxchg16b(local_ipi_complex, &fnbox_copy, &result_box)` 写回结果。
如果 slot 在 handler 读取快照后到 cmpxchg16b 之前被发送方的超时回退修改，
cmpxchg16b 会失败，结果不写入 slot。发送方侧看到 slot 未变化，可能触发回退路径
并认为超时，但实际上目标核已执行完毕。

**建议修复方向：** cmpxchg16b 失败时 retry（重读当前值再 CAS），或检查失败原因后
选择丢弃结果（反正发送方已超时回退）。

#### P1.4 `Kspace_phyaddr_direct_unmap` 中 `status` 声明未初始化

**位置：** `out_surfaces.cpp` → `Kspace_phyaddr_direct_unmap`

```cpp
KURD_t status;  // ← 未初始化
{
    spinlock_interrupt_about_guard guard(kspace_pagetable_modify_lock);
    // ...
    pak = KspacePageTable::disable_VMentry(interval, status);
}
status = broadcast_invalidate_tlb(&pak);
if (error_kurd(status)) return status;
```

目前 `disable_VMentry` 失败时函数提前返回 `fail`，`status` 不会被读到——
但后续修改者可能无意中在 PTE clear 失败后继续执行，读到未初始化值。
靠代码布局保证安全脆弱易碎。

**建议修复方向：** `KURD_t status = KURD_t(...);` 显式初始化。

### P2 — 建议修

#### P2.1 `broadcast_invalidate_tlb` deadline 检查频率

每轮 while + 每核 for 都调用 `ktime::get_microsecond_stamp()`。64 核满配时
每次 while 迭代调用 64+1 次高精度时间戳读取（IO 操作）。

**建议：** 每轮 while 迭代只在 for 循环外部检查一次 deadline，减少 IO 操作频次。

#### P2.2 `fly_ipi_send` 语义歧义

`fly_ipi_send` 发送 IPI_RUNAWAY 后仍 busy-wait 等待 lo64==1 确认消费，
有 10ms 超时。这不是真正的 fire-and-forget。

**建议：** 函数名或注释明确文档化语义为
"确认消费的 fire-and-forget"，调用者需接受 ≤10ms 的阻塞可能。

#### P2.3 `vec_demux::alloc_vec` 的 per-core 锁争用

**位置：** `x86_vecs_deliver_mgr.cpp` → `alloc_vec`

当前 `dispatch_lock` 全局锁保护所有核的 token slice。64 核频繁 alloc/free 时，
可能产生锁争用。

**建议：** 如成为性能瓶颈，改为 per-core 的 alloc 锁。

#### P2.4 FRED 路径 CR2 推栈确认

`fred_enable` 中 CR4.FRED = 1 启用后，FRED 硬件自动将 CR2 压入栈上 event data。
当前 FRED handler（`fred_vec_demux_hw_dispatch` / `fred_vec_demux_soft_dispatch`）
需要确认从栈上提取 CR2 的逻辑与 `page_fault_bare_enter` 路径一致。

**建议：** 保持 `page_fault_cpp_handler` 入口处 CR2 来源的统一抽象层，避免 IDT 与 FRED
两路径的 page fault handler 走不同的 CR2 获取方式。

### P3 — 观察

#### P3.1 50ms deadline 在 64 核极端场景下的充裕性

当前算法：每轮 while 迭代扫描 bitmap 中所有未完成核，逐个 `ret_ipi_send`。
每个 `ret_ipi_send` 有 10ms 超时。如果大量核同时处于 IF=0 长路径（如 panic 或其他临界区），
`ret_ipi_send` 反复 BUSY 回退，每次 pause 8 次（~40 ns），50ms 内可能不够完成全部广播。

**建议：** 实测后根据核数动态调整 deadline（如 `nproc * 1ms + 10ms` 保底），
或在 panic 路径中主动标记自身为"不可达"让 broadcast 跳过。
