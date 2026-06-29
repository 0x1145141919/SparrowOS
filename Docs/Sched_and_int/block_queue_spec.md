# block_queue spec（契约，非日志）

> 最终版 — 2026-06-29
> 所有决策已在 `git log` 中记录，此文件只写最终结论。

---

## 1. bq_id_t 句柄系统

Block queue 通过全局容器（rb_map 或固定表）管理，不暴露裸指针。
句柄代替指针：防伪、防 UAF、可跨进程传递（用户态可通过 syscall 使用相同句柄）。

```cpp
typedef uint64_t bq_id_t;
constexpr bq_id_t  BQ_ID_INVALID = ~0u;

bq_id_t  bq_alloc();                         // 分配一个新 block_queue，返回句柄，state=ready
uint64_t bq_free(bq_id_t qid);               // 释放（KURD raw，失败若 qid 不存在 / state != ready）
spinlock_cpp_t* get_lock(bq_id_t id);        // 拿锁指针，没找到返回 NULL

// 以下操作要求调用方已通过 get_lock 拿到的锁，且在锁的临界区内调用：
uint64_t enable_queue(bq_id_t id, task::event_type_t type);  // ready+empty → running，返回 KURD raw
uint64_t disable_queue(bq_id_t id);          // empty → ready，返回 KURD raw

uint64_t bq_wake_push(bq_id_t qid);          // 唤醒 head 阻塞者，返回唤醒个数
uint64_t bq_wake_all(bq_id_t qid);           // 唤醒全部，返回唤醒个数
uint64_t bq_wake_timeouts(bq_id_t qid);      // 从 head 向后遍历，唤醒超时者，返回唤醒个数
```

**设计理由：**
- 句柄防伪、防 UAF，可跨进程传递
- `get_lock` 模式将锁的生命周期管理交给调用方——中断 handler 一次拿锁，多次操作（设置 token + wake），共享锁开销
- `enable_queue` / `disable_queue` 显式生命周期控制：bq_alloc 预分配句柄，使用前 enable，用完 disable 归还

### 灵活性（对比旧 `kthread_wait(tid)` 的硬耦合）

旧 API 将等待关系绑定到线程 TID 上，只能等特定线程退出。
句柄系统解耦了"等待关系"和"父子关系"：

- 父线程创建一个 BQ，以 `wait_other` enable
- 父线程调用 `block_if_equal(bq_id, &running_word, word_running, wait_other, timeout_us)`
- 子线程退出时：原子置 `running_word` 为非 `word_running` → `bq_wake_all(bq_id)`
- 父线程醒来后，该 BQ 句柄可复用于其他线程或其他目的

---

## 2. block_queue 数据结构

组合方式，不继承：

```cpp
class block_queue {
    enum state_t { ready, running } state;  // ready=可复用, running=已分配
    spinlock_cpp_t                qlock;          // 只保护 inner_queue 头尾原子出入
    task::event_type_t            queue_event;    // 该 BQ 的等待语义（wait_io / wait_other / wait_mutex）
    Ktemplats::list_doubly<task*> inner_queue;    // FIFO：push_back 尾注入，pop_front 头吐出
};
```

**状态机：** `ready ↔ running`。
- `bq_alloc` → `ready`（句柄可用但未激活）
- `enable_queue` → `ready`→`running`（只有 ready+empty 才能成功）
- `disable_queue` → `running`→`ready`（只有 empty 才能成功）
- `bq_free` → 释放句柄（只能 free ready 态）

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
bq_lock  >  task_lock  >  sched_lock
(最高)                    (最低)
```

- 允许拿高层锁的临界区内再拿低层锁（嵌套）
- 不允许先拿低层锁再拿高层锁（断链）
- 不相关的两把同级锁（如两个不同 task 的 `task_lock`）按固定顺序：参考 `kthread_exit_cppenter` 的 `exit_task_lock > waiter_task_lock`（先锁被引用对象，再锁引用者）

注意：bq_lock 由 `get_lock()` 返回的 `spinlock_cpp_t*` 标识。`block_if_equal` 入队时自动拿 bq_lock → task_lock。调用方也可通过 `get_lock` 手动拿锁后调用 `bq_wake_*` 系列函数（这些函数假设锁已持有，不自拿锁）。

### 4.2 各调度路径对照

| 路径 | 锁顺序 | 嵌套？ | 位于 |
|---|---|---|---|
| block_if_equal 入队 | `bq_lock > task_lock` | ✅ 嵌套 | `block_if_equal_cppenter` |
| bq_wake_push / bq_wake_all / bq_wake_timeouts | 调用方已持 `bq_lock` → 函数内部不拿锁 | ❌ 调用方负责 | 调用者上下文 |
| kthread_exit | `exit_task_lock > waiter_task_lock > waiter_sched_lock` | ✅ 三层嵌套 | `kthread_exit_cppenter` |
| kthread_wait | `waited_task_lock > waiter_task_lock` | ✅ 嵌套 | `kthread_wait_cppenter` |
| kthread_sleep | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `kthread_sleep_cppenter` |
| yield / resched | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `kthread_yield_true_enter` / `resched` |
| create_kthread | `task_lock` → (放出) → `sched_lock` | ❌ 分离 | `create_kthread` |
| wakeup_thread | `task_lock > sched_lock` | ✅ 嵌套 | `wakeup_thread` |

`→` 表示同一临界区内拿取（嵌套），`>` 表示拿取顺序，`──→(放出)──>` 表示释放锁后在其他临界区拿更低层级锁。

### 4.3 入队流程（block_if_equal 内部）

```
  1. bq_lock(qid)                          // 锁 checker 判别
  2.   if checker != NULL && *checker != expected:
  3.     bq_unlock(qid)                     // 条件已不成立，不入队
  4.     return
  5.   task_event_shift(wait_type)         // 关旧帐、开新帐
  6.   task_lock(self)                      // ──── 嵌套在 bq_lock 下 ────
  7.     self->set_blocked()
  8.     self->blocked_reason = ...
  9.     self->min_wakeup_stamp = timeout_us ? now + timeout_us : 0
  10.  task_unlock(self)
  11.  inner_queue.push_back(self)          // FIFO 尾注入
  12. bq_unlock(qid)
  13. bq_timeout_scan_rand()                // 随机选一个 BQ 扫超时
  14. sleep_tasks_wake()                    // 扫 sleep_queue
  15. sched()                               // 内部拿 sched_lock
```

### 4.4 唤醒流程（bq_wake_push / bq_wake_all — 调用方已持锁）

```
  // 调用方已：get_lock(qid)→lock()
  1.  if checker_present:
  2.    *checker = wake_val                 // 调用方自行修改 checker
  3.  t = inner_queue.pop_front()           // bq_wake_push: 只 pop head
                                            // bq_wake_all: while(非空)pop_front
  4.  // 在 bq_lock 下 pop 结束
  // 调用方 unlock()
  // 调用方逐 task:
  5.  task_lock(t)
  6.    t->set_ready()
  7.    t->min_wakeup_stamp = 0
  8.  task_unlock(t)
  9.  insert_ready_task(t)
```

为什么唤醒在锁外做 pop？因为 bq_lock 只保护 inner_queue 的出入，不保护 task 状态。
pop 出来的 task* 在 bq_lock 释放后也不会被其他人修改（只有本 CPU 持有此引用）。
分开做避免在中断上下文中久持 bq_lock。

### 4.5 锁序违规示例

```
❌ sched_lock → bq_lock     // 必须先拿 bq_lock
❌ sched_lock → task_lock   // 必须先拿 task_lock
❌ task_lock  → bq_lock     // 必须先拿 bq_lock
❌ 两个不同的 task_lock 无固定顺序获得               // 可能导致 ABBA
```

---

## 5. 核心原语

### 5.1 block_if_equal（条件阻塞）

```cpp
void block_if_equal(
    bq_id_t          qid,
    uint64_t*        checker,     // nullptr 等价无条件阻塞
    uint64_t         expected,    // *checker == expected 时阻塞；checker=NULL 时忽略
    task::event_type_t  wait_type,   // 记入 accumulates_bank 的桶 (wait_io/wait_other/wait_mutex)
    uint64_t         timeout_us   // 0 = 不限时，实际限制 ≤ MAX_BLOCK_TIME
);
```

- 切出前自动 `task_event_shift(wait_type)` + `set_blocked()`
- `min_wakeup_stamp = now + min(timeout_us, MAX_BLOCK_TIME)`（timeout_us≠0时）
- task 只入 BQ inner_queue，**不**入 sleep_queue
- 醒来后调用方自行 `task_event_shift(run_kthread)` + 重检 checker
- 超时唤醒走 `bq_wake_timeouts` 路径（见 §7），醒来后必须先重检 checker

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
  3. lock = get_lock(global_bqs[rand_idx])
  4. 如果 lock 非空：
       lock->lock()
       bq_wake_timeouts(global_bqs[rand_idx])   // 从 head 向后遍历
       lock->unlock()
  5. 分离处理每个超时的 task（同 §4.4 唤醒流程）
```

**FIFO 超时扫描的 O(n)：** block_queue 不按 `min_wakeup_stamp` 排序（FIFO 先等先服务纪律）。`bq_wake_timeouts` 从 head 扫到尾，对 `min_wakeup_stamp != 0 && now > min_wakeup_stamp` 的 task pop + set_ready。

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

## 8. get_lock + wake 模式的使用示例

### NVMe 中断处理（改进后）

```cpp
void cq_interrupt_handler(uint16_t qid) {
    cq_complex& cq = cqs[qid];

    lock = get_lock(cq.cq_bq_id);         // 拿 BQ 锁指针
    if (!lock) return;

    lock->lock();                          // 手动锁
    // ── 读 CQ ring（MMIO）──
    // ── 对每个完成的 CID：设置 block_token ──
    // ── 更新 head，写 doorbell ──
    bq_wake_push(cq.cq_bq_id);            // 唤醒一个等待者（已在锁下）
    // 或者 bq_wake_all(cq.cq_bq_id);     // 唤醒全部
    lock->unlock();
}
```

### 提交路径（block_if_equal 内部）

```cpp
// block_if_equal_cppenter 自动处理：
//   bq_lock(qid) → task_lock(self) → set_blocked → push_back → bq_unlock
// 调用方无需手动拿锁
```
