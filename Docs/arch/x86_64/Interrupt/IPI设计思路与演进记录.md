# IPI 设计思路与演进记录

日期: 2026-05-25  
最后修订: 2026-05-26 (v1 冻结)
涉及文件: memory_base.h / x86_vecs_deliver_mgr.cpp / 设计草案

---

## 一、起点：现行 IPI 机制的局限

**现行接口：**
```cpp
static void broadcast_exself_fixed_ipi(void(*ipi_handler)());
```

一个全局 `global_ipi_handler` 函数指针，一个向量号 ivec::IPI=240，
所有 IPI 调用者共用。调用即覆盖。

**问题：**
- 不可组合：两次不同 IPI 调用之间有竞争窗口
- 没有 per-CPU 差异：所有收端 CPU 跑同一个 handler
- 无法按地址空间精准点名：广播到所有 CPU，连不跑该 AS 的 CPU 也被打断
- 无法区分跑飞型 IPI（yield/exit/resched）和非跑飞型 IPI（TLB flush）

---

## 二、最终冻结方案：三层 IPI 架构（v1，2026-05-26）

### 2.1 设计总览

IPI 按执行语义分三层，各层用最匹配的机制：

```
Layer 1 — 跑飞型（soft_interrupt_functions，带 frame，不返回）
   向量固定，handler 拿 frame 后不返回主流程

Layer 2 — 非跑飞型（tokens 区域，永久槽，返回）
   高频固定事务，handler 执行后 EOI 返回

Layer 3 — 通用远程调用（tokens 区域，动态分配，返回）
   低频一次性调用，alloc_vec / free_vec 管理
```

### 2.2 向量分配表

| 层级 | 向量号 | 用途 | 递送路径 | 行为 |
|------|--------|------|----------|------|
| L1 | IPI_PANIC | 远程崩溃同步 | soft_interrupt_functions | 不返回 |
| L1 | IPI_RESCHED | 远程抢占/重调度 | soft_interrupt_functions | 不返回 |
| L1 | IPI_HALT | 远程停机（AP offline） | soft_interrupt_functions | 不返回 |
| L2 | IPI_TLB | 全局 TLB shootdown | tokens（全局锁串行） | 返回 |
| L2 | IPI_FROZED | 远程冻结（暂停） | tokens | 返回 |
| L2 | IPI_DEAD | 远程死亡通知 | tokens | 返回 |
| L3 | 32~255 中剩余 | 通用 smp_call | tokens（dispatch_lock） | 返回 |

`ivec::IPI=240`（旧全局 IPI 向量）退役。

### 2.3 全局 TLB shootdown 的锁方案

全局 TLB shootdown 是**严格串行**的事务。原因：
- 广播 IPI 本身的延迟（~1-2 us）远超锁争议开销
- 全局锁方案够简单，足以满足规模
- 高并发锁-free 方案带来的复杂度收益在全局路径上不成立

**三步串行模型**：

```
lock(global_ipi_lock)
    构造 TLB flush 包（pending_mask 全 1）
    sfence
    broadcast_all_exself_IPI(TLB_FLUSH_VEC)
    wait_all_acks(pending_mask == 0)  // 各收端清自己的 bit 作为 ACK
unlock(global_ipi_lock)
```

`pending_mask` 每位对应一个目标 CPU，收端清自己的位即完成 ACK。

### 2.4 per-AS TLB flush（暂缓）

用户空间页表 TLB shootdown 无需广播所有 CPU——
只需 IPI 给真正在运行该地址空间的 CPU。

但 per-AS TLB 与 swap / file_cache 机制深度耦合，
且需要 per-CPU AS tracking 基础设施。
**此路径暂缓，等用户 TLB 失效研究完成后再设计。**

### 2.5 设计决策推演记录

2026-05-26 讨论中考虑并否决的方案：

- **方案 B（per-CPU 多槽 bitmap）**：发端写目标 CPU 的 N 槽。
  优点：lock-free。缺点：O(N²) 空间，且全局 TLB 不值得 lock-free。
- **反转方案（发布者写自己的 GS）**：发端写自己的 tlb_req 槽，
  收端全量扫描。优点：零写写竞争。缺点：加了一个不需要的抽象层，
  全局路径的瓶颈在 IPI 本身不在锁上。

**最终结论**：全局 TLB shootdown 是全局事务，值得全局锁。

---

## 三、PCID 管理层设计

### 3.1 为什么需要 PCID

延迟 TLB shootdown 的成立条件：收端 CPU 的 TLB 里用 PCID tag 区分了不同地址空间。
如果不跑该 AS，它的 TLB 条目虽然残留但无命中。
只有在该 CPU 切回该 AS 时才需要刷新——前提是 PCID 在加载 CR3 时保留了旧条目。

SparrowOS 当前状态：
- `KERNEL_SPACE_PCID = 0`（内核用 PCID 0）
- `unsafe_load_pml4_to_cr3(uint16_t pcid)` 接口已支持
- `INVPCID type 0`（individual address + PCID）已在 flush_one_page 中使用
- 但每个 AddressSpace 没有独立 PCID 分配

### 3.2 7-slot per-CPU PCID 缓存

PCID 是 per-CPU 资源（TLB 物理隔离）。每个 CPU 独立管理自己的 7 个 PCID 槽（1~7，0 留给内核）。

**淘汰策略**：Clock 算法（二次机会）而非 LRU tick。
因为 load/store 不更新 PCID 使用信息，tick 粒度不够。
Clock 方案：每个 slot 一个 `used` bit，sched 切到时置 1，
淘汰时指针循环扫描，used=0→淘汰，used=1→清 0 继续。

**正确性**：如果该 AS 不在任何 CPU 上运行，淘汰它的 PCID 没有 TLB 损失。
如果该 AS 即将被调度到淘汰它的 CPU 上，损失一次 TLB refill。

**与延迟 TLB shootdown 的衔接**：
发端对目标 CPU 设 pending flush 标记。
收端在 `pcid_cache_acquire`（sched 路径）检查是否有 pending flush，
如果有 → INVPCID type 1 flush 整 PCID → 清标记。

---

## 四、page 元数据重构

### 4.1 从 1B 到 8B（page_v2）

原有 `struct page { page_state_t state; }`（1 byte）不满足 swap/LRU 需求。

新编码 `page_v2`（64-bit，packed）：

```
[3:0]   = state        (4b, 0-15)
[55:4]  = vaddr_compact (52b, 内核指针 [55:4]，16B 对齐约束)
[61:56] = order         (6b, 页框阶)
[63:62] = reserved      (2b)
```

`vaddr_compact` 存储 owner 指针（inode* / VM_DESC* / anon_vma*），
解码：`ptr = (void*)(vaddr_compact << 4)`。

### 4.2 page_cache_node_t（48B）

用于 `user_file` / `user_anonymous` / `kernel_anonymous` 页框（order ≥ 3）。

```
page_v2 meta       8B   — 编码: state + owner_ptr + order
next               8B   — LRU 链表后继
prev               8B   — LRU 链表前驱
flags              8B   — PAGE_DIRTY / PAGE_LOCKED 等 8 个标志位
offset_of_file     8B   — 文件块索引 / VMA 内偏移
refcount           4B   — 引用计数
map_count          4B   — PTE 映射数
                   ───
                   48B
```

**语义切换**：
- `state=user_file`：`vaddr_compact → inode*`，`offset_of_file → file_block_index`
- `state=user_anonymous`：`vaddr_compact → VM_DESC*`，`offset_of_file → vaddr_offset`
  物理地址从 mem_map 索引推：`phyaddr = mem_map_pbase + idx * 4096`

**为什么没有 swap 字段**：swap out 后元数据归还 free pool，page 结构不保留。
swap entry 编码在 PTE（non-present）或 page tree 中，这是 owner 的责任。

### 4.3 两种 page 状态的分流

| page_state | 所需结构 | owner 类型 |
|---|---|---|
| free / kernel_persist / kernel_pinned / dma | 仅有 page_v2（8B 编码） | 无（仅状态） |
| user_file / user_anonymous / kernel_anonymous | page_cache_node_t（48B） | inode* / VM_DESC* / 分配器描述符 |

---

## 五、IPI 锁方案设计推演

### 5.1 原始方案：通用函数指针 IPI

```
broadcast_exself_fixed_ipi(handler)
    → 竞争风险：写 func_ptr 期间收端可能读到中间态
    → 不可等待确认：收端跑完 handler 后 EPI 返回，发端无法确认
```

### 5.2 方案 A：全局大锁

```
lock(global_ipi_lock)
    set_handlers(all_targets)
    sfence
    write_ICR(broadcast)
    wait_all_acks()
unlock()
```

优点：简单、正确。缺点：广播时所有 IPI 串行化。

### 5.3 方案 B：per-CPU 锁 + 按 APIC ID 顺序取

```
lock(targets in apic_id ascending order)
    set_handlers...
    send
    wait_acks
unlock(reverse order)
```

死锁分析：所有发送者按升序取锁，降序释放 → 无循环等待。

问题：等 ACK 期间持锁，阻塞其他想发 IPI 的 CPU。

### 5.4 Linux 式 lock-free pending flag（方案 C，已否决）

发端不写函数指针，只写 **per-CPU pending request 槽**（GS slot）。
收端上线读固定枚举的 cmd 字段 dispatch。

```
// per-cpu 请求槽（GS slot 或独立数组）
enum ipi_cmd:  IPI_NONE, IPI_TLB_RANGE, IPI_TLB_ALL, IPI_PANIC, ...

// 发端
target->ipi_req.cmd = IPI_TLB_RANGE
target->ipi_req.params = ...
sfence
send_IPI(target)

// 收端 ipi_cpp_enter
cmd = self->ipi_req.cmd
self->ipi_req.cmd = IPI_NONE
switch (cmd): ...
```

**不需要函数指针**。IPI 不是一个远程过程调用机制，它是一组定义好的有限事务集合。
远程执行任意函数是个错误抽象——它引入了所有竞争风险，却没有得到足够的表达力回报。

此方案的直觉方向正确（固定枚举 vs 函数指针），但经过 2026-05-26 讨论，
全局 TLB shootdown 的瓶颈在 IPI 延迟本身，不值得为它实现 lock-free 路径——
全局锁串行已足够。保留此思路供未来 per-AS TLB shootdown 参考。

### 5.5 2026-05-26 冻结结论：全局事务用全局锁

经过三轮推演后，2026-05-26 冻结的结论：

| 路径 | 锁策略 | 理由 |
|------|--------|------|
| 全局 TLB shootdown | **全局锁**（方案 A） | 广播 IPI 延迟 ~1-2 us >> 锁争议开销；全局事务值得串行 |
| per-AS TLB shootdown | 暂缓 | 依赖 swap/file_cache 研究 + PCID 缓存 |
| 跑飞型 IPI | 无锁（固定向量 + soft_int） | 不返回路径不需要同步 |
| 动态远程调用 | `dispatch_lock` | 低频，现有接口 |

### 5.6 x2APIC Logical Mode 支持多播

SDM §13.12.10 确认：x2APIC Logical 模式下同一簇内的 CPU 可以通过
OR 它们的 Logical ID 实现多播。每簇最多 16 个 CPU。
跨簇需要多个 ICR 写入。

保留为 per-AS TLB shootdown 的可选优化方案。当前全局 TLB 走 broadcast，不需要多播。

---

## 六、未完成的待办（2026-05-26 状态）

### 6.1 代码改动（冻结，等待下一阶段）

代码尚未改动。当前状态：`soft_interrupt_functions[ivec::IPI]` 和 `global_ipi_handler` 仍为过渡接口。

以下改动在 v1 设计冻结后排队：

1. **`fixed_interrupt_vectors.h`**：新增 `ivec::IPI_RESCHED`、`ivec::IPI_HALT`、`ivec::IPI_TLB`、`ivec::IPI_FROZED`、`ivec::IPI_DEAD` 枚举值
2. **`x86_vecs_deliver_mgr.cpp::Init()`**：注册新 soft_interrupt 和 token 条目
3. **`alloc_vec` 跳过固定槽**：`alloc_vec` 线性扫描时跳过 Layer 1 & 2 固定向量
4. **全局 TLB shootdown 实现**：`global_ipi_lock` + 广播 + wait_acks 模式
5. **`global_ipi_handler` 废弃**：残余场景迁移
6. **`broadcast_exself_icr`**：从 Physical 改为 Logical 模式（推迟到 per-AS TLB）

### 6.2 下一阶段：用户 TLB 失效研究

当前正在进行用户空间页表 TLB 失效的研究，与 swap / file_cache 紧密耦合。
per-AS TLB shootdown 设计依赖该研究的输出，因此 IPI 框架的具体代码改动
暂停直到用户 TLB 失效方案确定。

### 6.3 长期待办（独立于 IPI）

7. **page_v2 + page_cache_node_t**：代码已写入 `memory_base.h`，但 `page_allocator` / `all_pages_arr` 尚未迁移到新类型
8. **PCID 缓存管理层**：7-slot per-CPU 设计已完成，但未写代码。等待 per-AS TLB shootdown 时一并实现
