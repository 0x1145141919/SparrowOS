# WRAITH · 疑似解决里程碑（2026-09-17）

> **状态**：`疑似已解决 / 【未结案】` —— 根因已定位、修复已实现并**初步 A/B 验收**，
> 但**尚未满足结案条件**（缺调度器单元压测 / 并行性回归 / 通稿），见 §4。
> **性质**：里程碑记录（相对 `WRAITH.md` 的"活战报"）。结案后另出**史诗/通稿**（§4.3）。
> **关联**：`WRAITH.md`（权威上下文）· `WRAITH_multischedule_review.md` · `WRAITH_static_review.md` ·
> `w22_01_report.md` · `w13_report.md`；通用武器库在上级 `../TCG_TRACE_ARSENAL.md`。

---

## 0. 一句话

2026-09-17：经**静态审查**（两份 subagent 分析）把 WRAITH 收敛到**"双调度/多调度"**——
阻塞/退出者「已发布 `blocked`/`zombie`、但尚未切离本核栈」期间，被异核 `唤醒 + sched() 偷取`，
**同一片内核栈被两核并发压** → 帧内指针槽被覆写成异核的 `call` 返回址；
据此实现修复 **F1–F5**（`67e4a76`），同配方 A/B：**pre-fix 命中 / post-fix 0/40**。
**但**这仍是"疑似解决"——**未过调度器单元压测、未证并行度未被改没、史诗未写**。

---

## 1. 里程碑时间线（基于 git）

### 露头（2026-09-15）
- `42f6bbe` panic/vec_demux 调试期加固（GS_BASE 钳位 + IPI 空槽防御 + panic 永不返回）
- `55cef76` / `1f86f2c` TCG+trace 抓取器 + **WRAITH 战报立锚 + 武器库成篇**
- `b169468` WRAITH 现场解析工具链（符号化精读 / 内存寻址 / GDB 后验）

### 攻坚（2026-09-16）
- `de187e2` / `0025271` 内存转储改用 QEMU 官方 `dump-guest-memory` + 虚拟视角 `mkcore`
- `c4a7438` 弃可重入锁；临界区改"先关中断再上锁"
- `4b300a0` `[BISECT]` 忙等实验（**后来证明它把复现率压没了**）
- `f6d2038` / `0f9088c` 异常入口 IST 权宜加固（防自噬风暴）
- `f139ce0` QEMU trace 档位死路（只保留 `in_asm`）；样本 `t10_33` 首爆冻结

### 突破（2026-09-17）
- `bc37012` 首爆冻结取证链（拆旧 int3 魔断点；#PF/#GP 入口冻结 + QEMU 断点钉死/自检）
- `0fe0463` `FAULT_FREEZE` 钩子；`d4c3568` **`interrupt_log_ring`**（IRQ-safe 内存日志环）
- `edcf5f7` **首爆留证探针**（`wraith_probe.h`：W/R/S/D 面包屑 + 取时 H0/H1 + "异常帧被踩不再静默放行"）
- `50f6c0e` 工具/文档强一致（`ring-dump.py`、废弃 `.ram` 老路、`--skip-noise-re`；docs 唯一权威）
- 两份**静态审查**报告成文（→ 本目录 `WRAITH_multischedule_review.md` / `WRAITH_static_review.md`）
- `3ba5b1f` 回滚 `[BISECT]` 忙等 → `kthread_sleep`（**复现率恢复**）
- `67e4a76` **修复 F1–F5**（handoff 安全 / 不可重入调度 / 锁内快照 / 退出栈保护）
- A/B 验收通过（§3）；`9cdaf58` 本目录成篇

---

## 2. 根因（已收敛）

**跨核同栈并发写**（非"两核同时 `sched()` 同一 task"——那会撞 `set_running` 失败 panic，样本不崩那）：
1. **先发布、后切走**（无 handoff 握手）：`block_if_equal`/`kthread_sleep`/`self_blocked`/`exit` 均先改状态+入队、再 `next_task_with_routine()`；
2. **唤醒不查"谁在跑"**：`bq_flush_pending` 无条件 `set_ready`+异核入队；`set_ready` 还放行 `running→ready`；
3. **`sched()` 跨核偷取无 owner 校验**。
叠加"**中断里嵌套整段调度**"（设备 IRQ → `resched`），把窗口拉长。
字节指纹：被覆写值 = 取时/调度路径的 `call` 返回址（`w13 0x2C700`）；写**不遵循本帧 `rsp`**（在其上）。
受害者均在 **NVMe 并行初始化**时段；`HPET_driver::get_time_stamp_in_us` / `sleep_tasks_wake` 为高发落点。

---

## 3. 修复与初步验收（`67e4a76`）

| # | 修复 | 依据 |
|---|---|---|
| F1 | `pop_all`/`pop_timeouts` 入口复位 `batch_count` + 写前边界判（`arr[64]` 越界/重复入队） | MS-10 |
| F2 | `sched()` 锁内快照 `priv_ctx`，锁外 `atomic_load_from(&snap)` 落地（消除撕裂 iret 帧 → `RIP=0`） | MS-6 |
| F3 | 每核"正在调度"门 `g_cpu_in_sched[]`；`resched` 去 `[[noreturn]]`、见门即返回不嵌套 | MS-2 |
| F4 | `sched_handoff.h`：每核 `g_cpu_running[]` 登记 + 每任务 `wake_pending`；唤醒"仍在别核跑"的任务只置 pending、不异核入队；`sched()` 交接点换登记+补投；`try_take` 拒认领"仍在别核"的任务 | MS-1/4/5（主嫌）|
| F5 | `release_kthread` 释放内核栈前确认任务不在任何核登记中（否则有界自旋/延迟） | MS-3 |

**A/B（同配方：非忙等 · SMP6 TCG · 12× `yes` · 40 组 · `--dump-vmcore --mkcore --skip-noise-re`）**
- pre-fix（`git revert -n 67e4a76`）：`pre01–07` KSHELL → **`pre08` FAULT**（首爆 `sleep_tasks_wake`，`CR2=0x7ffff`）。
- post-fix：**0 异常 / 40**（37 KSHELL + 3 AP 启动 NOISE；37/37 到 `kshell>`；NVMe 窗口照走）。
- 判据：pre ~1/8 命中 vs post 0/40 ⇒ 修复**消除**该竞态（同配方同负载，排除纯观测漂移）。

---

## 4. 【未结案】还差什么（TODO）

### 4.1 调度器单元压测（此前从未做过）
WRAITH 的修复动的是**调度器状态机与并发**，但**从未有单元级压测**兜底。需补：
- **状态机**：`set_ready`/`set_blocked`/`set_running`/`set_zombie`/`resurrect` 的合法/非法转移矩阵与断言；
- **队列**：`ready_queue`/`sleep_queue`/`block_queue`（`pop_all`/`pop_timeouts`/`bq_flush_pending`）的并发进出、`batch_count` 复用、边界（≥64）；
- **新件专项**：F4 的 `g_cpu_running[]` 登记一致性、`wake_pending` 的"置位↔补投"不丢不重、`try_take` 延后不会饿死；F3 门"置位必达清位"；F5 自旋耗尽的处理；
- **跨核场景**：block-wake、跨核唤醒、同核重入、抢锁时序。宿主/内核双形态皆可。

### 4.2 并行性回归（确保**不是**靠"串行化"把 bug 藏了）
F1–F5 加了登记/锁/门——必须证明**并行度没被改没**：
- 6 核并行吞吐 / 调度切换延迟 / 唤醒时延，修复前后对照；
- 任务在多核上的真实分布（是否仍被并行调度、跑在 ≥2 核）；
- **A/B 归因**：只回退 **F4**（其余保留）再跑一批——若异常回归 → 主责即 F4；若并行度在 F4 下显著下降 → 说明"堵洞"顺带把并行压了，需重审 F4 的代价。
- 判据：修复不得以**显著降低并发/吞吐**为代价（否则是"藏 bug"而非"修 bug"）。

### 4.3 史诗 / 通稿（9-15 露头 → 压测过完）
基于 **git 历史 + OpenClaw 原始数据**，成文完整叙事：从 9-15 首战锚定，经工具链/取证链/静态审核，
到定位双调度、修复、A/B、单元压测与并行性回归——**结案通稿**。

### 4.4 结案判据（建议）
① 调度器单元压测绿；② 并行性无显著回退；③ 长批（≥100 组）零异常 + 同配方可复核；
④ 通稿成文并归档本目录。**四者齐，方可标 `结案`。**

---

## 5. 引用与数据
- 战报：`WRAITH.md`（§11 收口 / §12 目录）；分析原文：本目录另四件。
- 样本（store）：`wh02` / `wl02` / `pre08`（各 8.1G `.vmcore`+`.core`）；工具 `Tools/tcg-trace/ring-dump.py`。
- 修复提交：`3ba5b1f` + `67e4a76`；里程碑件提交：`9cdaf58`。
