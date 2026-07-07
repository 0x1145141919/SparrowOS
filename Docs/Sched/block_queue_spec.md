# block_queue spec（契约，非日志）

> 最终版 — 2026-07-01
> 所有决策已在 `git log` 中记录，此文件只写最终结论。

---

## 1. 全局容器 + bq_id_t 句柄系统

Block queue 通过全局 `rb_map<bq_id_t, block_queue*>` 管理。

- bq_id_t 是跨边界句柄（跨进程、中断、syscall），**不参与内核内热路径**
- 内核内热路径通过 `get_lock(qid)` → `block_queue*` → 全指针操作，不反复查容器
- 容器锁 `container_lock`（spinrwlock）保护 map 结构，写者仅 bq_alloc/bq_free

```
                 container_lock (rwlock)
                      │
                 rb_map<qid, block_queue*>
                      │
                 get_lock(qid) → block_queue*
                      │
                 qlock.lock()
                 queue->pop_head() / pop_all() / pop_timeouts()
                 qlock.unlock()
                 bq_flush_pending(clamp)    // 出锁后批量 set_ready
```

```cpp
typedef uint64_t bq_id_t;
constexpr bq_id_t  BQ_ID_INVALID = ~0u;

bq_id_t  bq_alloc(block_queue* q);          // 注册预分配的 BQ，返回句柄
ckurd    bq_free(bq_id_t qid);              // 注销句柄，返回 KURD raw

block_queue* get_lock(bq_id_t id);          // 通过 qid 查找 BQ 指针，没找到返回 NULL

void bq_flush_pending(blocked_tasks_clamps_t* clamp); // 出锁后批量 set_ready
```

**设计理由：**
- qid 只做**跨边界句柄**（用户态 syscall、中断 handler），不做操作载体
- 热路径（block_if_equal/wake）走指针，不反复查容器 → 减少读锁竞争
- 容器保留全量 BQ 枚举能力 → 集中超时扫描（§7）
- 写者极罕见（仅 alloc/free），读者占绝大多数，rwlock 场景适配

---

## 2. block_queue 数据结构

```cpp
struct blocked_tasks_clamps_t {
    uint8_t batch_count;          // 当前 batch 中 task 数
    bool    is_queue_empty;       // 源队列是否已空
    task*   arr[64];              // 固定 64 槽
};

class block_queue {
    enum state_t { ready, running } state = ready;
    task::event_type_t            queue_event;
    Ktemplats::list_doubly<task*> inner_queue;   // FIFO：push_back 尾注入，pop_front 头吐出
public:
    spinlock_cpp_t qlock;                         // 只保护 inner_queue 头尾原子出入

    block_queue() : state(ready), qlock{}, queue_event{}, inner_queue{} {}

    // 生命周期控制
    KURD_t enable_queue(task::event_type_t type); // ready+empty → running
    KURD_t disable_queue();                        // empty → ready
    bool   is_queue_ready();                       // ready && empty

    // 弹出操作（在 qlock 临界区内调用）
    task*  pop_head();                              // 吐出一个，返回 nullptr 若空
    void   pop_timeouts(blocked_tasks_clamps_t*);   // 弹超时 → batch
    void   pop_all(blocked_tasks_clamps_t*);        // 弹全部 → batch
};
```

**状态机：** `ready ↔ running`
- `block_queue()` 构造 → `ready`
- `enable_queue` → `ready`→`running`（只有 ready+empty 才能成功）
- `disable_queue` → `running`→`ready`（只有 empty 才能成功）
- `bq_free` → 从容器移除并 `delete`（只能 free ready 态）

**FIFO 纪律：** 先等先服务，`push_back` 入队，`pop_front` 出队。
**batch 模式：** pop 操作将 task 塞入 `blocked_tasks_clamps_t`（栈上分配），出 qlock 临界区后统一 `bq_flush_pending` 批量 `set_ready`，减少临界区时长。

### 与旧版对比

| 旧版 | 新版 | 理由 |
|------|------|------|
| 自由函数 `bq_wake_push(qid)` | 成员函数 `pop_head()` | 去 qid 反复查找 |
| 自由函数 `bq_wake_all(qid)` | 成员函数 `pop_all(clamp)` | batch + 出锁 set_ready |
| 自由函数 `bq_wake_timeouts(qid)` | 成员函数 `pop_timeouts(clamp)` | batch + 出锁 set_ready |
| `enable_queue(qid, type)` 自由函数 | `block_queue::enable_queue(type)` | 成员函数，操作 this |
| `disable_queue(qid)` 自由函数 | `block_queue::disable_queue()` | 同上 |
| `get_lock` 返回 `spinlock_cpp_t*` | 返回 `block_queue*` | caller 自己 `.qlock.lock()` |
| `bq_alloc()` 无参 | `bq_alloc(block_queue* q)` | caller 预分配，生命周期解耦 |

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

调度器涉及四类锁，严格按以下方向拿取：

```
container_lock  >  qlock  >  task_lock  >  sched_lock
   (最高)                                         (最低)
```

- 允许拿高层锁的临界区内再拿低层锁（嵌套）
- 禁止逆向拿取
- container_lock 仅保护 rb_map 结构，不保护 queue 状态
- qlock 保护 inner_queue 出入 + 状态字段

### 4.2 各调度路径对照

| 路径 | 锁顺序 | 嵌套？ | 位于 |
|---|---|---|---|
| bq_alloc / bq_free | `container_lock` 写锁 | 独占 | `bq_system.cpp` |
| get_lock | `container_lock` 读锁（查 map），不拿 qlock | 只读 | `bq_system.cpp` |
| block_if_equal 入队 | `container_lock` 读锁 → `qlock` → `task_lock` | ✅ 三层嵌套 | `block_if_equal_cppenter` |
| pop_head / pop_all / pop_timeouts | 调用方已持 `qlock` | ❌ 调用方负责 | 调用者上下文 |
| bq_flush_pending | 无锁（qlock 已放） | ❌ | 调用者上下文 |
| kthread_exit | `self_task_lock` → `sched_lock` | ❌ 分离（exit 不再遍历 waiters） | `kthread_exit_cppenter` |
| kthread_wait | **已移除** — 2026-07-02，waiters 链表一并清除。见 task_v3设计.md。 | — | — |
| kthread_sleep | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `kthread_sleep_cppenter` |
| yield / resched | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `kthread_yield_true_enter` / `resched` |
| wakeup_thread | `task_lock > sched_lock` | ✅ 嵌套 | `wakeup_thread` |

### 4.3 入队流程（block_if_equal 内部）

```
  1. container_lock 读锁            // 保护 map 的读取
  2. q = get_lock(qid)               // container->find
  3. container_lock 读解锁
  4. q->qlock.lock()                  // qlock（bq_lock）
  5.   if checker != NULL && *checker != expected:
  6.     q->qlock.unlock()
  7.     return
  8.   task_event_shift(wait_type)   // 关旧帐、开新帐
  9.   task_lock(self)                // ──── 嵌套在 qlock 下 ────
  10.    self->set_blocked()
  11.    self->blocked_reason = ...
  12.    self->min_wakeup_stamp = timeout_us ? now + timeout_us : 0
  13.  task_unlock(self)
  14.  q->inner_queue.push_back(self) // FIFO 尾注入
  15. q->qlock.unlock()
  16. bq_timeout_scan_rand()          // 随机选一个 BQ 扫超时
  17. sleep_tasks_wake()              // 扫 sleep_queue
  18. sched()                         // 内部拿 sched_lock
```

### 4.4 唤醒流程（pop → flush_pending 两步分离）

```
  // 阶段一：锁内弹出
  q = get_lock(qid)
  q->qlock.lock()
  blocked_tasks_clamps_t clamp{};
  q->pop_all(&clamp);                 // 或 pop_head / pop_timeouts
  q->qlock.unlock()                   // 临界区结束

  // 阶段二：出锁后批量 set_ready
  bq_flush_pending(&clamp);
  for each task in clamp.arr:
    task_lock(t)
      t->set_ready()
      t->min_wakeup_stamp = 0
    task_unlock(t)
    insert_ready_task(t)
```

为什么分离？因为 `set_ready` 可能触发调度，但在 qlock 临界区内做这件事会牵连锁纪律。pop 出来塞进栈上 batch 极快，真正的重活（`set_ready` + 插入就绪队列）在锁外完成。

### 4.5 锁序违规示例

```
❌ sched_lock → qlock             // 必须先拿 qlock
❌ qlock      → container_lock    // 必须先拿 container_lock
❌ sched_lock → task_lock         // 必须先拿 task_lock
❌ task_lock  → qlock             // 必须先拿 qlock
❌ 两个不同的 task_lock 无固定顺序获得  // 可能导致 ABBA
```

---

## 5. 核心原语

### 5.1 block_if_equal（条件阻塞）

```cpp
void block_if_equal(
    bq_id_t             qid,
    uint64_t*           checker,     // nullptr 等价无条件阻塞
    uint64_t            expected,    // *checker == expected 时阻塞；checker=NULL 时忽略
    task::event_type_t  wait_type,   // 记入 accumulates_bank 的桶 (wait_io/wait_other/wait_mutex)
    uint64_t            timeout_us   // 0 = 不限时，实际限制 ≤ MAX_BLOCK_TIME
);
```

- 强制走 qid，**不支持裸 `block_queue*` 参数**（确保所有 BQ 都在容器中可枚举，支持集中超时扫描）
- 切出前自动 `task_event_shift(wait_type)` + `set_blocked()`
- `min_wakeup_stamp = now + min(timeout_us, MAX_BLOCK_TIME)`（timeout_us≠0时）
- task 只入 BQ inner_queue，**不**入 sleep_queue
- 醒来后调用方自行 `task_event_shift(run_kthread)` + 重检 checker
- 超时唤醒走 `pop_timeouts` + `bq_flush_pending` 路径（见 §7）

### 5.2 kthread_sleep（定时睡眠）

```cpp
void kthread_sleep(miusecond_time_stamp_t offset_us);
```

- 保留，不依赖 BQ，靠 sleep_queue 自唤醒
- 切出前自动 `task_event_shift(sleep)`
- 走原来的 sleep_queue 排序插入 + sleep_tasks_wake 扫描路径

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

## 7. 超时唤醒机制

### 7.1 MAX_BLOCK_TIME 全局兜底

任何 `block_if_equal` 的 `timeout_us` 实际生效值为：

```
effective_timeout = min(timeout_us, MAX_BLOCK_TIME)
```

`MAX_BLOCK_TIME = 5000 ms`（全局常量）。防止调用方设置无限制的超时导致系统死锁。

### 7.2 分散式 BQ 超时扫描

`block_if_equal` 超时任务不进入 sleep_queue。超时唤醒由各 CPU 在 `sched()` 调用前通过**随机轮转临幸法**执行。

```
每个 CPU 每次 sched() 调用前：
  1. 取当前微秒时间戳（调用方已测得，不额外 TSC/MMIO 读）
  2. 微秒戳 & 掩码 → 随机数，映射到全局 BQ 索引
  3. container_lock 读锁
  4. 从 container 中取随机 entry → block_queue*
  5. container_lock 读解锁
  6. 如果 BQ 非空：
       q->qlock.lock()
       blocked_tasks_clamps_t clamp{};
       q->pop_timeouts(&clamp)                // 弹超时 → batch
       q->qlock.unlock()
       bq_flush_pending(&clamp)               // 出锁后统一 set_ready
```

**FIFO 超时扫描的 O(n)：** block_queue 不按 `min_wakeup_stamp` 排序（FIFO 先等先服务纪律）。`pop_timeouts` 从 head 扫到第一个非超时 task，连续弹出超时者。

O(n) 复杂度在以下场景可接受：
- 大多数 BQ 只有 0-1 waiter（per-CID NVMe）
- 共享 BQ（如 i8042）通常是 1 waiter，最多几十个
- 5000ms MAX_BLOCK_TIME 提供充裕宽容度

### 7.3 随机熵源

使用调用 `bq_timeout_scan_rand` 的路径中**已经测量过的微秒时间戳**，不额外触发 TSC 读或 MMIO 读。熵量足够在 2^n 个 BQ 中做均匀分布。

```
uint32_t entropy = (uint32_t)(micro_stamp ^ (micro_stamp >> 16));
uint32_t bq_idx = entropy & (bq_count - 1);    // bq_count 需为 2 的幂
```

### 7.4 sched() 前的完整扫描顺序

```
sched() 调用前：
  1. bq_timeout_scan_rand()       // 随机选一个 BQ 扫超时
  2. sleep_tasks_wake()           // 扫 sleep_queue（kthread_sleep 专用）
  // 两个步骤顺序可互换，任务互不重叠：
  //   sleep_queue 上只有 kthread_sleep 放入的 task
  //   BQ 上只有 block_if_equal 放入的 task（含超时）
```

**重叠规避：** `kthread_sleep` 往 sleep_queue 放，`block_if_equal` 往 BQ inner_queue 放。两条队列互不交叉，同一个 task 不可能同时在两条队列上。

### 7.5 精确性说明

由于随机轮转的分散特性，一个 BQ 的实际超时扫描间隔是不确定的（依赖其他 CPU 的 sched 频率）。但这不影响正确性——超时只是一个宽容的兜底边界，不是精确的时间承诺。需要精确时序的场景应该使用 `kthread_sleep`（走 sleep_queue + timer 中断扫描）。

---

## 8. get_lock + pop/flush 模式的使用示例

### NVMe 中断处理

```cpp
void cq_interrupt_handler(uint16_t cq_id) {
    cq_complex& cq = cqs[cq_id];

    block_queue* q = get_lock(cq.cq_bq_id);   // 查容器拿 BQ 指针
    if (!q) return;

    q->qlock.lock();
    // ── 读 CQ ring（MMIO）──
    // ── 对每个完成的 CID：设置 block_token ──
    // ── 更新 head，写 doorbell ──
    blocked_tasks_clamps_t clamp{};
    q->pop_all(&clamp);                        // 弹全部 → batch
    q->qlock.unlock();

    bq_flush_pending(&clamp);                  // 出锁后统一 set_ready
}
```

### block_if_equal 内部（自动锁管理）

```cpp
// block_if_equal_cppenter 自动处理：
//   get_lock(qid) → qlock.lock() → task_lock(self) → set_blocked
//   → push_back → qlock.unlock → sched
// 调用方无需手动拿锁
```
