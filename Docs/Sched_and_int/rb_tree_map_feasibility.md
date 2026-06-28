# RB Tree Map 可行性分析

> 2026-06-28 — 基于 Ktemplats 红黑树再造 map 轮子的评估

---

## 一、动机

两个痛点指向同一个解决方案：

**痛点 1：`task_pool` 的两级表架构**
```
root_table[65536][sub_table[65536]]
→ 512KB 固定常驻（64bit ptr × 65536）
→ 位图扫描找空闲槽 O(N)
→ 子表 65536 槽只能整体创建/释放（粒度粗）
→ 无法动态收缩
```

**痛点 2：`wq 句柄表` 的定长数组**
```
g_wq_table[256]
→ 256 硬上限
→ 用不用都占着 slot
→ 句柄本质是数组索引 → 复用靠 in_use 标记
```

**共同解：** 一个基于红黑树的通用 K/V map，让 key→value 映射动态伸缩。

---

## 二、RB Tree Map 接口设想

```cpp
// 位于 Ktemplats 命名空间
template<typename K, typename V>
class rb_map {
    // ── 核心 ──
    bool    insert(K key, V value);   // 返回 false = key 已存在
    V*      find(K key);              // 返回 nullptr = 不存在
    bool    remove(K key);

    // ── 迭代 ──
    V*      first();                  // 最小 key
    V*      next(V* current);         // 中序后继
    V*      last();                   // 最大 key
    uint64_t size() const;

    // ── 特殊 ──
    K       min_key();                // 当前最小 key（用于 tid 分配策略）
};
```

---

## 三、替换场景分析

### 3.1 `qid → tid_wait_queue*`（wq 句柄表）

```
当前方案：    g_wq_table[256] 定长数组
RB 方案：     rb_map<wq_id_t, tid_wait_queue*> wq_table

wq_alloc():
  wq_id_t qid = next_qid++;    // 简单递增
  tid_wait_queue* wq = new tid_wait_queue();
  wq_table.insert(qid, wq);
  return qid;

wq_free(qid):
  tid_wait_queue* wq = *wq_table.find(qid);
  delete wq;
  wq_table.remove(qid);

wq_wake_one(qid):
  tid_wait_queue* wq = *wq_table.find(qid);
  // ...
```

**可行性：✅**
- wq 实例数预期很小（每个 task 的 exit_wq + 几个业务 wq）
- log N 开销 ≈ log 256 ≈ 8 cmp（实际可能更少）
- 无限量，不再担心 WQ_TABLE_SIZE 用完
- qid 递增分配，不会重用冲突（除非 2^32 个分配后才回绕 — 不可能）

### 3.2 `tid → task*`（替换 task_pool）

```
当前方案：    两级数组 + 位图扫描 + slot version
RB 方案：     rb_map<uint64_t, task*> tid_map

alloc_tid():
  // 方案 A：简单递增，插入映射
  tid = next_tid++;
  tid_map.insert(tid, task_ptr);
  return tid;
  
  // 方案 B：重用已释放的 tid
  // 拿一个释放队列中的 tid
  
get_by_tid(tid):
  task** pp = tid_map.find(tid);
  return pp ? *pp : nullptr;

release_tid(tid):
  tid_map.remove(tid);
```

**可行性评估：**

| 对比项 | task_pool（当前） | RB tree map |
|--------|:------------:|:--------:|
| 查表 | 2 次数组访问（O1） | O(log N) 约 10 cmp |
| 分配 | 位图扫描 O(N) 平均半表 | 递增 O(1) |
| 释放 | 位图清除 + version bump | remove O(log N) |
| 固定成本 | 512KB 根表常驻 | 0 |
| 每 task 成本 | sub_table slot（64K 粒度假设） | RB node 约 32B |
| 安全性 | slot version 防 UAF | 无 version（find 返回 nullptr） |
| 用户态传递 | tid（uint64_t） | tid（uint64_t）不变 |

**key insight：** 当前 `task_pool` 的 `get_by_tid` 是 O(1) 数组访问，RB tree 会是 O(log N)。这个函数在调度器热路径上（`wakeup_thread`、`kthread_wait` 等每步都调）。如果系统有 1000 个 task，log(1000) ≈ 10 次比较 vs 2 次数组访问——数组快但 RB tree 也不是慢得不可接受。

**但真正的收益在别处：** `alloc_tid` 从位图扫描 O(N) 变成递增分配 O(1)。位图扫描在 task 频繁创建/销毁时产生不可忽略的开销。

**另一个收益：** 去掉 `slot_version` 机制。当前 slot version 是为了检测"拿到一个 tid 但对应的 task 已经被释放"的 UAF。用 RB tree，find 返回 nullptr 或有效指针，不存在"过期 slot"的问题。

### 3.3 其他潜在用途

| 映射 | 说明 |
|------|------|
| `fd → file_struct*` | 未来 VFS，每个进程的文件描述符表 |
| `pid → process_group*` | 未来进程模型 |
| `irq_num → handler*` | 中断路由 |
| `addr → vm_area*` | 用户地址空间 VMA 管理（替代链表） |

---

## 四、风险与争议

### 4.1 RB tree 在中断上下文中可用吗？
- `find` 只读，无分配 → 中断安全 ✅
- `insert` / `remove` 需要分配/释放 → 不能在硬中断中调用 ⚠️
- `task_pool` 的 bitmap 操作也需要锁，同样不能在硬中断中分配 → 现状一致

### 4.2 内存开销
- 每个 `rb_map` 节点：`left/right/parent/color (28B)` + `key (8B)` + `value (8B)` = ~44B
- 1000 task: 44KB
- 10000 task: 440KB
- 对比 task_pool 的 512KB 根表常驻：1000 以下 RB 更省，10000 以上 RB 更费
- wq 额外 ~44B 每个

### 4.3 锁模型
- task_pool 用 `spinrwlock_cpp_t`（读写锁，读路径可以并发）
- RB tree `find` 是纯读取，可以用 RW 锁或 RCU
- RB tree `insert`/`remove` 需要写锁

### 4.4 递增 tid 的安全顾虑
- 当前 tid = `(idx << 32) | slot_version`
- slot_version 防重用：同一个 idx 被分配、释放、再分配时 version 递增，旧 tid 不再指向新 task
- 递增分配：`next_tid++` 天然唯一，不回绕（64bit 够用 2^64次分配）
- 但如果 task 释放了，持有旧 tid 的 waiter 调 `get_by_tid(old_tid)` → find 返回 nullptr → 正确处理（`kthread_wait` 见到 nullptr 时通常是"target 已经死了"）

---

## 五、推荐路线

```
Phase 1 — Ktemplats 基础
  在 Ktemplats 中实现 rb_map<K, V>，测试通过
  基于已有的红黑树（list_doubly 旁边找找已有实现）

Phase 2 — wq 句柄化
  wq_alloc/wq_free → 基于 rb_map<wq_id_t, tid_wait_queue*>
  砍掉 g_wq_table[256] 定长方案
  block_if_equal 参数改为 wq_id_t

Phase 3 — task_pool 替换（可选）
  rb_map<uint64_t, task*> 替代两级数组
  砍掉 task_pool::bitmap / slot_version / subtable
  alloc_tid 变为 next_tid++ 递增
  注意：这是个大手术，不急于 Phase 3
```

---

## 六、结论

| 场景 | 可行性 | 收益 | 风险 |
|------|:---:|:---:|:---:|
| wq 句柄化 | ✅ 高 | 消除 256 上限、防伪、用户态可复用 | 低 |
| task_pool 替换 | ✅ 中 | 砍 512KB 常驻、O(N)位图→O(1)分配；但 get_by_tid 变慢 | 中（调度器热路径需实测） |
| 通用 map 轮子 | ✅ 高 | VFS、IPC、进程模型可复用 | 低 |

**建议：Phase 1 + 2 直接做。Phase 3（task_pool）不急于落地，先让 rb_map 跑通再评估性能影响。**
