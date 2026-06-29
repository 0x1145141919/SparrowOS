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

两条路径，方向固定，永不嵌套：

```
入队（block_if_equal 内部）：
  1. self->task_lock               // 改自己状态
  2.   task_event_shift(wait_type)  //   关旧帐
  3.   self->set_blocked()
  4.   self->min_wakeup_stamp = ... // 设超时（可选）
  5. self->task_lock.unlock()       // ← 改完再取 qlock
  6. qlock                          // 插入 waiter（task* 已冻结）
  7.   inner_queue.push_back(self)  //   FIFO 尾注入
  8. qlock.unlock()
  9. sched()                        // 切走

唤醒（wq_wake_one / wq_wake_all）：
  1. qlock                          // 弹出 task*
  2.   t = inner_queue.pop_front()
  3. qlock.unlock()
  4. t->task_lock                   // 弹出后才能改
  5.   t->set_ready()
  6.   t->min_wakeup_stamp = 0
  7. t->task_lock.unlock()
  8. insert_ready_task(t)
```

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


