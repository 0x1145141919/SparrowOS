# create_kthread 创建/就绪解耦与 Worker Pool 草案

> 衍生自 Tomcat 线程池"提前分配、动态派活"的思想对比
> 2026-06-17

---

## 一、现状问题

### 1.1 create_kthread 的单一语义

当前 `create_kthread(entry, arg)` 是**创建即开工**：

```
alloc_context()
alloc_stack()
alloc_task()
set_ready()               ← 焊死在创建流程里
insert_ready_queue()      ← 立即可被调度器选中运行
```

这在一个调用里绑定了两种语义：
- **资源分配**：context + stack + task
- **调度就绪**：丢进 ready_queue，调度器随时选走

### 1.2 缺点

- **无法预创建线程池**：想提前造一批 worker 等活，没地方存
- **无法替换入口函数**：`entry` 在创建时就写死了，worker 跑完旧任务就退出
- **高频任务场景开销大**：短生命周期任务每次都要 alloc + free，cache 污染 + 分配压力
- **block_reason 缺少上游**：`no_job` 已经在枚举里，但没有机制让线程以 `no_job` 状态等待

---

## 二、改造目标

```
分离：创建（alloc）≠ 就绪（ready）
新增：parked — 创建完毕但不动，等唤醒
新增：worker_pool — 预分配的 kthread 池子
复用：blocked_reason = no_job（枚举已存在）
```

### 2.1 新增的 task 状态

```
task_state_t:
  ready     ← 就绪（现有）
  running   ← 运行（现有）
  blocked   ← 阻塞（现有）+ blocked_reason 子类型
  parked    ← 新：预创建但空闲，不在任何调度队列里

或复用 blocked(no_job) + 额外 parked 位：
  blocked
    ├── sleeping
    ├── mutex
    ├── wait_other_kthread
    ├── no_job     ← worker 空闲（仅在 worker_pool 里）
    └── ...
```

**推荐复用 blocked(no_job)**：`set_ready()` → `set_blocked(no_job)`，由 `worker_pool` 持有引用而非 `ready_queue` 持有。减少状态枚举膨胀。

### 2.2 核心接口变更

```cpp
// 1. 改动 create_kthread：增加 park 变体
uint64_t create_kthread(void *(*entry)(void *), void *arg, KURD_t *out_kurd);
  → 保持原语义不变（兼容旧代码）

uint64_t create_kthread_parked(KURD_t *out_kurd);
  → 新：分配资源但不设 entry/arg，不 set_ready
     task 状态 = blocked(no_job)
     栈保留，TCB 保留

// 2. 唤醒时注入入口
uint64_t wakeup_with_entry(uint64_t tid, void *(*entry)(void *), void *arg, bool front);
  → 类似现有 wakeup_thread()，但额外设置 entry/arg
  → 若 tid 对应的 task 不是 blocked(no_job)，返回错误

// 3. 线程自己跑完后回到池子
[[noreturn]] void kthread_return_to_pool();
  → 类似 exit，但不销毁资源
  → 设置 blocked(no_job)
  → 回到 worker_pool，等待下次 wakeup_with_entry
```

### 2.3 数据流改造

```
当前 create_kthread + wakeup：

  create_kthread(fn, arg)
    → alloc()
    → set_ready()
    → insert_ready_queue()
    → 调度器选上 → 跑 fn(arg) → exit → 销毁

改造后 Worker Pool 模式：

  boot 阶段：
    for i in 0..N:
      create_kthread_parked()
        → alloc()
        → set_blocked(no_job)
        → worker_pool.push_back(task)  // 不经过 ready_queue

  runtime：来活了
    submit_to_pool(fn, arg)
      → 从 worker_pool 弹出一个空闲 worker
      → wakeup_with_entry(worker->tid, fn, arg)
      → 调度器选上 → 跑 fn(arg) → kthread_return_to_pool()
      → set_blocked(no_job) → worker_pool.push_back(self)
      → 等下一趟
```

---

## 三、Worker Pool 数据结构

### 3.1 定义

```cpp
// per-CPU worker pool（或全局单例，取决于设计）
struct kthread_worker_pool {
    // 空闲 worker 链表—都是 blocked(no_job) 状态
    // 不用标准队列，加减频繁，用 intrusive list
    intrusive_list_node<task> idle_list;

    // 总 worker 数（固定，boot 时定）
    uint32_t total_workers;

    // 当前忙的 worker 数
    uint32_t busy_count;

    // 待提交的任务队列（池子满时排队）
    struct pending_job {
        void *(*entry)(void *);
        void *arg;
        intrusive_list_node<pending_job> link;
    };
    intrusive_list<pending_job> pending_queue;
};
```

### 3.2 接口

```cpp
// boot 时：创建 N 个 park 好的线程
void worker_pool_init(uint32_t count);

// 提交任务：有空闲 worker 直接唤醒，否则入 pending_queue
uint64_t worker_pool_submit(void *(*entry)(void *), void *arg);

// worker 内部：跑完后自动回池
[[noreturn]] void worker_return_to_pool();

// 提交扇出：一对多
uint64_t worker_pool_broadcast(void *(*entry)(void *), void *arg);
```

### 3.3 配额控制

每个 CPU 一个 pool，数量 = 核数 × 配额因子：

```
desk_laptop 配置示例（4P+4E）：
    P-core: 每个核 2 个 worker（8 个）
    E-core: 每个核 1 个 worker（4 个）
    total: 12

server 配置示例（锅大）：
    每个核 4 个 worker
```

通过 kernel cmdline 或 kshell 调节 `worker_pool_quota`。

---

## 四、kthread_return_to_pool 实现要点

### 4.1 和 exit 的异同

| | kthread_exit | kthread_return_to_pool |
|---|---|---|
| 栈 | 释放 | 保留 |
| context | 释放 | 保留 |
| task 结构 | 销毁 | 设 blocked(no_job) |
| worker_pool | 不涉及 | 把自己加回 idle_list |
| 谁调用 | 任何 kthread | 仅 pool 里的 worker |

### 4.2 栈复用约束

worker 必须是 `kthread_return_to_pool()` 前把栈**完全清理**到刚 alloc 完的状态。需要保证：

```
run fn(arg)
  → fn 返回 → kthread_return_to_pool()
    → 自身状态重置：
        - 寄存器恢复为初始值（从 context 里快照）
        - blocked_reason = no_job
        - task_lock 释放
        - 排入 idle_list
    → 线程进入 blocked(no_job) 状态
    → sched() 切走
```

**栈不清零**——上次调用残留的数据在栈帧之上，下趟分发时 fn 从头开始跑，栈指针在最高位，旧数据自然覆盖。不清零省了 memset 的开销。

### 4.3 wakeup_with_entry 的 entry 注入时机

```
wakeup_with_entry(tid, fn, arg)
  → 锁住目标 task
  → 检查 state == blocked && blocked_reason == no_job
  → 改写 context->regs.rdi = fn    // 第一个参数
  → 改写 context->regs.rsi = arg   // 第二个参数
  → 改写 context->regs.rip = allthread_true_enter  // 入口
  → set_ready()
  → insert_ready_queue()
  → 解锁
```

worker 被唤醒后从 `allthread_true_enter` 开始执行，就像第一次创建时一样：

```cpp
extern "C" void allthread_true_enter(void *(*entry)(void *), void *arg) {
    entry(arg);                       // 跑业务
    kthread_return_to_pool();         // 回池
}
```

---

## 五、和现有机制的兼容

### 5.1 不破坏现有接口

`create_kthread` 原语义**不动**—只加新接口，不删不改旧的。已用 `create_kthread` 的地方（定时器线程、load_balancer 等启动阶段创建的特殊线程）继续用旧的。

### 5.2 和 DTS 调度器的关系

| 组件 | Worker Pool | DTS |
|------|------------|-----|
| 关注点 | 线程创建/销毁的复用 | 时间片/负载均衡/抢占 |
| 关联 | worker 被唤醒后进入 ready_queue，由 DTS 正常调度 | worker 被调度时和其他任务共享同一套时间片规则 |
| 互斥 | 无 | 无 |

Worker Pool 是**调度器之上的资源管理层**，不干预调度决策。

### 5.3 和 block_queue / wakeup 的关系

`block_queue_cppenter` 和 `wakeup_thread` 的现有实现保持不动。`no_job` 加入后，`wakeup_thread` 只是新增一条 `blocked_reason == no_job` 的检查路径：

```cpp
// wakeup_thread 现有逻辑中，blocked 分支新增：
if (task_ptr->get_state() == task_state_t::blocked) {
    // 原有睡眠/mutex 唤醒逻辑...
    // 新增：
    if (task_ptr->blocked_reason == no_job) {
        // 从 worker_pool 移回 ready_queue
        // 不重置栈/寄存器（caller 负责注入 entry）
    }
}
```

---

## 六、脑图与全景

```
                      boot 阶段                           运行时
                ┌─────────────────┐              ┌──────────────────────┐
                │ create_kthread   │              │                      │
                │ _parked() × N   │              │  worker_pool_submit  │
                │                 │              │  (fn, arg)           │
                │  alloc_stack    │              │     │                │
                │  alloc_context  │              │     ▼                │
                │  alloc_task     │              │  有空闲?             │
                │  blocked(no_job)│              │  ┌─┴─┐              │
                │                 │              │  是  否              │
                │  → idle_list    │              │  │   │              │
                └───────┬─────────┘              │  │   ▼              │
                        │                        │  │  pending_queue   │
                        │                        │  │                  │
                        ▼                        │  ▼                  │
                ┌─────────────────┐              │ wakeup_with_entry  │
                │  idle worker    │◄─────────────│ (tid, fn, arg)     │
                │  blocked(no_job)│              └──────┬──────────────┘
                └─────────────────┘                     │
                        ▲                              │
                        │                              ▼
                ┌───────┴─────────┐          ┌─────────────────────┐
                │  worker return  │          │  ready_queue → DTS  │
                │  _to_pool()     │          │  调度到某个核       │
                │                 │          │                     │
                │  blocked(no_job)│          │  fn(arg) 执行       │
                │  → idle_list    │          │                     │
                └─────────────────┘          │  fn 返回            │
                                             │                     │
                                             │  return_to_pool()   │
                                             └─────────┬───────────┘
                                                       │
                                                       ▼
                                                (回到原地，循环)
```

---

## 七、实现路线图

### Phase 1：创建/就绪解耦
- [ ] `create_kthread_parked()` 接口
- [ ] `wakeup_with_entry()` 接口
- [ ] `task_state_t::blocked(no_job)` 的 wakeup 路径

### Phase 2：Worker Pool 框架
- [ ] `kthread_worker_pool` 数据结构（per-CPU）
- [ ] `worker_pool_init()` boot 时调用
- [ ] `worker_pool_submit()` 分派逻辑
- [ ] `worker_return_to_pool()` worker 回池

### Phase 3：整合
- [ ] 替换高频短生命周期 kthread 创建为 pool 提交
- [ ] pending_queue 满时退化为同步执行或阻塞等待
- [ ] kshell 命令 `pool status` / `pool quota N`

### Phase 4（可选）：动态扩缩
- [ ] 监控 idle_list 深度，低于阈值自动补充 worker
- [ ] 长时间空闲 worker 可回收（定时 sweep empty workers）
- [ ] NUMA 感知分配（优先从本地内存取 worker）
