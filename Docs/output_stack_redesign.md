# 输出栈重构草案（Output Stack Redesign）

> 状态：**draft（待设计方确认）**
> 版本：**v3 —— 归因修正为「历史债务」**（v2 `06ad24a`，v1 `0f077df`）
> 作者：Raven（AI 草案）
> 日期：2026-09-10
> 关联：`Docs/kout_tmp_buff.md`、`Docs/Debug/spdb_roadmap.md`、`Docs/kshell_framework_design.md`、`Docs/KERNEL_DISCIPLINE.md` §11
> 标注约定：事实带 `file:line`；推断打 ⚠️；待拍板汇总在 **§7 对齐点清单**。

---

## 0. 根因（一句话）

**`bsp_kout` 是前调度器时代（2025-08）为「非中断轮询串口」写的「老将」——在那个年代它是合理的。** 真正的问题是：**今天 runtime / kshell / panic 的多场景 I/O 栈需求，把这份历史债务挖掘了出来**——当年成立的设计前提（**无调度、无并发、单路同步**）今已全部崩塌，这份债**必须重构清偿**。

> 这不是「一个 kout 解决一切的野心」，而是**随时间累积、被新需求暴露的历史债务**。
> 类比 STL streams：今天要求 `ifstream`/`cout`/`cerr` 各司其职——可 `bsp_kout` 出生时压根没有这些场景。

---

## 1. 论点（三箭）

### 1.0 债务的两个侧面（历史前提已崩塌）

当年前提：**无调度器 → 单路、同步、无并发**，输出只有「早期启动」一种场景。今天前提的反面全部成立：

- **A 面（结构债务）**：一个 `bsp_kout` 要同时服务早期、runtime、kshell、panic，被迫把所有场景的行为塞进一个 backend 结构 → **backend 膨胀**；kshell 只能借道日志管线 → **退格等 UI 需求难以实现**。
- **B 面（状态债务）**：`bsp_kout` 是 **BSS 全局对象**，是「无并发」时代的产物（多线程不友好）；且内部携带**长生命周期进制子状态**，其编排范式 **AI 不友好**（谁改了状态、何时复位，靠人脑记忆）。

→ 二者是**同一份历史债务**：在「无调度、无并发、单路同步」下攒成，如今被 runtime/kshell/panic 的复杂需求挖出。

### 1.1 箭一（**结构性修复**，本草案重心）：不同场景 → 不同 kout

- 每个场景一个 kout 对象：`log_kout` / `kshell_kout` / `panic_kout`（可再细分）。
- **附带红利**：每个 kout 的后端数目上限**可在编译期按业务预测** → **外部编排引索、静态分配**（消掉 `register_backend` 的 `new` 与膨胀）。

> 这一箭清偿 **backend 膨胀 + kshell 难以改造**，是历史债务的直接解。

### 1.2 箭二（**工程抉择**）：长线 kout 的进制子状态 → `tmp_buffer` 一律栈上

- 问题 1：`kout` 内部进制子状态的编排范式 **AI 不友好**。
- 问题 2：`kout` 在 **BSS 区、多线程不友好**。
- 解法：一律 `tmp_buffer`、**栈上分配**；内部仍保留进制选择，但**可在栈上安全调整**（per-record，不被并发/后续代码污染）。

### 1.3 箭三（**工程抉择**）：level 数码比较

- `kout` 与 `tmp_buffer` 各带内部**数码（`level_code`）**，比较决定是否打印 → 实现不同日志等级。

---

## 2. 现状证据（服务论点）

### 2.0 历史考古（归因依据）

| 事件 | 提交 | 日期 |
|------|------|------|
| **`bsp_kout` 诞生**（「实现基本输出驱动，**非中断串口驱动**」，前调度器时代） | `b6b6751` | 2025-08-28 |
| backend 模型 `src/include/util/kout.h` 引入 | `a5bbd89` | 2026-01-10 |
| 旧 `src/include/init/util/kout.h` 引入 | `fa5cbd8` | 2026-03-11 |
| **调度器头文件成型** | `9859e53` | 2026-07-11 |

→ `bsp_kout` 比调度器早约 **10.5 个月 / 246 个提交**；其 5 指针 backend + 全局 radix 是**在「无调度器」前提下攒成**。归因据此：**历史债务**，而非设计野心。

### 2.1 单一 `bsp_kout` → backend 膨胀

`src/include/util/kout.h`：`kout_backend` 带 **5 个函数指针**，把三个 stage 的传输塞一起：

```c
void (*running_stage_write)(const char*, uint64_t);
void (*running_stage_putchar)(char);
void (*running_stage_num)(uint64_t, num_format_t, numer_system_select);
void (*panic_write)(const char*, uint64_t);
void (*early_write)(const char*, uint64_t);
```

**决定性证据** —— `src/arch/x86_64/core_hardwares/x86_arch/PortDriver.cpp:199`，同一个 COM1：

```c
running_stage_write   = backend_submit_write,   // runtime: 投 ring
running_stage_putchar = backend_submit_putchar,
running_stage_num     = backend_submit_num,
panic_write           = polling_puts,           // panic: busy-poll 同步
early_write           = polling_puts,           // early: busy-poll 同步
```

→ **三套不同传输**硬塞进一个 struct，靠 `uniform_puts`（`src/utils/kout.cpp:306` 的 `switch(GlobalKernelStatus)`）每次挑选。

### 2.2 kshell 绑定 `bsp_kout` → 退格困难

- `Docs/kshell_framework_design.md:10`：「输出：统一使用 `bsp_kout`」。
- `src/utils/kshell.cpp:37,45`：`bsp_kout << '\b';`（退格经日志管线发）。
- `src/arch/x86_64/core_hardwares/i8042/i8042_kshell.cpp:397,625,701`：`bsp_kout << "\\b";`（打印**字面** `\b` 字符串）。

→ kshell 只需要"向屏幕上屏/退格"，却借道日志后端；屏上与日志需求不一致，退格/光标一类操作实现别扭。

### 2.3 panic 场景需求**相反**：不可睡眠、必须同步

- `src/init/panic.cpp:29` 注释：「无条件切换 CPU 资源，**使用 BSP 的 EARLY_BOOT 那一套**」。
- panic 走 busy-poll 直写（`polling_puts`），**不能依赖调度/中断/睡眠**。

→ 与 runtime（可异步、投 ring）需求相反，挤一个对象必然畸形。

### 2.4 `kout` 的 BSS 全局态 + 长生命周期进制 → AI 不友好 + 货不对版

- `kout` 持 `curr_numer_system` + `shift_bin/dec/hex()`（`src/include/util/kout.h:47`）：**进制是挂共享对象上的可变全局态**；`tmp_buff` 另有一套 `num_sys` → **两套状态机**。
- 货不对版实证：

| 位置 | 现象 |
|------|------|
| `src/init/panic.cpp:48` | `shift_hex()` 后**从不复位** → 其后输出全变 hex |
| `src/init/pages_alloc.cpp:600,610` | 区间内 `index`、`NumberOfPages`、`(size/1024) KB` **一起被打成 hex** |
| `src/arch/x86_64/core_hardwares/PCIe/PCIe.cpp:23,46,68,...` | 多处 `shift_hex()`，靠后续调用自觉收尾 |

### 2.5 两套实现并存

`src/utils/kout.cpp`（新）与 `src/init/util/kout.cpp`（旧）各被 CMake 两 target 编译（`CMakeLists.txt:150` 与 `:210`）。

---

## 3. 目标架构

### 3.1 场景 kout + 静态后端编排（箭一）

```
场景（调用者） ──► 该场景的 kout ──► 该 kout 的 backend 表（编译期定上限、外部编排引索）
  log                    log_kout         [console, dmesg, spdb-eth, (file)]
  kshell                 kshell_kout      [GOP text console]
  panic                  panic_kout       [fb, 预注册物理区段]
```

- **无全局 current 指针**：调用者用**它认识的场景 kout**，不做运行时挑选。
- **后端槽位数上限按业务静态预测**，由场景定义处**外部编排引索**（固定区间、静态分配）→ 消掉 `register_backend` 的 `new`（`src/utils/kout.cpp:702`）。
- kshell 用自己的 kout → 退格/清屏/光标等 UI 操作可**直连 GOP 控制台**（`textconsole_GoP::PutChar/Clear`），不再借道日志。

### 3.2 `tmp_buffer` 一律栈上（箭二）

- 唯一 formatter；`LOG(lvl) << ...` 在栈上建 Record（无线程安全、随作用域回收）。
- 进制选择搬进 buffer：`buf << HEX` 改 `num_sys`，每个数值 entry **append 时快照** → 每条记录自带进制，**免疫并发与后续代码污染**。
- `kout` 侧只保留 `operator<<(tmp_buffer&)`（消费/投递）。

### 3.3 level 数码比较（箭三）

- `level_code`（`src/include/abi/os_error_definitions.h:86`）：`INVALID=0 < INFO=1 < NOTICE=2 < WARNING=3 < ERROR=4 < FATAL=5`。
- Record 带 level；每个 sink/backend 带 `min_level`；`record.level >= sink.min_level` 才打印，否则丢弃**并计数**（`suppressed_count`，不可静默）。
- 顶部宏短路：`if (lvl < g_min_level) return;` → 没人要就不建 Record。
- ⚠️ INFO 以下无 `DEBUG/TRACE`；若要让某 sink 更啰嗦需补低端等级。

### 3.4 场景内的上下文约束（← 你上轮的 P3，作为**机制**保留）

不同场景的**上下文契约不同**，且 kshell/panic 各自独立后，剩下的痛点在 **log 场景**（它既被进程上下文、也被 IRQ 调用，而后端里有**可阻塞**者如文件）：

- 后端按上下文分：**immediate**（任何上下文安全：fb、UART polling）／**deferred**（may-block：文件、eth 慢路径）。
- **可阻塞后端永不直接被 producer 调用**，只由 **drainer（可睡眠 kthread）** 喂；IRQ/原子/panic 上下文只落 immediate。
- **旁证**：`textconsole_GoP` **已经**自带 `RuntimeServiceThread` + `RuntimeSubmit*`（`src/include/util/textConsole.h:89`）——producer 投递、服务线程排空的 **drainer 模式你们已用过**，只是没推广到日志。

---

## 4. 与 SpDB 排期的关系

`Docs/Debug/spdb_roadmap.md` 要新增两个后端：**runtime 以太网 logcat**、**panic 预注册物理区段裸写**——恰好分别落到 **log_kout 的 deferred 表** 与 **panic_kout**。**本改造是 SpDB 第 0 步**：模型就位后，SpDB 只需"注册一个后端"，不碰调用点。

---

## 5. 迁移计划（按三箭）

| 步 | 对应 | 内容 | 可验证点 |
|----|------|------|----------|
| **S0** | 箭二 | 冻结 kout 进制状态：删 `curr_numer_system`/`shift_*`/数值重载，只留 `operator<<(tmp_buffer&)`；过渡 shim（`bsp_kout<<X` 内部临时建 Record 再 flush，radix 来自 buffer） | panic 后不再"变 hex"；调用点语义不变 |
| **S0.5** | — | 修 §6 即时 bug（`DmesgRingBuffer` rwlock、register/mask/unregister） | IRQ 写 ring 不再死锁 |
| **S1** | 箭一 | 拆场景 kout（log/kshell/panic）＋静态后端编排；删 `kout_backend` 5 指针与 `uniform_puts` 的 stage 分支 | kshell 脱离日志管线；`new` 消失 |
| **S2** | 箭三 | level 数码比较 ＋ `suppressed_count` ＋ 宏短路 | 分级打印可按 sink 配置 |
| **S3** | 3.4 | log 场景内 immediate/deferred 分离 ＋ drainer（复用 `creat_kthread`） | FS 未起时日志积压、起后落盘 |
| **S4** | §4 | 接 FS 文件后端 ＋ SpDB eth/panic 后端 | 新后端零调用点改动 |

> **决策点 D-mig**：是否先 S0.5（低风险高收益）再进 S0。

---

## 6. 待修缺陷清单（可直接当 TODO）

1. `src/utils/kout.cpp:702` `register_backend` 用 `new` → 静态分配。
2. `src/utils/kout.cpp:722` `mask_backend` 语义反了（只 unmask）。
3. `src/utils/kout.cpp:713` `unregister_backend` delete 后不置空 → 悬垂/double free。
4. `src/utils/tmp_buff.cpp` 满时静默丢 → 加 `dropped_count` ＋ 溢出标记。
5. `src/include/kcirclebufflogMgr.h` `DmesgRingBuffer` 用 rwlock → IRQ 死锁隐患，改无锁/每核。
6. `defalut_KURD_module_interpator`（`src/utils/kout.cpp`）在 flush 持锁时回调 `bsp_kout` → 核实自锁（spinlock 可重入性）。
7. 两套 kout 实现（`src/utils/` vs `src/init/util/`）→ 统一或明确边界。
8. `print_numer` 把格式化泄漏给 `running_stage_num` hook → 后端 sink 化后去掉 num hook。

---

## 7. 对齐点清单（**低带宽复核区**）

> 请直接圈点：✅ 同意 / ✏️ 改 / ❌ 否。

- [ ] **D0** 承认归因 ＝「**前调度器时代的历史债务**被复杂 I/O 需求挖掘」（非设计野心）；三箭中**箭一为结构性修复（清偿核心债务）**、箭二/箭三为工程抉择。
- [ ] **D1** 场景 kout 划分：`log_kout` / `kshell_kout` / `panic_kout`（是否还有其它场景？）
- [ ] **D2** 后端**静态编排引索**：每个 kout 槽位上限定死、外部编排（确认采纳）
- [ ] **D3** `kshell_kout` 是否**完全**直连 GOP 控制台（自带 UI 操作），不做 Record？
- [ ] **D4** 箭三 level：是否补 `TRACE/DEBUG`（低于 INFO）？是否引入编译期等级？
- [ ] **D5** §3.4 上下文约束：log 场景内 immediate/deferred 分离 ＋ drainer（确认保留）
- [ ] **D6** panic 时是否把 dmesg 内容带进崩溃报告（读已写 ring，还是主动 dump）？
- [ ] **D7** drainer 的 ring：**每核无锁** vs **全局 irq-save 锁**？
- [ ] **D8** 迁移顺序：先 S0.5 再 S0，还是并行？

---

## 8. 开放问题（待讨论）

- `PANIC_WILL_ANALYZE`(stage=2) 归哪类？（现路由到 early_write）
- 渲染器一份 vs 每个场景自带渲染（kshell 要控制序列，日志不要）→ 是否需要一个 `UI` 专用 Record 类型？
- deferred 的顺序性：单 drainer 保序；多 drainer 需分区。

---

*v3 由 AI 按设计方 2026-09-10 论点重构（归因：历史债务）；推断（⚠️）与决策点（D*）需设计方确认后升为 spec。*
