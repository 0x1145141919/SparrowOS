# block_queue 归一化（v2）

> 2026-06-28 — 第二次修订
> 整合 accumulates_bank、缩减阻塞接口面

---

## 一、动机（同 v1）

### 1.1 NVMe 初始化超时无兜底

### 1.2 现行两套阻塞机制并行

| 机制 | 队列 | 唤醒方式 | 超时 |
|---|---|---|---|
| `kthread_wait` | `task->waiters` | exit 时 drain | ❌ |
| `block_queue` / `block_if_equal` | `tid_wait_queue` | `release_kthread` / `wakeup_thread` | ❌ |

### 1.3 锁序交叉（同 v1）

---

## 二、核心思想：只留两个原语

`step1` 的 wq 系统仍然成立，但减少暴露的接口：

```
所有阻塞效果：
  ┌─ 条件阻塞   → block_if_equal(wq, checker, expected)
  └─ 定时阻塞   → kthread_sleep(offset_us)

取消的接口：
  kthread_wait          → block_if_equal(target->exit_wq, &target->status, DEAD)
  block_queue           → block_if_equal(wq, NULL, 0)  // 无条件
  kthread_block         → block_if_equal(self_wq, NULL, 0)
  kthread_self_blocked  → block_if_equal(self_wq, &flag, NO_WORK)
```

### 2.1 现存的阻塞接口迭代

**`kthread_wait(tid)`：**
```
→ block_if_equal(target->exit_wq, &target->task_state, zombie)
  - checker 轮询：target→task_state == zombie
  - 相等则阻塞在 exit_wq 上
  - 不等则直接返回（target 已经死了）
```

**`kthread_self_blocked(reason)` vs `wakeup_thread`（textConsole 模式）：**
```
// current:
kthread_self_blocked(no_job);

// future:
block_if_equal(render_wq, &render_job_pending, false)
// 当 render_job_pending == false 时阻塞
// wakeup_thread → render_job_pending = true → wq_wake_one(render_wq)
```

**`kthread_sleep(offset_us)`：**
```
单独保留。区别：
  sleep 不依赖外部 wake 源，靠 timer 到期自唤醒
  block_if_equal 依赖 checker 变真或被别人 wq_wake
```

### 2.2 阻塞与 accumulates_bank 的集成

当 task 因 `block_if_equal` 或 `kthread_sleep` 切走时，**通过 `task_event_shift` 记录时间段**：

```
// block_if_equal 切走前:
task_event_shift(wait_io  / wait_other);   // 取决于等待性质
sched();

// kthread_sleep 切走前:
task_event_shift(sleep);
sched();

// 被唤醒、回到 CPU 后:
task_event_shift(run_kthread);
// 继续执行
```

这意味着 `accumulates_bank` 的维护和阻塞接口的调用天然绑定——**不需要单独的"进入阻塞"和"记录时间"两步操作**，`block_if_equal` 内部在切出前自动做 `task_event_shift`，醒来后第一个恢复的是 `task_event_shift(run_*)`。

### 2.3 核心接口设计

```cpp
// ── 原语 ──

// 条件阻塞：当 *checker == expected 时阻塞在 wq 上
// 若 checker == nullptr 则无条件阻塞（等价旧 block_queue）
// timeout_us = 0 表示不限时
//
// 切出前自动 task_event_shift(wait_type)
// 唤醒后调用方自行 task_event_shift(run_*) / 重检 checker
void block_if_equal(
    tid_wait_queue* wq,
    uint64_t*       checker,
    uint64_t        expected,
    event_type_t    wait_type = wait_other,  // 记录到哪个 accumulate 桶
    uint64_t        timeout_us = 0
);

// 定时睡眠（特殊化，靠 timer 自唤醒）
// 切出前自动 task_event_shift(sleep)
void kthread_sleep(miusecond_time_stamp_t offset_us);

// ── 辅助（语法糖，不增加新原语） ──

// kthread_exit 现在只需要：
//   1. wq_wake_all(self->exit_wq, exit_code)
//   2. 清理资源
//   3. sched()
```

---

## 三、textConsole 案例

`src/utils/textConsole.cpp` 现行逻辑：

```cpp
// 渲染线程入口:
for (;;) {
    if (no_work_to_do) {
        kthread_self_blocked(task_blocked_reason_t::no_job);  // line 530
    }
    render_frame();
}

// 业务线程:
submit_render_job();
wakeup_thread(render_tid);  // line 399
```

归一化后：

```cpp
// 渲染线程:
static tid_wait_queue render_wq;
static bool           render_pending = false;

for (;;) {
    if (!render_pending) {
        // 无条件阻塞的 block_if_equal ≈ 旧 block_queue
        // 但写法统一：checker=NULL 跳过检查
        block_if_equal(&render_wq, nullptr, 0, wait_io);
        // 醒来：render_pending == true
    }
    render_pending = false;
    do_render();
}

// 业务线程:
render_pending = true;
wakeup_thread(render_tid);
// 或 future:
wq_wake_one(&render_wq, 0);
```

优势：
- 去掉了 `kthread_self_blocked` 这个特殊接口
- 阻塞原因（`wait_io`）直接体现到 `accumulates_bank` 中
- 同样的 `wq` 机制，不需要单独维护 `blocked_reason` 状态

---

## 四、block_queue 数据结构（组合方式 + 冻结语义）

### 4.1 结构定义

```cpp
class block_queue {
    spinlock_cpp_t                lock;        // 只保护 task_list 头尾的原子出入
    Ktemplats::list_doubly<task*> task_list;   // 内联组合（非继承）
    event_type_t                  wait_type;   // 语义：wait_io / wait_other
                                                // wake_one/wake_all 开新 event 时归档到此桶
};
```

**组合 vs 继承的取舍：**
- 继承空壳 `class block_queue:list_doubly<task*>{}` 暴露整个 list 接口，锁纪律无法内建
- 组合将锁和容器绑在一起，对外只暴露窄接口（`push`、`pop_front`、`wake_one`、`wake_all`、`size`）
- `wait_type` 跟着队列走，`block_if_equal`/`wake_one`/`wake_all` 内部靠此自动做 `task_event_shift`

### 4.2 冻结语义（核心纪律）

```
┌───────────────────────────────────────────────────────┐
│                                                       │
│  入 block_queue 的 task* 状态冻结，不可修改。          │
│  必须 pop 出来后，才能拿 task_lock 改状态。            │
│                                                       │
│  冻结范围：task_state、blocked_reason、                │
│  blocked_by_queue（调试用）、on_wait_queue 标记        │
│                                                       │
│  冻结目的：wq->lock 只保护链表头尾原子性，不嵌套       │
│  task_lock。去掉了旧设计中的锁嵌套，规避死锁。         │
│                                                       │
└───────────────────────────────────────────────────────┘
```

**约束推理：**
- wq 内 task* 只有唯一性 + 只读引用
- pop 出来后持有 wq 锁的代码才释放 task*（但此时已脱离队列）
- 对于唤醒来讲：wq 锁内只弹 task*，锁外才拿 task_lock → set_ready
- 对于入队来讲：`block_if_equal` 在入队前已完成 `task_event_shift` + `set_blocked()`（此时 task 自己的锁还在手），入队后 unlock、sched

**好处：**
- 避免了 `wq_lock` 和 `task_lock` 的嵌套交叉（旧 §四 所示）
- wq 锁临界区极窄，只在链表的 push/pop 瞬间
- 唤醒时不再需要在 wq 锁内拿 task_lock，可以多个 waiter 并行处理

---

## 五、锁序纪律（基于冻结语义）

```
block_if_equal 内部（入队冻结）:
  1. self->task_lock                // 改自己的状态
  2.   task_event_shift(wait_type)   //   关旧帐
  3.   self->set_blocked()           //   改 task_state
  4.   self->task_lock.unlock()      // ← 改完再取 wq 锁
  5.   wq->lock                      // 插入 waiter（task* 已冻结）
  6.     wq->task_list.push_back(self)
  7.   wq->lock.unlock()
  8. sched()                        // 切走

wq_wake_one / wq_wake_all（弹出解冻）:
  1. wq->lock                       // 弹出 waiter
  2.   task* t = wq->task_list.pop_front()
  3. wq->lock.unlock()              // ← 先放锁，再逐 task 处理
  4. for each popped task* t:
       t->task_lock
         t->set_ready()             // 解冻、改状态
       t->task_lock.unlock()
       insert_ready_task(t)         // 推入调度器就绪队列（内部 sched_lock）
```

**关键变化：** `wq→lock` 和 `task→lock` 不再嵌套。
- 入队方向：`task_lock`（先改好状态）→ `wq_lock`（只插指针）
- 唤醒来向：`wq_lock`（只弹指针）→ `task_lock`（单独处理每个 task）

固定传递方向：`task_lock → wq_lock`（入队），`wq_lock → task_lock`（唤醒），方向相反互不嵌套。

---

## 六、event_type 与 block 的映射规范

`block_if_equal` 的 `wait_type` 参数（来自 `block_queue.wait_type`）决定 `task_event_shift` 归档到哪个桶：

| wait_type | 使用场景 | accumulates_bank 索引 |
|---|---|---|
| `wait_io` | NVMe 等待、外设等待、渲染线程空闲 | `accumulates_bank[wait_io]` |
| `wait_other` | 等另一个 kthread exit、等锁（非 IO） | `accumulates_bank[wait_other]` |
| `wait_mutex` | 互斥锁等待 | `accumulates_bank[wait_mutex]` |
| `sleep` | 仅 `kthread_sleep` 内部使用 | `accumulates_bank[sleep]` |

## 八、timeout_us 与 min_wakeup_stamp 的关系

`block_if_equal` 的 `timeout_us` 参数通过 `task::min_wakeup_stamp` 实现：

```
block_if_equal(wq, checker, expected, timeout_us):
  if *checker == expected:
    task_event_shift(wq->wait_type)
    if timeout_us != 0:
      self->min_wakeup_stamp = now + timeout_us   // 设超时边界
    else:
      self->min_wakeup_stamp = 0                   // 不限时
    self->set_blocked()
    wq->push(self)                                  // 冻结入队
    sched()                                          // 切走
    // 醒来（被 wq_wake 或超时强制唤醒）:
    self->min_wakeup_stamp = 0                      // 清超时
    task_event_shift(run_kthread)                   // 开新帐
```

**超时唤醒路径（调度器/timer tick）：**

```
for each blocked task in sleep_queue:
  if task->min_wakeup_stamp != 0
     && now > task->min_wakeup_stamp
     && task->current_event ∈ {sleep, wait_io, wait_other, wait_mutex}:

    task->min_wakeup_stamp = 0
    task_event_shift(run_kthread)
    task->set_ready()
    insert_ready_task(task)
```

注意：超时唤醒不通过 wq，而是调度器直接设置 `set_ready()`。
这意味着 `block_if_equal` 醒来后第一件事是**重检 checker**——
因为超时不等于条件满足。

调用方根据直觉选 `wait_io` 还是 `wait_other`——这直接决定调度器的行为画像准确性。

---

## 七、textConsole 迁移路线

> 注意：`block_queue` 从继承改为组合后，`tid_wait_queue` 退役，统一使用 `block_queue`。
> `block_if_equal` 的参数 `wq` 从 `tid_wait_queue*` 更新为 `block_queue*`。

```
Phase 0 — 清理 header
  ─ 砍掉 task::waiters
  ─ 砍掉 kthread_wait / kthread_block / kthread_self_blocked 声明
  ─ 砍掉 tid_wait_queue 声明
  ─ 保留 block_if_equal + kthread_sleep 为新原语

Phase 1 — block_queue 数据结构落地
  ─ 组合方式：lock + list_doubly<task*> + wait_type
  ─ 冻结语义：wq 内 task* 只读，pop 后拿 task_lock 改状态
  ─ wake_one / wake_all 成员方法

Phase 2 — 接口重写
  ─ kthread_exit_cppenter → self->exit_wq.wake_all()
  ─ kthread_wait          → block_if_equal(&target->exit_wq, &state, zombie)
  ─ block_queue           → block_if_equal(&wq, NULL, 0)
  ─ textConsole.cpp       → block_if_equal(&render_wq, &pending, false)
  ─ kthread_self_blocked  → 删除，所有调用点改为 block_if_equal

Phase 3 — NVMe init 超时
  ─ block_if_equal(&wq, ..., timeout_us=5'000'000)
```

*Phase 0 可在当前会话直接落地，不阻塞其他改动。

---

*Phase 0 可在当前会话直接落地，不阻塞其他改动。*
