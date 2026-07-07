# DTS 调度器升级计划草案

> Dynamic Timeslice Scheduling — 动态时间片调度算法

---

## 一、核心理念变迁

### 旧模型：Periodic Tick Preemptive

```
时钟 ← 固定间隔 → 时钟 ← 固定间隔 → 时钟
                      │                     │
                      └→ resched()          └→ resched()
```

- 依赖周期性的时钟中断触发调度
- 所有调度事件被动的、时间驱动的
- 任务时间片固定，无自适应

### 新模型：Event-Driven Preemptive with Starvation Guard

```
任务开始运行
  │
  ├→ 主动让出（yield/block/wait）
  │     └→ 调度新任务，设 one-shot 截止线时钟
  │
  ├→ IO 中断唤醒（NVMe/键盘/网络）
  │     └→ 检查紧迫度 + 剩余时间片 → 决定是否抢占插队
  │
  └→ 截止线时钟过期（保底）
        └→ 任务超时 → 调度走
```

- 主调度入口是 **IO 中断 + 主动让出**，时钟仅为保底
- **One-shot 时钟**，不是 periodic tick
- 自适应时间片，按行为调节

---

## 二、DTS 时间片自适应

### 2.1 核心变量（在 task 上）

```c
struct {
    uint32_t dts_timeslice_us;      // 当前分配的时间片 (μs)
    uint8_t  cpu_stickiness;        // CPU 粘性分 (0~255, EMA)
    uint64_t dts_sched_stamp;       // 本次开始运行的时间戳
};
```

### 2.2 时间片计算

```
每次调度到新任务时：
  new_timeslice = BASE_TIMESLICE_US * cpu_stickiness / 128
  clamp(MIN_TIMESLICE_US, MAX_TIMESLICE_US)

基准时间片: 8ms
最小值: 1ms (纯 IO 任务)
最大值: 64ms (纯 CPU 任务)
```

### 2.3 更新时机

每次 `schedule_and_switch` 切走 `prev` 任务时：

```
actual_run = now - prev->dts_sched_stamp
ratio      = actual_run / prev->dts_timeslice_us

if (reason == TIMER || ratio >= 90%):
    // CPU-bound：被时钟打断或几乎跑满
    increase cpu_stickiness (EMA)

else if (reason != YIELD/BLOCK && ratio <= 25%):
    // IO-bound：被中断早早切走
    decrease cpu_stickiness (EMA)

else:
    // 灰色区域，不变
```

### 2.4 全量重置语义

```
任务被切走 → 插回 ready_queue 队尾
再次调度时 → 时间片重置为满（100%）
  不是截止线扣减语义，而是每次分配一个完整量子
```

---

## 三、CPU 粘性分（cpu_stickiness）

### 3.1 定义

衡量一个任务对 CPU 的"粘性"：
- **高粘性**：重计算、轻调用，讨厌被打断（编译、渲染）
- **低粘性**：IO 密集、频繁让出，不在乎打断（键盘响应、网络）

### 3.2 更新公式（EMA）

```
sample = ratio × 256          // actual_run / timeslice，归一化 0~256
stickiness = (stickiness × 15 + sample) / 16   // α = 1/16
```

### 3.3 半衰期

α=1/16 的半衰期约 10.7 次调度事件。
- 调度粒度 ~1ms 时，行为改变后约 **10ms** 收敛
- 没有窗口边界，没有跳变

### 3.4 分档

| 粘性分 | 标签 | 典型时间片 | 调度行为 |
|--------|------|-----------|---------|
| 192~255 | 高粘性 | 32~64ms | 优先放低噪音核，避免被打断 |
| 64~191 | 中性 | 8~16ms | 默认处理 |
| 0~63 | 低粘性 | 1~4ms | 放高噪音核，IO 友好 |

---

## 四、CPU IO 噪音指数（noise_score）

### 4.1 定义

per-CPU 的指标，衡量该核心被 IO 中断干扰的程度。
- **高噪音**：频繁有任务被 IO 中断在时间片早期切走
- **低噪音**：任务大多能跑满时间片

### 4.2 更新公式（EMA，per-CPU 调度器）

```
只在非自愿让出时更新：
  remain_ratio = (timeslice - actual_run) / timeslice    // 剩余占比
  noise_sample = remain_ratio × 256                      // 0~256
  noise_accum += noise_sample
  noise_count++

if (noise_count >= 32):             // 每 32 次刷新一次 EMA
    instant = noise_accum / 32
    noise_score = (noise_score × 7 + instant × 4) / 8    // 归一化 0~1024
    noise_accum = 0
    noise_count = 0
```

### 4.3 不计入噪音的事件

- `YIELD` / `BLOCK`：自愿让出，不算外部打扰
- `TIMER`：时钟保底，不算 IO 噪音

### 4.4 分档

| 噪音分 | 标签 | 迁移策略 |
|--------|------|---------|
| 0~200 | 安静 | 留给高粘性任务 |
| 200~500 | 正常 | 不主动干预 |
| 500~1024 | 吵闹 | 推送高粘性任务离开 |

---

## 五、粘性-噪音对称迁移

### 5.1 迁移规则

```
高粘性任务（≥192）→ 被推送线程迁到最低噪音核（同 L3 内）
低粘性任务（≤64）  → 被推送线程迁到最高噪音核（给粘性任务腾位置）
```

### 5.2 推送线程（load_balancer）

- 独立的常驻内核线程，每 **1ms** 唤醒一次
- 扫描所有 CPU 的 ready_queue
- 不做主动偷窃，只做定向推送

### 5.3 任务偷窃（idle_steal）

- **正常情况下不参与调度主路径**
- 只有 CPU 空闲超过阈值（如 100μs）才去邻居偷
- 推送线程崩溃时的正确性保底（退化到纯偷窃模式）
- 检查 stall_deadline：被推送的任务如果在目标 CPU 停留太久还没跑，偷窃必须将其抢走

### 5.4 Stall Deadline 铁律

```
被推送的任务带有 stall_deadline（例如 50μs）
闲核扫描时，如果 deadline 过期，必须优先偷走该任务
推送线程下次扫描时检查 deadline 过期 → 换一个目标 CPU
确保推送的任务不会在忙核上排队饿死
```

---

## 六、IO 中断紧迫度与插队

### 6.1 中断 handler 返回值语义

```c
// interrupt_token_t 的 func 返回值
bit 0:   NEED_SCHEDULE          // 需要触发调度
bit 1-2: URGENCY                // 紧迫度
            00 = LOW
            01 = NORMAL
            10 = HIGH
            11 = CRITICAL
```

### 6.2 各设备

| 设备 | 紧迫度 | 理由 |
|------|--------|------|
| i8042 键盘 | HIGH | PS/2 无缓冲，60ms 延迟人就察觉 |
| NVMe | NORMAL 或 HIGH | 按 CQ 类型判断，同步带 error 的加急 |
| 虚拟 IO 设备 | LOW 或 NORMAL | 通常不敏感 |

### 6.3 调度器决策

```
vec_demux dispatch 读到 token 返回值:
  拆出 URGENCY
  查当前任务剩余时间片占比

  CPU STICKY HIGH + 剩余多 + IO URGENCY LOW → 不切
  CPU STICKY LOW  + 剩余少 + IO URGENCY HIGH→ 切
  中间区域 → 查表或不变
```

---

## 七、单 Socket 缓存拓扑感知

### 7.1 AP bringup 阶段采集

每个 CPU 在 AP bringup 时执行：
- **CPUID.1FH**：拓扑域层级（HT → Core → Module/Tile/Die）
- **CPUID.04H**：每级 cache 大小 + sharing 域
- **CPUID.1AH**：Core Type（0x40=P, 0x20=E）

存入全局 `cpu_topology[]` 表。

### 7.2 BSP 收尾构建

```
遍历 cpu_topology:
  l2_share_mask = 同 L2 cluster 的 CPU 位图
  l3_share_mask = 同 L3 的 CPU 位图（单 socket 全部）
  core_type     = P / E / LP-E
```

### 7.3 调度器消费

```
迁移决策：
  同 L2 cluster → cost=low    (缓存基本热)
  同 L3          → cost=medium (L3 热，L1/L2 冷)
  不同 L3        → cost=high   (缓存全冷，单 socket 不应发生)
```

---

## 八、调度域与数据流总览

```
                    ┌──────────────────────┐
                    │   IO 中断 handler     │
                    │   (返回 URGENCY)      │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │   vec_demux dispatch  │
                    │   拆 token 返回值     │
                    └──────────┬───────────┘
                               │
          ┌────────────────────▼────────────────────┐
          │         schedule_and_switch()            │
          │                                         │
          │  1. 更新 prev 的 cpu_stickiness (EMA)    │
          │  2. 更新 per-CPU noise_score (EMA)       │
          │  3. 写 Gantt 条目                        │
          │  4. 从 ready_queue 选出 next             │
          │  5. next->dts_timeslice = 新值           │
          │  6. set one-shot timer                  │
          │  7. gs_u64_write(now_running_task)       │
          │  8. context switch                       │
          └─────────────────────────────────────────┘
                               ▲
                               │
          ┌────────────────────┴────────────────────┐
          │         Load Balancer (1ms)              │
          │                                          │
          │  扫描 ready_queue 队首                    │
          │  if 高粘性 && 当前核吵 → 迁到低噪音核     │
          │  if 低粘性 && 当前核静 → 迁到高噪音核     │
          │  if stall_deadline 过期 → 重迁           │
          └──────────────────────────────────────────┘
                               ▲
                               │
                    ┌─────────┴──────────┐
                    │    Idle CPU steal    │
                    │  （保底，非主路径）   │
                    └────────────────────┘
```

---

## 九、测量与调试基础设施

### 9.1 DTS Gantt

每 CPU 一个 4096 条目的环形缓冲区，按需分配：

```c
struct dts_gantt_entry {
    uint64_t tsc;                 // rdtsc
    uint32_t tid;
    uint16_t dts_timeslice_us;
    uint8_t  reason : 4;          // TIMER / YIELD / BLOCK / IO_WAKE
    uint8_t  preempt_cnt : 2;
    uint8_t  voluntary_cnt : 2;
    uint8_t  io_urgency : 2;
};  // 16 bytes
```

- `dts_gantt = nullptr` 时零开销（单分支跳过）
- kshell 命令：`dts_gantt enable | disable | dump | clear`

### 9.2 运行时参数调节（kshell）

```
dts_tune show                    # 显示当前所有参数
dts_tune stick_high 160          # 改粘性高阈值
dts_tune ema_alpha 32            # 改 EMA 衰减率
dts_tune noise_delta 16          # 改噪音迁移门槛
dts_tune base_timeslice 4096     # 改基准时间片(μs)
```

### 9.3 调试周期

```
1. dts_gantt enable
2. 跑负载
3. dts_gantt dump cpu=0 count=200  → 看甘特图
4. dts_tune 调参数
5. dts_gantt clear
6. 重复 2-5 直到满意
```

---

## 十、实现路线图

### Phase 1：基础框架
- [ ] `dts_gantt_write` 插桩 + kshell 命令
- [ ] `cpu_stickiness` 字段 + EMA 更新
- [ ] `noise_score` per-CPU 字段 + EMA 更新
- [ ] `dts_timeslice_us` 计算
- [ ] `dts_tune` kshell 命令

### Phase 2：调度主路径改造
- [ ] `schedule_and_switch` 加上时间片自适应逻辑
- [ ] timer 从 periodic 改为 one-shot（`set_clock_by_offset(timeslice)`）
- [ ] IO 中断 urgency 返回值定义 + vec_demux 传递到调度器

### Phase 3：拓扑与迁移
- [ ] AP bringup 缓存拓扑采集（CPUID.04H/1FH/1AH）
- [ ] `cpu_topology[]` 全局表 + l2_share_mask / l3_share_mask
- [ ] 推送负载均衡线程（`load_balancer`）
- [ ] 粘性-噪音对称迁移
- [ ] stall_deadline 过期强制偷窃

### Phase 4：打磨
- [ ] Gantt 复盘 + 参数调优
- [ ] 混合架构 P/E/LP-E 核心差异化处理
- [ ] ITD（Intel Thread Director）可选适配

---

## 附录：参数量表（初始推荐值）

| 参数 | 初始值 | 范围 | 描述 |
|------|--------|------|------|
| `BASE_TIMESLICE_US` | 8192 | 1024~65536 | 基准时间片(μs) |
| `MIN_TIMESLICE_US` | 1024 | - | 最小时间片 |
| `MAX_TIMESLICE_US` | 65536 | - | 最大时间片 |
| `EMA_ALPHA` | 16 | 8~32 | EMA 衰减率分母 |
| `STICKY_HIGH` | 192 | 160~224 | 高粘性阈值 |
| `STICKY_LOW` | 64 | 32~96 | 低粘性阈值 |
| `NOISE_SCALE` | 4 | 2~8 | 噪音分缩放因子 |
| `NOISE_MIGRATE_DELTA` | 32 | 16~64 | 迁移最小噪音差(归一化) |
| `STALL_TIMEOUT_US` | 50 | 20~200 | 推送任务超时(μs) |
| `PREEMPT_RATIO_X256` | 230 | 204~230 | CPU-bound 阈值(≥90%) |
| `VOLUNTARY_RATIO_X256` | 64 | 38~77 | IO-bound 阈值(≤25%) |
| `GANTT_CAPACITY` | 4096 | 1024~16384 | 每 CPU Gantt 条目数 |
