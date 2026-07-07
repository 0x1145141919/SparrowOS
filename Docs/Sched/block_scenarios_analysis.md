# 阻塞场景需求分析

> 2026-06-28 — 检验 `block_if_equal` 是否能一招鲜吃遍天

---

## 场景A：等别人退出（kthread_wait）

```
A_create B;
A: block_if_equal(B->exit_wq, &B->is_alive, true, wait_other);
B: do_work(); exit(0);
B: wq_wake_all(B->exit_wq, 0);
A: 被唤醒 → 继续
```

**检查器语义**：`*cur_task->is_alive == true` → 阻塞。`is_alive` 在 exit 时设为 false。
`block_if_equal` 的 checker 是等式，刚好。**一次命中，无需重检循环。**

**结论：✅ 覆盖**

---

## 场景B：阻塞互斥锁

```
blocking_mutex_lock(m):
  if CAS(m->locked, 0, 1) == 0: return
  loop:
    block_if_equal(&m->wq, &m->locked, 1, wait_io)
    // 醒来后 *m->locked == 0（unlock 写过），checker 不命中
    if CAS(m->locked, 0, 1) == 0: return

blocking_mutex_unlock(m):
  m->locked = 0
  wq_wake_one(&m->wq, 0)
```

**检查器语义**：`*m->locked == 1` → 阻塞。unlock 写 0 后 waiters 醒来，checker 不命中。
**一次唤醒就刚好抢到锁？不一定——可能有多个 waiter 同时醒来。但 CAS 仲裁即可。**

**结论：✅ 覆盖**

---

## 场景C：等工件（producer-consumer）

```
// 渲染线程
for (;;) {
    block_if_equal(&render_wq, &render_pending, false, wait_io);
    render_pending = false;
    do_render();
}

// 业务线程提交
render_pending = true;
wq_wake_one(&render_wq, 0);
```

**检查器语义**：`render_pending == false` → 阻塞。业务线程设 true 后 wake，checker 不命中。

**结论：✅ 覆盖**

---

## 场景D：等 IO 完成

```
nvme_submit(cmd);
block_if_equal(&nvme_wq, &cmd->done, false, wait_io);
// 中断 handler: cmd->done = true; wq_wake_one(&nvme_wq, 0);
```

**检查器语义**：`cmd->done == false` → 阻塞。done 被中断设为 true 后唤醒。
**和 C、B 是同一个模式。**

**结论：✅ 覆盖**

---

## 场景E：等待资源（>= 阈值）

这是最可能翻车的——**不等式检查**：

```
// 需要 ring buffer 至少有 5 个条目才能批量处理
// ring->count 可能从 3 跳到 7，跳过 5
→ 不能用 == 5 作 checker

// 解法：caller 套循环 + block_if_equal 做无条件阻塞
while (ring_buffer_count(rb) < 5) {
    // checker = &dummy, 0 → 恒假 → 一直阻塞到被 wake
    block_if_equal(&rb->wq, &rb->dummy, 0, wait_io);
}
// 醒来后重判，>= 5 则退出
// 若仍不足 5，下次 push 时再 wake 一次
```

**检查器结论**：等式不够，但 caller 循环 + 无条件 block 覆盖。

实际 `block_if_equal` 接口里还有个隐含方案——**checker 可以是任意函数指针**（但增加了接口复杂度）。更干净的做法：caller 套 `while (cond) block_if_equal(&wq, &dummy, 0)`。dummy 恒 != 0，checker 永远命中，等价无条件阻塞。醒来后业务条件由 while 自己判。

**结论：⚠️ checker 等式不够覆盖，但 caller 循环 + 无条件 block 可兜底**

---

## 场景F：等信号（旧 block_queue / block_if_equal 的原始用途）

```
// 任意自定义条件
uint64_t flag = 0;
// 等待者:
block_if_equal(&wq, &flag, 0, wait_other);
// 通知者:
flag = 1;
wq_wake_one(&wq, 0);
```

**结论：✅ 覆盖（正是 block_if_equal 的设计目标）**

---

## 场景G：主动让出（yield）

```
kthread_yield:
  sched()   // 不阻塞，只是放弃当前时间片
  // 仍然是 running 状态
```

**这不属于阻塞，不需要 block 原语。保持 yield 独立。**

**结论：✅ 不相关**

---

## 汇总

| 场景 | checker 等式足够 | 可否 block_if_equal |
|------|:---:|:---:|
| A. 等 exit | ✅ `alive == true` | ✅ |
| B. 等锁 | ✅ `locked == 1` | ✅ |
| C. 等工件 | ✅ `pending == false` | ✅ |
| D. 等 IO | ✅ `done == false` | ✅ |
| E. 等 >= 阈值 | ❌ 等式不够 | ⚠️ 套 while + 无条件 block |
| F. 等自定义信号 | ✅ `flag == 0` | ✅ |
| G. yield | 不阻塞 | — |

---

## 最终结论

`block_if_equal` + `kthread_sleep` **能覆盖所有阻塞场景**。

唯一的不等式场景（E）用 caller 循环 + 无条件 block 兜底：
```cpp
while (want_condition) {
    // dummy checker 永远命中 → 等价无条件阻塞
    block_if_equal(&wq, &dummy, 1, wait_io);
}
```

这其实就是 pthread 条件变量的 `while(pred) pthread_cond_wait()` 模式，没什么新鲜的。

---

## 对接口的影响

`block_if_equal` 保留等式 checker 作为一等公民——大部分场景（ABCD）一次命中，不用循环。等式不够的套 `while` 即可。**不需要引入函数指针或不等号参数。**
