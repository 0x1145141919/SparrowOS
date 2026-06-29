# 2026-06-30 — Bqs 容器设计讨论历史

> 本文档记录 Bqs（Block Queue System）容器从 v1 到最终方案的设计博弈。
> 对应 `block_queue_spec.md` 的落地实现讨论。
>
> 参与角色：
>   AI = 编码方（我）
>   设计方 = 你（项目方）

---

## 一、v1 — 直接抄 spec 的裸 rb_map + 静态池

**设计方要求：** 使用 `rb_map` 实现 Bqs 容器，基于 `per_processor_scheduler.h` 中的 BQS 接口声明。

**AI 出方案：**

```
键值对：rb_map<bq_id_t, bq_slot>   // 值语义
bq_slot = { block_queue, pending_list, allocated_flag }
bq_lock = spinlock_cpp_t (互斥锁)
```

值语义直接存放 `bq_slot`，用固定静态池 `bq_slot pool[256]` + 空闲链管理 slot。

**暴露问题：**
1. 静态池限制了 BQ 数量（256），与 rb_map 的"动态无限"设计意图矛盾
2. `bq_lock` 用普通 spinlock，`get_lock`（查找路径）在多核下串行化

---

## 二、v2 — RW 锁升级 + 静态池辩护

**设计方提示：** `lock.h` 有 `spinrwlock_cpp_t`，问能否改进。

**AI 改进：**

```
bq_lock = spinrwlock_cpp_t  // RW 锁
  ├─ 读锁：get_lock / find 路径     → 多核并行
  └─ 写锁：bq_alloc / bq_free       → 独占

map 改用指针：rb_map<bq_id_t, bq_slot*>
  → 避免值语义下 remove 立即析构 bq_slot 导致 UAF
  → 静态池保证指针永久有效

bq_slot 增加 pending 链表
  → wake_* 只 pop + 追加 pending
  → flush_pending 在 qlock 外处理 task_lock/set_ready
  → 缩小 qlock 临界区 ✓
```

**设计方指出问题：**

> "既然用了 rb_map 肯定是希望无限增多的，你的静态分配反而引入了上限，不好。"

**决议：** 静态池退役。`rb_map<bq_id_t, block_queue*>`，`bq_alloc` 时 `new block_queue`，`bq_free` 时 `delete`。

---

## 三、v3 — 动态分配 + wake 语义修正

**设计方进一步指出三个 wake 函数的临界区问题：**

> "uint64_t bq_wake_push(bq_id_t qid); uint64_t bq_wake_all(bq_id_t qid); uint64_t bq_wake_timeouts(bq_id_t qid) 却是希望直接在里面全部唤醒，而这三个函数都在 blockqueuelock 的临界区下，撑大了临界区，应该修改那三个函数的语义是吐出相应的数组。"

**修正：wake 三件套只 pop 不唤醒**

| 原语义（qlock 内做唤醒） | 修正语义（qlock 内只 pop + 吐出） |
|---|---|
| `bq_wake_push`：pop → push pending → task_lock → set_ready | `bq_wake_push`：pop → 返回链表 **← 只剩这个** |
| `bq_wake_all`：pop N → 逐个 task_lock → set_ready | `bq_wake_all`：pop N → 返回链表 |
| `bq_wake_timeouts`：遍历 min_wakeup_stamp → 逐个唤醒 | `bq_wake_timeouts`：遍历 min_wakeup_stamp → pop → 返回链表 |

新增公共唤醒函数 **`flush_tasks(list)`**，在 qlock 外调用：
```
flush_tasks:
  遍历链表，逐个 task_lock → set_ready → min_wakeup_stamp=0 → 
  task_event_shift(run_kthread) → insert_ready_task
↑ 这才是"真正唤醒"的地方，不在 qlock 临界区内
```

---

## 四、最终方案总结

### 数据结构

```
键值对:    rb_map<bq_id_t, block_queue*>
          bq_alloc → new block_queue
          bq_free  → delete block_queue
          ☆ 动态增长，无硬上限

锁:       bq_lock = spinrwlock_cpp_t
          ├─ 读锁: get_lock → 多核并行查找
          └─ 写锁: bq_alloc / bq_free → 互斥修改 map

锁层次:   bq_lock (RW) > qlock (per-BQ spinlock) > task_lock > sched_lock
```

### 接口

| 函数 | 锁 | 说明 |
|---|---|---|
| `Init()` | — | 单例初始化（空 KURD 占位）|
| `bq_alloc()` | bq_lock (写) | new block_queue → map.insert → 返回 bq_id_t |
| `bq_free(qid)` | bq_lock (写) + qlock | find → qlock.lock → map.remove → delete → qlock.unlock |
| `get_lock(qid)` | bq_lock (读) | find → 返回 &block_queue.qlock 或 nullptr |
| `enable_queue(qid, type)` | caller 已持 qlock | state=ready+empty → state=running |
| `disable_queue(qid)` | caller 已持 qlock | empty → state=ready |
| `bq_wake_push(qid)` | caller 已持 qlock | pop_front → 返回 `list_doubly<task*>` |
| `bq_wake_all(qid)` | caller 已持 qlock | pop_all → 返回 `list_doubly<task*>` |
| `bq_wake_timeouts(qid)` | caller 已持 qlock | 遍历超时 pop → 返回 `list_doubly<task*>` |
| `flush_tasks(list&)` | qlock 外 | 逐个 task_lock → set_ready → min_wakeup_stamp=0 → insert_ready_task |

### 典型使用模式

```cpp
// 分配
bq_id_t qid = Bqs::bq_alloc();

// 使用（带锁获取）
spinlock_cpp_t* lk = Bqs::get_lock(qid);
lk->lock();
Bqs::enable_queue(qid, task::wait_io);
// 入队、阻塞等操作...
auto tasks = Bqs::bq_wake_all(qid);     // qlock 内极短：只 pop 链表
lk->unlock();
// qlock 外：处理唤醒（可被中断、耗时）
Bqs::flush_tasks(tasks);

// 释放
Bqs::bq_free(qid);
// 内部：bq_lock(write) → qlock.lock → map.remove → delete → qlock.unlock
```

---

## 五、未解决的问题（下次讨论）

1. **`flush_tasks` 中 `task_event_shift` 的 next_event 参数** — 被 IO 唤醒的 task 应该开 `run_kthread` 还是 `run_uthread`？需要根据 `choose` 字段决定，但 `flush_tasks` 是通用函数，可能需要 caller 传入事件类型或根据 task.state 推断。
2. **启用中断的 guard** — `spinrwlock_interrupt_about_*_guard` 会关中断。`get_lock` 作为高频路径，关中断的开销是否可接受？或者应该提供不加中断保护的版本（须 caller 保证不在中断上下文调用）？
3. **`bq_free` 中的 delete 时机** — 如果 pending 列表在 `bq_free` 时非空，应该 panic 还是默默丢弃？规范上说 `disable_queue` 已校验 empty，但 `bq_free` 的竞态窗口仍需明确约定。

---

*本文档完成于 2026-06-30 00:33 GMT+8，在 `bq_system.cpp` 实际实现前。*
