# TLB shootdown v4 —— IPI RPC 规范（线程态 / 可中断）与静态无死锁推演

> 前身：`2026-06-06-TLB-shootdown-v3-IPI-重构.md`（把 shootdown 从页表管理剥离、引入 per-core slot 的
> returnable/fly RPC）。本文件修订其**等待语义**，确立 v4 编排。
> 定位：全局（内核）TLB 失效**本就允许并行**——v3 隐含"shootdown 全局排他"才需要串行化。

---

## 0. 一句话

把 `returnable_ipi_send` / `fly_ipi_send` 当**两类核间 RPC**看待：
- **slot 抢占 = try-lock**（`cmpxchg16b`，失败即 BUSY，绝不内部自旋）；
- **等待对端回写期间必须 IF=1**（线程态可中断），等待者是"进程"，不是"死循环"。
在这两条之上，`broadcast_invalidate_tlb` 的并行是安全的，且可**静态推演无死锁**。

---

## 1. v3 的根因（关中断的 hold-and-wait）

`Kspace_phyaddr_direct_unmap` / `__wrapped_pgs_vfree` 顶层 `interrupt_guard g;` → 整函数 IF=0；
其内的锁 guard 构造时读到 IF=0，出锁后**又 CLI 回 0**（不恢复中断）⇒ `broadcast_invalidate_tlb`
全程 IF=0。而 `returnable_ipi_send` 内部再套 `interrupt_guard irq;` 把「发 IPI + 轮询回写」整段包住
⇒ **等对端 ACK 期间本核 IF=0**。

`IPI_RETURNABLE(253)` 是普通 fixed 投递中断，**本核 IF=0 时只在 LAPIC IRR 里 pending、不执行 handler**。
于是两个核同时 broadcast ⇒ A 等 B、B 等 A，双方都进不去自己的 handler ⇒ 内层 100ms 超时 →
`confirmed` 不增 → 外层 50ms deadline 到 → `Panic(TLB_SHOOTDOWN_TIMEOUT)`。

> 荒谬点：lock-free 的核间 RPC 被当成了"要关中断保护的临界区"。

---

## 2. 规范（v4）

| # | 条目 | 约束 |
|---|------|------|
| **R1** | 上下文 | `returnable_ipi_send` / `fly_ipi_send` **仅内核线程态**可调；**禁止**中断上下文 / #PF / panic 路径。 |
| **R2** | 中断态 | 接口**内部不得关中断**，等待段全程 **IF=1**（`enable_interrupt_guard` 落地）。 |
| **R3** | 无临界区 | 调用时**不得持有**任何 IF=0 临界区（自旋锁 / `*_interrupt_about_guard`）。 |
| **R4** | slot = try-lock | `cmpxchg16b(slot,0→req)`，失败即 **BUSY(2)** 立即返回；释放一律 `cmpxchg16b(cur→0)`（禁裸 16B 写）。 |
| **R5** | handler 叶子 | 被调函数（如 `remote_invalidate_seg`）有界、无锁、无 RPC、无分配、**不等待**；大 packet 需分块以界住时延。 |
| **R6** | kick 语义 | slot 是唯一真值，IPI(253) 只是"催促"；因 R4 保证 **同一 target ≤1 个 armed 请求**，1 个 pending kick 恒够；发送方**每轮重试重 kick**，化解硬件同向量 IRR 合并 + 迟到消费。 |
| **R7** | 停机摘核 | `fly_ipi_send` 的 RUNAWAY handler 末尾 `ud2`，被调核**永久死亡**；shootdown 前必须把停机核**移出** `nproc`/活动掩码，否则必超时。 |

落地件（已装）：`util/lock.h` 的 **`enable_interrupt_guard`**（`interrupt_guard` 的对偶：进作用域 STI，
出作用域按进入前 IF 还原）；`returnable_ipi_send` / `fly_ipi_send` / `broadcast_invalidate_tlb` 均已改用。

---

## 3. `broadcast_invalidate_tlb` 编排（v4）

```
// 线程态、不持锁、IF=1（入口再保险一层 enable_interrupt_guard）
remote_invalidate_seg(pak);                       // 本核直调
for (round = 0; confirmed < nproc; ++round) {
    if (now - t0 >= DEADLINE) -> panic/timeout;   // 轮首检查；DEADLINE 必须 > 单次 RPC 超时
    for (pid != self, 未确认) {
        r = returnable_ipi_send(remote_invalidate_seg, pak, pid);
        if (r == OK) confirmed++;
        else /* BUSY / 超时 */ ;                  // 本轮跳过，下轮重 arm + 重 kick
    }
    backoff();                                     // 错峰，防 N 核互饿
}
```

---

## 4. 静态推演：无死锁

- **等待边** X→Y：X 卡在 `returnable_ipi_send(Y)` 的轮询里。
- X 在等待期间**唯一持有**的是 Y 的 slot（try-lock 占位）；Y 完成服务**不需要任何 X 持有的资源**
  —— Y 的动作只是「取中断 → 读自己 slot → 跑有界 handler → 回写 → EOI」，既不碰 X 的 slot 也不碰锁。
- ⇒ 每条等待边的对端都能在不依赖发送方的前提下**有限步推进**（R2 保证 kick 到达即被取；R5 保证 handler 有界）。
- **反证**：若存在环 X₁→X₂→…→X_k→X₁，则每个 X_i 在服务自己 handler 前都要等 X_{i+1}。
  但 X_i 服务所需资源全部自给（自身 slot + kick），且 X_i 在 RPC 期间 IF=1，故其 kick 必被投递并在
  有界步内跑完 handler ⇒ 某条边提前消失 ⇒ 无环。**⇒ 无死锁。**

> R2 / R3 / R5 是充分条件的三块基石；**任一块破了循环就能回来**（v3 正是 R2 破）。
> 必要条件：调用点不持 IF=0 临界区（否则 STI 会把中断开进锁里，破坏锁的"同核不重入"前提）。

---

## 5. 轮子状态（不变量完整性）

| 项 | 状态 | 落地 |
|----|------|------|
| R1/R3 硬断言 | ✅ 已装 | 导出 `local_irq_enabled()`；`returnable_ipi_send`/`fly_ipi_send` 入口断言 IF==1 |
| DEADLINE 重标 | ✅ 已装 | `broadcast_invalidate_tlb`：50ms → `500ms + nproc*1ms`（> 单次 RPC 100ms） |
| R5 大 packet 分块 | ✅ 已装 | 按 `TLB_BROADCAST_CHUNK_PAGES=4096` 拆单 entry 子包逐包广播 |
| R6 迟到消费重检 | ✅ 已装 | 超时路径先看 `lo64==1` 按成功；否则确认是本请求再原子释放（防 slot 卡死）；每轮重臂+重 kick |
| R6 字面 seq | ⏸ 缓 | 同 func/arg 重臂的 `cmpxchg16b` 误判已被“超时重检”消解（语义无害）；待真正需要再加 seq 字段 |

**R1/R3 实现取舍**：未用每核 `in_irq_depth`/`spin_depth` 计数，而采用 **IF==1 入口断言**：
中断门（IPI/#PF）与 `*_interrupt_about_guard` 临界区均把 IF 置 0，两者等价；
IF 判据零热路径开销，也避开了“早期 boot 尚未装 GS 就调 `fast_get_processor_id()`”的风险。
若将来 FRED 路径保持 IF=1，需补显式 irq 深度计数。

**回归**：`mode=s`（pass=5 fail=0 mismatch=0）、`mode=full`（total=35 pass=35 fail=0）；
入口断言未误报 ⇒ 三处 RPC 调用点（broadcast / AP bringup / shutdown）均 IF=1。

---

## 6. 残留风险（非死锁）

- **饥饿/超时**：target 长 IF=0（别的长临界区）或 packet 过大 ⇒ 发送方超时重试（liveness，非死锁）。
- **同向量合并**：靠 R4（≤1 armed/target）+ R6（每轮重 kick）化解。
- **调用点顶层 `interrupt_guard`**：v4 后不再致命（broadcast 内部强制 IF=1），但属冗余项，宜清。
