# block_queue spec（契约，非日志）

> 锁定日期：2026-06-29
> 所有决策已在 `git log` 中记录，此文件只写最终结论。

---

## 1. wq_id_t 句柄系统

Wait queue 通过 `rb_map<wq_id_t, block_queue>` 管理，不暴露裸指针。
`block_queue` 对象内联存储在红黑树节点中（非堆分配），避免不必要的 new/delete。

```cpp
typedef uint64_t wq_id_t;
constexpr wq_id_t  WQ_ID_INVALID = ~0u;

wq_id_t  wq_alloc();                         // 分配一个新 wait queue，返回句柄
void     wq_free(wq_id_t qid);               // 释放
void     wq_wake_one(wq_id_t qid, uint64_t wake_val);  // 唤醒一个等待者
void     wq_wake_all(wq_id_t qid, uint64_t wake_val);  // 唤醒全部等待者

// 内核态专用：dump 出锁指针，供调用方在锁下做 checker 修改 + pop
// 例如 NVMe 中断可直接用 cq.wait_queue 的锁
spinlock_cpp_t* wq_id_to_lock(wq_id_t qid);
```

**设计理由：**
- 句柄防伪、防 UAF
- 用户态可复用相同句柄机制（通过 syscall）
- 内联存储：`block_queue` 对象嵌入 rb_map 节点，不产生额外的动态分配开销

**灵活性（对比旧 `kthread_wait(tid)` 的硬耦合）：**

旧 API 将等待关系绑定到线程 TID 上，只能等特定线程退出。
句柄系统解耦了"等待关系"和"父子关系"：

- 父线程创建一个 wq，以 `wait_other` 使能，将 `wq_id` + `running_word` 嵌入子 task 的数据域
- 父线程调用 `block_if_equal(wq_id, &subtask.running_word, word_running, wait_other, timeout_us)`
- 子线程退出时：原子置 `running_word` 为非 `word_running` → `wq_wake_one(wq_id)`
- 父线程醒来后，该 wq 句柄可复用于其他线程或其他目的

**优势：**
- 自由安排哪个线程"具有被 wait 的能力"（而非硬编码为"等 TID 退出"）
- wq 句柄可复用、可传递，不绑定到特定 task 生命周期
- 超时由 `block_if_equal` 的 `timeout_us` 直接支持，调度器路径统一监控

---

## 2. block_queue 数据结构

组合方式，不继承：

```cpp
class block_queue {
    enum state_t { ready, running } state;  // ready=可复用, running=已分配
    spinlock_cpp_t                qlock;          // 只保护 inner_queue 头尾原子出入
    event_type_t                  queue_event;    // 该 wq 的等待语义（wait_io / wait_other / wait_mutex）
    Ktemplats::list_doubly<task*> inner_queue;    // FIFO：push_back 尾注入，pop_front 头吐出
};

**状态机：** `ready ↔ running`。`wq_alloc` 从 `ready` 取出置 `running`，`wq_free` 归还回 `ready`。
状态切换复用 rb_map 节点内的内联对象，避免节点销毁/重建。
```

**FIFO 纪律：** 先等先服务，`push_back` 入队，`pop_front` 出队。

---

## 3. 冻结语义（核心纪律）

```
入 block_queue 的 task* 状态冻结，不可修改。
必须 pop 出来后，才能拿 task_lock 改状态。

冻结范围：task_state、blocked_reason、on_wait_queue 标记等
冻结目的：qlock 只保护链表头尾，不嵌套 task_lock，规避死锁。
```

---

## 4. 锁序纪律

### 4.1 全局锁层级

调度器涉及三类锁，严格按以下方向拿取，**允许嵌套**（内层锁在高层锁的临界区内获取），**禁止逆向**：

```
wq_lock  >  task_lock  >  sched_lock
(最高)                    (最低)
```

- 允许拿高层锁的临界区内再拿低层锁（嵌套）
- 不允许先拿低层锁再拿高层锁（断链）
- 不相关的两把同级锁（如两个不同 task 的 `task_lock`）按固定顺序：参考 `kthread_exit_cppenter` 的 `exit_task_lock > waiter_task_lock`（先 lock 被引用对象，再 lock 引用者）

---

### 4.2 各调度路径对照

| 路径 | 锁顺序 | 嵌套？ | 位于 |
|---|---|---|---|
| block_if_equal 入队 | `wq_lock > task_lock` | ✅ 嵌套 | `block_if_equal_cppenter` |
| wq_wake_one 唤醒 | `wq_lock` → (放出) → `task_lock → sched_lock` | ❌ 分离 | 待实现 |
| kthread_exit | `exit_task_lock > waiter_task_lock > waiter_sched_lock` | ✅ 三层嵌套 | `kthread_exit_cppenter` |
| kthread_wait | `waited_task_lock > waiter_task_lock` | ✅ 嵌套 | `kthread_wait_cppenter` |
| kthread_sleep | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `kthread_sleep_cppenter` |
| yield / resched | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `kthread_yield_true_enter` / `resched` |
| create_kthread | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `create_kthread` |
| wakeup_thread | `task_lock > sched_lock` | ✅ 嵌套 | `wakeup_thread` |

`→` 表示同一临界区内拿取（嵌套），`>` 表示拿取顺序，`──→(放出)──>` 表示释放锁后在其他临界区拿更低层级锁。

---

### 4.3 入队流程（block_if_equal 内部）

```
  1. wq_lock(qid)                         // 锁 checker 判别
  2.   if checker != NULL && *checker != expected:
  3.     wq_unlock(qid)                    // 条件已不成立，不入队
  4.     return
  5.   task_lock(self)                     // ──── 嵌套在 wq_lock 下 ────
  6.     task_event_shift(wait_type)
  7.     self->set_blocked()
  8.     self->min_wakeup_stamp = ...
  9.   task_unlock(self)
  10.  inner_queue.push_back(self)         // FIFO 尾注入
  11. wq_unlock(qid)
  12. sched()                             // sched() 内部拿 sched_lock
```

---

### 4.4 唤醒流程（wq_wake_one）

```
  1. wq_lock(qid)                         // checker 修改 + pop 绑在一个临界区
  2.   if checker_present:
  3.     *running_word = NON_RUNNING
  4.   t = inner_queue.pop_front()
  5. wq_unlock(qid)
  6.  // ── 锁外分界 ──
  7. task_lock(t)                         // 单独临界区，不嵌套在 wq_lock 下
  8.   t->set_ready()
  9.   t->min_wakeup_stamp = 0
  10. task_unlock(t)
  11. insert_ready_task(t)                // 内部拿 sched_lock
```

为什么唤醒不嵌套？因为唤醒者往往是中断上下文（如 NVMe 完成中断），不应持有 wq_lock 太多时间。把 `task_lock` 放出来后做，满足锁序方向但不嵌套，同样安全。

---

### 4.5 锁序违规示例

```
❌ sched_lock → wq_lock     // 必须先拿 wq_lock
❌ sched_lock → task_lock   // 必须先拿 task_lock
❌ task_lock  → wq_lock     // 必须先拿 wq_lock
❌ 两个不同的 task_lock 无固定顺序获得               // 可能导致 ABBA
```

不同 task_lock 之间的获得顺序由 `kthread_exit_cppenter` 确立规则：**先锁被引用对象（waited），再锁引用者（waiter）**。

---

### 4.6 正确性推理

- `*checker == expected` 的判别和 `*running_word` 的修改都在 `wq_lock` 下 → 互斥，不丢唤醒
- 全部路径锁序方向一致 `wq_lock > task_lock > sched_lock` → 无循环等待（无死锁）
- 出路径（sched）不持有任何调度器锁以外的锁，不会在 context switch 时泄漏锁

---

## 5. 核心原语

### 5.1 block_if_equal（条件阻塞）

```cpp
void block_if_equal(
    wq_id_t          qid,
    uint64_t*        checker,     // nullptr 等价无条件阻塞
    uint64_t         expected,    // *checker == expected 时阻塞；checker=NULL 时忽略
    event_type_t     wait_type,   // 记入 accumulates_bank 的桶 (wait_io/wait_other/wait_mutex)
    uint64_t         timeout_us   // 0 = 不限时
);
```

- 切出前自动 `task_event_shift(wait_type)`
- 醒来后调用方自行 `task_event_shift(run_kthread)` + 重检 checker
- 超时唤醒不经 wq，调度器直接 `set_ready()`，因此醒来必须先重检

### 5.2 kthread_sleep（定时睡眠）

```cpp
void kthread_sleep(miusecond_time_stamp_t offset_us);
```

- 保留，不依赖 wq，靠 timer 自唤醒
- 切出前自动 `task_event_shift(sleep)`

---

## 6. event_type 与 wait_type 映射

| event_type | 场景 | accumulates_bank 索引 |
|---|---|---|
| `wait_io` | NVMe 等待、外设等待、渲染线程空闲 | `accumulates_bank[wait_io]` |
| `wait_other` | 等另一 kthread exit、等锁（非 IO） | `accumulates_bank[wait_other]` |
| `wait_mutex` | 互斥锁等待 | `accumulates_bank[wait_mutex]` |
| `sleep` | 仅 `kthread_sleep` 内部使用 | `accumulates_bank[sleep]` |

所有 event_type 定义在 `task::event_type_t` 中（含 `run_kthread`/`run_uthread`/`run_vCPU`/`offline`/`init`）。

---

## 7. timeout_us 机制

```
block_if_equal 带超时：
  self->min_wakeup_stamp = now + timeout_us    // 设置超时边界
  入队，sched
  醒来：self->min_wakeup_stamp = 0             // 清超时

超时唤醒路径（调度器遍历 sleep_queue）：
  sleep_queue 按 min_wakeup_stamp 升序
  队头扫描，遇到 min_wakeup_stamp==0 或未到期 → 停
  到期 task：pop → clear_min_wakeup → task_event_shift(run_kthread) → set_ready → 入 ready_queue
```

---


