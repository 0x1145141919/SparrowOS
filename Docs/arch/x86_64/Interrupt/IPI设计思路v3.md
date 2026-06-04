# IPI 设计演进：从函数指针到消息槽

日期: 2026-06-04
最后修订: 2026-06-05 (v3.1)
涉及文件: x86_vecs_deliver_mgr.cpp / GS_complex.h / cmpxchg16b.asm / kinit.cpp

---

## 演进脉络

```
global_ipi_handler (v1)     →    独立向量表 (v2)     →    ipi_local_complex (v3)
   2026-05-25                     2026-05-26                 2026-06-04

  一个全局函数指针          per-vector 固定路由         16B cmpxchg16b 消息槽
  一个向量号 240           三个向量层次                 两个向量 +
  一锤子买卖               soft/token/alloc 分层        per-core 原子 {fn, arg}
```

---

## 第一章：v1 — global_ipi_handler（2026-05-25）

### 机制

```cpp
// lapic.cpp
void (*global_ipi_handler)() = nullptr;

void broadcast_exself_fixed_ipi(void (*handler)()) {
    global_ipi_handler = handler;   // 写函数指针
    sfence;
    raw_send_ipi(broadcast_exself_icr);  // 向量 240
}

// exceptions_handler.cpp（收端）
void ipi_cpp_enter(x64_standard_context *frame) {
    global_ipi_handler();           // 执行
    write_eoi();
}
```

### 缺陷

- **不可组合**：两次 `broadcast` 之间 `global_ipi_handler` 是共享全局，覆盖即丢失
- **无接收确认**：发端发完就走，不知道 handler 是否已在所有目标上执行完毕
- **无 per-CPU 差异**：所有收端跑同一个 handler，无法做差异化响应
- **无法区分语义**：无法区分跑飞型（sched/halt）和返回型（TLB flush）
- **竞争窗口**：写 handler 指针和发 IPI 之间有窗口，收端可能读到旧 handler

### 结论

v1 是临时过渡方案，只能支持单次广播 + 不检查完成。TLB shootdown 需要等待所有核确认完成，v1 无法满足。

---

## 第二章：v2 — 独立向量表（2026-05-26）

### 机制

把 IPI 拆成三个向量层次，各自有独立的递送路径：

```
soft_interrupt_functions[256]    — 跑飞型（no return）
ipi_descrioptors[256]            — 系统 IPI（返回/不返回混合）
tokens[256]                      — 硬件中断 / 动态分配
```

**向量分配：**

| 向量 | 用途 | 路径 | 行为 |
|------|------|------|------|
| START_SCHED | AP 首次调度 | ipi_descrioptors | 不返回 |
| RESCHEDDUE  | 远程抢占 | ipi_descrioptors | 不返回 |
| IPI_HALT    | 远程停机 | ipi_descrioptors | 不返回 |
| GLOBAL_TLB  | 全局 TLB 刷新 | ipi_descrioptors | 返回 |
| LOCAL_TLB   | 单核 TLB 刷新 | ipi_descrioptors | 返回 |

### 分层设计推演

**Layer 1 — 跑飞型（soft_interrupt_functions, 带 frame, 不返回）**
   向量固定，handler 拿 frame 后 `ud2`

**Layer 2 — 非跑飞型（tokens, 永久槽, 返回）**
   高频固定事务，handler 执行后 EOI 返回

**Layer 3 — 通用远程调用（tokens, 动态分配, 返回）**
   `alloc_vec/free_vec` 管理

### 遗留问题

- 每个表项 4K 空间占用（`soft_interrupt_functions[256]` + `ipi_descrioptors[256]` + `tokens[256]`）
- 仍然使用函数指针表，局部子竞争仍然存在
- 全局 TLB 的 `shared_inval_kspace_VMentry_info.completed_count` 是全局计数器，死核会引发 timeout
- 函数分发表 + 计数器的两层结构增加了解释开销：查询 vector → 查表 → indirect call；对比之下 v1 只是一个函数指针

### 全局 TLB shootdown 串行方案

```
lock(global_ipi_lock)
    构造 flush 包（pending_mask 全 1）
    sfence
    broadcast_all_exself_IPI(TLB_VEC)
    wait_all_acks(pending_mask == 0)
unlock(global_ipi_lock)
```

**优点**：简单、正确。
**缺点**：广播时所有 IPI 串行化；死核导致 pending_mask 永远等不完。

---

## 第三章：v3 — ipi_local_complex（2026-06-04）

### 核心观察

v2 的独立向量表解决了"不同 IPI 不同路径"问题，但没有解决：
1. **投递竞争**：两个发送者同时向同一目标发不同 IPI → `ipi_descrioptors` 的 func 被覆盖
2. **通信原语过重**：每个 IPI 类型对应一个表项，但表项只是个 `void(*)(x64_std_context*)` 指针 + `is_no_return` flag，没有携带参数的能力
3. **ACK 机制外挂**：`completed_processors_count` 是独立于 IPI 传递机制的全局计数器

**真正需要的**是一个 per-core 的单槽消息通道：发送端原子写入 {fn, arg} → 接收端原子取走执行 → 返回结果。

### 设计

#### ipi_local_complex

在 `gs_complex_t` 中新增一个 16 字节的原子槽：

```cpp
struct gs_complex_t {
    // ...
    alignas(16) volatile unsigned __int128 ipi_local_complex;
    // 编码: [0:7] = func (void*), [8:15] = arg/value (void*)
};
```

**状态：**
- `0` = 可抢占（idle）
- `非零` = 有消息待处理或返回值待消费

**只有两个普通向量 + 一个硬编码向量：**

| 向量 | 名称 | 行为 |
|------|------|------|
| 1 | `IPI_HALT` | 硬编码，跳过 slot，直接 `cli; hlt` |
| 2 | `IPI_RETUABLE` | 通过 slot 收发，返回结果 |
| 3 | `IPI_UNRETURNABLE` | 通过 slot 收发，不返回 |

#### 原子操作原语

```nasm
; bool cmpxchg16b(void* ptr, void* expected, const void* desired)
; RDI = ptr, RSI = expected* (失败时更新), RDX = desired*
; 内部 lock cmpxchg16b
; → 纯汇编，无 libatomic 依赖
```

#### 跑飞型 IPI（UNRETURNABLE）时序

```
Sender:                            Target:
  cmpxchg(slot, 0 → {fn, arg})          IPI_UNRETURNABLE 到达
  ├─ 成功 → send IPI                     │
  └─ 失败 → 重试                       handler:
                                           {fn, arg} = slot
                                           slot = 0           ← 释放槽
                                           EOI                ← 在 func 之前
                                           fn(arg)            ← sched / halt, 不返回
```

`slot = 0` 在 `fn(arg)` 之前。UNRETURNABLE handler 执行期间 `sti` 后嵌套中断到来，slot 已归零可被抢占。

#### 返回型 IPI（RETUABLE）时序

```
Sender:                            Target:
  cmpxchg(slot, 0 → {fn, arg})          IPI_RETUABLE 到达
  ├─ 成功 → send IPI                     │
  │  while(slot != 0) pause()          handler:
  │  ret = slot.high64                    fn, arg = slot
  │  slot = 0                             // slot 保持非 0，阻挡新 IPI
  └─ 失败 → 重试                          ret = fn(arg)
                                           slot = {fn=0, arg=ret}  ← 写回返回值
                                           EOI
```

Sender 轮询到 slot 恢复为返回值格式，读取后清 0。

#### IPI_HALT

完全不碰 slot。直接 `cli; hlt`。核武器语义，panic 和关机场景使用。

### 为什么 3 个向量就够了

前两代设计陷入了"一个 IPI 类型一个向量"的思维。实际上一个 per-core 原子槽可以承载无限多种消息——

槽里装的是 `{fn*, arg*}`，不是枚举常量。fn 决定了行为。只需要区分"是否返回"和"是否走槽"：

| 实际 IPI 事务 | 映射到 v3 |
|---------------|-----------|
| RESCHEDDUE | UNRETURNABLE → slot → sched() |
| START_SCHED | UNRETURNABLE → slot → start_sched() |
| TLB shootdown | RETUABLE → slot → invalidate() |
| 远端函数调用 | RETUABLE → slot → fn(arg) |
| Panic 停机 | HALT → cli; hlt |
| 普通 IPI | 全部合并到以上三类 |

**不需要 `ipi_descrioptors[256]`，不需要 `global_ipi_handler`。**

### cmpxchg16b 作为单核互斥

cmpxchg16b 的原子性保证了：
1. **排他投递**：多发送者同时向同一目标发 IPI，只有一个 cmpxchg 成功
2. **参数完整性**：{fn, arg} 作为一个 16B 原子单元写入或读取，中间态不可见
3. **状态明确**：`0 = 空闲`，`非 0 = 占用`，无歧义
4. **无 libatomic 依赖**：`cmpxchg16b.asm` 是纯内联 `lock cmpxchg16b`，NASM 16 条指令

### 死核检测

Sender 轮询 `slot != 0` 超时 → 该核未回来 ACK → 可判定为死核。
对比 v2 的 `completed_processors_count` 只知道"不够数量"，不知道"哪个没回复"。

### 标准 IPC 化

如果将来需要两个核之间多轮交互（如热拔插协调），可以在 slot 返回后继续走：
```
Core A → cmpxchg(slot, {req1, a}) → B 回复返回值 r1
Core A → cmpxchg(slot, {req2, b}) → B 回复返回值 r2
```
多轮通讯仍然走同一套 cmpxchg 协议，不需要新增基础设施。
但工程纪律建议：**一槽只做一轮往返，多轮通讯用共享内存，不滥用 IPI 槽。**

---

## 第四章：v3.1 — UMONITOR/UMWAIT sender 端休眠等待（2026-06-05）

### 动机

原 v3 sender 端 RETUABLE 路径使用 `while(slot != 0) pause()` 忙等。
当 target 核执行延迟较大（如 TLB invalidate），sender 持续 spin 浪费功耗。

### 机制

利用 UMONITOR/UMWAIT 将 sender 的 spin 替换为硬件监视 + C-state 休眠：

```c
// sender 端等待槽释放（RETUABLE 路径）
while (slot != 0) {
    _umonitor((void*)slot);          // 武装监视
    if (slot == 0) break;            // 双检：避免 arm 和 wait 之间错过 store
    _umwait(/*C0.1*/1, rdtsc() + TIMEOUT_TSC);  // 硬件休眠 + TSC deadline 兜底
}
```

**原理：**
- UMONITOR 使用 EAX 中的线性地址，经 guest 页表 → EPT → 解析为 HPA
- 监控硬件跟踪的是最终 **HPA（Host Physical Address）**
- Target 核 cmpxchg16b 写入同一 HPA → 缓存一致性协议触发唤醒
- TSC deadline 作为安全网：EPT 重映射或 vCPU 被 host 调度时兜底

### UMONITOR vs MONITOR 选择理由

| | MONITOR/MWAIT | UMONITOR/UMWAIT |
|---|---|---|
| 权限 | CPL=0 仅内核 | **任意特权级** |
| C-state | 传统 C1/C2/C3 | C0.1（快） / C0.2（省电） |
| TSC deadline | 无 | 内建 EDX:EAX 超时 |
| OS 限时 | 无 | IA32_UMWAIT_CONTROL MSR 可设 |

采用 UMONITOR/UMWAIT 而非 MONITOR/MWAIT，是因为在 AP 启动后期（sti 后），
sender 可能在 CPL=3 的用户态线程上下文中等待 IPI 结果。UMONITOR 不限制特权级。

### 环境感知：TCG 下永远不用 UMONITOR/UMWAIT

QEMU TCG 模式下 UMONITOR/UMWAIT **完全不实现**（group15 decoder 无对应入口，
CPUID 不暴露 WAITPKG），MONITOR/MWAIT 降级为 stub（`helper_pause()` 忙等）。

sender 端应写为：

```c
void ipi_sender_wait(ipi_local_complex_t *slot)
{
    switch (g_env) {
    case ENV_BARE_METAL:
    case ENV_KVM:
        while (slot->raw != 0) {
            _umonitor(slot);
            if (slot->raw == 0) break;
            _umwait(1, rdtsc() + 100000);  /* C0.1, ~100μs TSC deadline */
        }
        break;

    case ENV_TCG:
        while (slot->raw != 0)
            _mm_pause();
        break;
    }
}
```

### 唤醒可靠性

风险在于 vCPU 被 host 调度器踢出时 monitor 硬件状态丢失：

```
UMONITOR(&slot)   → monitor hardware armed
if (slot != 0)    → 继续
[VM-exit / vCPU preempted by host]  ← monitor 状态丢失！
[Target stores slot=0]              ← 没人监视，唤醒丢失
[VM-entry]                          ← vCPU 回来
UMWAIT                              ← 睡着直到 TSC deadline / 中断
```

**解决：** TSC deadline 兜底 + while 循环回检。`_umwait` 设一个合理的最大休眠时间
（如 100μs TSC quanta），醒来后重新 UMONITOR。不会死锁，只是偶尔多一轮 retry。

### 对 v3 文档的改动总结

| 位置 | 改动 |
|------|------|
| RETUABLE sender 端 | `while(slot!=0) pause()` → UMONITOR/UMWAIT 休眠等待 |
| UNRETUNABLE sender 端 | cmpxchg 投递后直接返回，不变 |
| TCG fallback | `while(slot!=0) pause()` 保留作为 TCG 降级路径 |
| 依赖 | 新增 `g_env` 全局环境变量（见 `Docs/runtime_env.md`） |

### 附录：待实现（增量）

1. 实现 `_umonitor()` / `_umwait()` 包装函数或内联 asm
2. 在 RETUABLE sender 路径插入环境感知的等待 loop
3. TCG 降级路径使用原 `pause()` spin
4. 验证 KVM 下 UMONITOR/UMWAIT 透传正确性

### 与之前的对比

| 维度 | v1 (global_handler) | v2 (独立向量表) | v3 (ipi_local_complex) |
|------|------|------|------|
| 投递机制 | 写全局 ptr | 查表 indirect call | cmpxchg16b 原子槽 |
| 竞争保护 | 无 | 局部（表项替换） | 强（cmpxchg 排他） |
| 参数传递 | 无（全局变量） | 无（仅函数指针） | {fn, arg} 16B 内嵌 |
| 返回值 | 无 | 无 | 编码回 slot |
| ACK 机制 | 无 | 外挂计数器 | 槽归零即 ACK |
| 死核检测 | 不可能 | 困难（计数器盲） | 轮询超时可知哪核 |
| 向量用量 | 1 个（240） | 5+ 个 | 3 个 |
| 代码量 | ~10 行 | ~200 行（表 + 注册） | ~40 行（分发 + cmpxchg） |

---

## 附录：待实现

1. 在 `gs_complex_t` 增加 `alignas(16) unsigned __int128 ipi_local_complex` 字段
2. 注册 `IPI_RETUABLE` / `IPI_UNRETURNABLE` / `IPI_HALT` 三个向量到 vec_demux 分发器
3. 实现 `cmpxchg16b` 纯汇编函数（已完成）
4. 迁移 `broadcast_exself_fixed_ipi` 到新槽机制
5. 迁移动态 TLB shootdown 到 RETUABLE 槽
4. 迁移 `panic()` 中的 `other_processors_froze_handler` 到 HALT 向量
7. 废除 `global_ipi_handler`、`ipi_descrioptors[256]`、`soft_interrupt_functions[256]` 的相关部分
