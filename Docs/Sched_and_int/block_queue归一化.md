# block_queue 归一化草案

> 2026-06-27 — 草稿 / 待定

---

## 一、动机

### 1.1 NVMe 初始化时序的触发性问题

现行 `kthread_wait(tid)` 无超时兜底：

```
NVMe_Controller::Init():
  for i = 0..N:
    tids[i] = create_kthread(init_thread_entry, ...)
  for i = 0..N:
    kthread_wait(tids[i])       ← 若 init 线程卡死（MMIO超时、硬件bug），永久阻塞
                                  → 整个系统boot卡死，无输出、无恢复路径
```

### 1.2 扩展性问题

现行阻塞机制有两套：

| 机制 | 队列 | 唤醒方式 | 是否支持超时 |
|---|---|---|---|
| `kthread_wait` | `task->waiters` (链表) | exit 时 drain | ❌ |
| `block_queue` / `block_if_equal` | `tid_wait_queue` (链表) | `release_kthread` / `wakeup_thread` | ❌ |

两套做同一件事（task 阻塞等待通知），但语义隔离。锁序也不同。

### 1.3 锁序隐患

```
kthread_exit_cppenter:
  waiter_task->task_lock.lock()
    → target_scheduler.sched_lock.lock()
      → target_scheduler.insert_ready_task(waiter_task)   // waiter 被唤醒

kthread_wait_cppenter:
  waited_task->task_lock.lock()
    → waiter_task->task_lock.lock()
      → waited_task->waiters.push_back(waiter)
```

exit 拿 waiter 的锁再拿 scheduler 的锁；wait 拿 waiter 的锁还拿 waited 的锁。不同路径锁序不同。

---

## 二、归一化设计

### 2.1 核心思想

统一为单一原语——**全局 wait_queue 句柄系统**：

```
block_queue（tid_wait_queue）是唯一的原语。
  ─ 等待在一个队列上
  ─ 被别人从队列中唤醒
  ─ 可设定超时

其他机制都是其特例：
  kthread_wait   = block on target->exit_wq
  kthread_exit   = wake_all(self->exit_wq, exit_code)
  block_queue    = block on custom wq
  block_if_equal = 条件检查 + block on custom wq
```

### 2.2 全局 wait_queue 句柄

中心化注册表，各 CPU 的 `resched()` 都能遍历检查超时：

```cpp
constexpr uint32_t MAX_GLOBAL_WQ = 256;

struct wq_slot {
    // ── 队列本体 ──
    uint64_t    entries[MAX_ENTRIES];   // 存放 waiter tid
    uint32_t    head, tail, count;

    // ── 超时控制 ──
    uint64_t    deadline_us;            // 若 count>0，超时截止时间戳
    // 注意：deadline 按 最长剩余 或 最早到期的条目 设定，
    //       取决于实现策略（详见§4）

    // ── 元数据 ──
    bool        in_use;
    // 可扩展：绑定 tid（用于 exit_wq 反向查找）
};

alignas(64) wq_slot g_wq_table[MAX_GLOBAL_WQ];
```

接口：

```cpp
wq_handle_t wq_create();          // 注册 → 返回句柄（= table index）
void        wq_destroy(wq_handle_t);

// 阻塞
uint64_t    wq_wait(wq_handle_t, uint64_t timeout_us);
// wq_wait 返回值的语义：
//   位63 = 0 → 正常唤醒（调用方自行检查业务条件）
//   位63 = 1 → 超时唤醒（调用方自行决定是否重试/回退）
//   低位  = 唤醒者传递的值（exit_code / 或其他业务信息）

// 唤醒
uint64_t    wq_wake_one(wq_handle_t, uint64_t wake_val);
void        wq_wake_all(wq_handle_t, uint64_t wake_val);

// 超时扫描（各 CPU resched 末尾调用）
void        wq_check_timeouts();
```

### 2.3 `task` 结构变化

```cpp
class task {
    // ── 移除 ──
    // Ktemplats::list_doubly<task*> waiters;     ← 砍掉

    // ── 新增 ──
    wq_handle_t exit_wq;       // 绑定的退出通知队列
};
```

每个 task 在 `alloc_tid` 时自动 `exit_wq = wq_create()`；`release_tid` 时 `wq_destroy(exit_wq)`。

### 2.4 现有接口重写

```
kthread_wait(tid, timeout_us):
  等价于:
    task* target = get_by_tid(tid);
    uint64_t ret = wq_wait(target->exit_wq, timeout_us);
    return ret;
    // 位63=0 → exit_code 在低位
    // 位63=1 → 超时，低位无意义

kthread_exit(uint64_t code):
  // 原：遍历 waiters → 一一 set_ready + 塞 exit_code
  // 新：
    wq_wake_all(self->exit_wq, code);
    // 退出原语（清理资源等）
    set_zombie();
    sched();

block_queue(wq_handle_t hq):
  等价于 wq_wait(hq, 0);  // 0 = 无限等待

block_if_equal(wq_handle_t hq, uint64_t* checker, uint64_t token):
  // 原：if (*checker == token) { block; }
  // 新：
    if (*checker == token) {
        ret = wq_wait(hq, timeout_us);
        // 回来后 *checker 可能已经变了，也可能没变
        // 调用方自行判断
    }

wakeup_thread(tid):
  等价于 wq_wake_one(target->exit_wq, wake_val);

release_kthread(tid):
  等价于 wq_wake_all(target->exit_wq, exit_code) + 资源释放
```

---

## 三、锁序改善

归一化后所有阻塞操作走同一条路径：

```
wq_wait:
  1. self->task_lock.lock()
  2.   wq_table[wq].lock.lock()
  3.     entry插入
  4.   wq_table[wq].lock.unlock()
  5.   set_blocked()
  6. self->task_lock.unlock()
  7. sched()  // 切走

wq_wake_all:
  1. wq_table[wq].lock.lock()
  2.   pop 所有 tid
  3. wq_table[wq].lock.unlock()
  4. for each tid:
       target = get_by_tid(tid)
       target->task_lock.lock()
         set_ready(target)
         target->context->rax = wake_val
       target->task_lock.unlock()
       insert_ready_task(target)
       // 注：insert_ready_task 需要 target CPU 的 sched_lock
       //     拿到 task_lock 后才能取 belonged_processor_id 确定目标CPU
```

exit 不再需要自己遍历 waiters、逐个拿 waiter 的锁、再拿 scheduler 的锁。只需要对 `exit_wq` 做 `wq_wake_all`——wq 内部统一了锁序。

**固定的锁序纪律：**

```
wq_table[wq].lock  →  task_lock  →  sched_lock
         (1)             (2)            (3)
```

不再有 "exit: waiter→lock then sched_lock" 和 "wait: waited→lock then waiter→lock" 的交叉。

---

## 四、超时检测时机与语义

### 4.1 检测触发

不依赖专用定时器线程。**各 CPU 在 `resched(ctx)` 末尾调用 `wq_check_timeouts()`**：

```cpp
// resched() 内部:
  cpu_self = fast_get_processor_id();
  {
     reentrant_spinlock_guard g(self_scheduler.sched_lock);
     // 正常调度逻辑...
  }
  wq_check_timeouts();      // ← 新增：当前 CPU 顺手扫一次
  scheduler.sched();        // 切走
```

扫描频率取决于 `resched()` 被调用的频率。在 timer tick / syscall / yield / exit 等路径上都会触发。

### 4.2 deadline 设定策略

最简单的实现——**每个 wait_queue 记录一个 `deadline_us`，取队列中所有条目的最早到期时间**：

```cpp
wq_wait(hq, timeout_us):
  if (timeout_us > 0) {
      deadline = now + timeout_us;
      if (deadline < wq->deadline_us)
          wq->deadline_us = deadline;  // 更新为更早的到期时间
  }

wq_check_timeouts():
  now = get_microsecond_stamp();
  for each wq_slot in use:
      if (wq->count > 0 && now >= wq->deadline_us) {
          // 遍历 entries，检查每个 entry 的实际 deadline
          // 到期者 → 从队列移除 → set_ready → 位63置1
          // 重新计算下一个 deadline
      }
```

### 4.3 返回值语义

```cpp
// 调用方示例 — NVMe init 场景:

uint64_t ret = kthread_wait(tids[i], 5'000'000);  // 5秒超时
if (ret & WAKE_CAUSE_TIMEOUT) {
    // 超时了。业务条件（init 成功？）自行判断
    // 通常意味着该盘初始化失败
    bsp_kout << "[NVMe] node " << i << " init timeout" << kendl;
    fail_count++;
} else {
    // 正常被退出唤醒
    ok_count++;
}

// 调用方示例 — block_if_equal:
uint64_t ret = wq_wait(hq, 100'000);  // 100ms超时
if (ret & WAKE_CAUSE_TIMEOUT) {
    // *checker 可能达到，也可能没达到
    // 调用方自己判断：
    if (*checker == expected) {
        // 实际上在超时前一刻达到了，只是通知延迟
        goto success;
    }
    // 真的超时了
    goto timeout_handle;
}
// 正常唤醒
if (*checker != expected) {
    // 被误唤醒？重试或继续等
    goto retry;
}
```

---

## 五、遗留问题（草案级别）

1. **`wq_check_timeouts()` 的性能** — 遍历 256 个 slot 每次 resched 可能热。考虑 bitmap 标记非空 wq 来跳过。
2. **deadline 重算粒度** — 扫描时若 wq 中 entry 很多且到期时间分散，无效检查多。但预计每个 wq 的 entry 数不多（NVMe N ≤ 32）。
3. **wq_wait 无锁等待** — 当前用 `sched()` 切走，wq 的 lock 在 block 前已释放。如果需要在被唤醒时自动获得 wq 锁？暂时不需要。
4. **`kthread_wait` 的 timeout 语义** — 超时后 `target->exit_wq` 里还有该 waiter 的残留 entry 吗？需要清除或标记 skip（参考 CAS outcome 仲裁）。
5. **task 析构时绑定 wq 的释放** — `release_kthread` 需确保 exit_wq 已空（没有人在等了）。
6. **`MAX_GLOBAL_WQ` 用尽** — 设计上 wq 是静态数组，用完则 `wq_create()` 返回错误。正常情况下每个 task 一个 exit_wq + 少量业务 wq 够用。

---

## 六、进度路线（建议）

```
Phase 1 ─ 新建 wait_queue 子系统
  ─ wq_table / wq_create / wq_destroy / wq_wait / wq_wake_one / wq_wake_all
  ─ wq_check_timeouts() + resched 集成
  ─ 测试：基本阻塞唤醒 + 超时

Phase 2 ─ 现有接口迁移
  ─ task: 加 exit_wq 字段，去 waiters 字段
  ─ kthread_exit_cppenter: 改为 wq_wake_all(self->exit_wq, code)
  ─ kthread_wait: 改为 wq_wait(target->exit_wq, timeout)
  ─ block_queue / block_if_equal: 改为 wq_wait
  ─ wakeup_thread: 改为 wq_wake_one(target->exit_wq)
  ─ release_kthread: 改为 wq_wake_all + wq_destroy

Phase 3 ─ NVMe Init 迁移
  ─ kthread_wait(tid) → kthread_wait(tid, 5'000'000)
  ─ 超时处理逻辑
```

Phase 1 和 Phase 2 可以同步进行（先写新子系统再重写现有入口）。Phase 3 改动极浅，主要是参数调整。

---

*此文是草案。Phase 1 落地前锁序、仲裁策略、返回值编码需要实作中验证。*
