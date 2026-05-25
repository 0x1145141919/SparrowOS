# IPI 设计思路与演进记录

日期: 2026-05-25  
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

## 二、第一轮思考：IPI 框架改造

### 2.1 per-CPU 局部性

`gs_complex_t::slots[256]` 有大量空闲槽位（目前只用了 5 个）。
可以在 GS slot 挖一个 per-CPU IPI 请求槽，替代全局变量。

### 2.2 两路向量分流

跑飞型和非跑飞型 IPI 需要不同向量号：

| 类型 | 向量 | 递送路径 | 行为 |
|------|------|----------|------|
| 跑飞型 | ivec::IPI_RUNAWAY | soft_interrupt_functions（带 frame） | resched / yield，不返回 |
| 非跑飞型 | ivec::IPI_NORUNAWAY | tokens 表（无 frame） | TLB flush / halt，返回后 EOI |

`all_vec_delivery` 的两路分流已经支持这个模式，只需分配新向量并注册。

### 2.3 per-AS TLB flush

用户空间页表变更只需 IPI 给**真正在运行该地址空间**的 CPU。
需要：每个 CPU 记录当前 address_space ID（GS slot 或 task->mm），
发端遍历查表 → 对匹配的 CPU 发 unicast / multicast IPI。

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

### 5.4 最终方向：Linux 式 lock-free pending flag（方案 C）

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

### 5.5 x2APIC Logical Mode 支持多播

SDM §13.12.10 确认：x2APIC Logical 模式下同一簇内的 CPU 可以通过
OR 它们的 Logical ID 实现多播。每簇最多 16 个 CPU。
跨簇需要多个 ICR 写入。

---

## 六、未完成的待办

1. **page_v2 + page_cache_node_t**：代码已写入 `memory_base.h`，但 `page_allocator` / `all_pages_arr` 尚未迁移到新类型
2. **PCID 缓存管理层**：7-slot per-CPU 设计已完成，但未写代码
3. **per-CPU IPI 请求槽**：GS slot 分配 + `ipi_cmd_t` 枚举 + `ipi_cpp_enter` dispatch 重写
4. **延时 TLB shootdown**：依赖 2 和 3，需 PCID 缓存 + per-CPU pending flush 标记
5. **`global_ipi_handler`**：逐步废弃，残余场景（AP bringup/panic）过渡到 `IPI_FUNC_CALL` 枚举
6. **`broadcast_exself_icr`**：从 Physical 模式改为 Logical 模式支持多播
