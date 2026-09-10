# 输出栈重构草案（Output Stack Redesign）

> 状态：**draft（待设计方确认）**
> 作者：Raven（AI 草案）
> 日期：2026-09-10
> 关联：`Docs/kout_tmp_buff.md`、`Docs/Debug/spdb_roadmap.md`（SpDB 排期）、`Docs/KERNEL_DISCIPLINE.md` §11
> 说明：本文把「事实（代码考古）」/「推断」/「待确认」分层标注。事实带 `file:line`；推断带 ⚠️；决策点汇总在 **§7 对齐点清单**，请直接在上面圈点。

---

## 0. 一句话结论

现在的输出栈把**三件事**揉进了 `kout`：**格式化（说什么）＋ 分发（往哪送）＋ 阶段分派（哪个后端）**，并把**进制状态**这种"内容属性"挂在了共享对象上。
改造方向：**Record（记录）/ Sink（裸函数＋归属表）/ Channel（按角色分的 kout）/ Drainer（可睡眠排空线程）** 四层分离；
**没有全局 current 指针**，**中断/原子/panic 上下文只能落到 never-block sink**，**可阻塞 sink 只由 drainer 喂**。

---

## 1. 设计意图来源（本轮讨论产出的三点共识）

| 编号 | 设计意图 | 来源 |
|------|----------|------|
| **P1** | **kshell 用自己的 kout**：kshell 是交互式 UI（键盘＋GOP），不是日志流，不该灌进内存 buffer/dmesg | 用户本轮 |
| **P2** | **`g_active_kout` 是过度设计**：全局可变指针 = 我们要消灭的"状态挂共享对象"，且引入翻转时序窗口 | 用户本轮 |
| **P3** | **阻塞后端不能进中断上下文**：FS 写日志文件会阻塞，绝不能被 IRQ/原子/panic 上下文调用 → 这是全局硬约束 | 用户本轮 |
| **P4** | sink 扩展性：新增（如文件日志）**只需提供一个符合签名的函数** | 用户本轮 |
| **P5** | kout 本质是**分发路由器**，不该带内部进制选择；`bsp_kout` 被滥用致"货不对版" | 用户上轮 |
| **P6** | 旧设计的真正轴是 **stage（early/runtime/panic）**，但它该是 **sink 的属性**，不该是每次写时的运行时分支 | 上轮讨论收敛 |

> **注**：P2 与 P6 看似矛盾（P6 想要三阶段类，P2 否掉切换它们的全局指针）。**本草案的调和**见 §3：**stage 下沉到 sink，kout 按 channel 分**。这是需要你重点确认的一处（决策点 D1）。

---

## 2. 现状盘点 —— 不合时宜的设计（事实，带证据）

### 2.1 `kout` 身兼 formatter ＋ router ＋ 全局进制状态

- `kout` 持有 `curr_numer_system` + `shift_bin/dec/hex()` + `operator<<(numer_system_select)`：**进制是挂在共享对象上的可变全局态**。`src/include/util/kout.h:47`，`src/utils/kout.cpp`（`shift_*` 实现）
- `tmp_buff` 又自带一套 `num_sys` → **两套进制状态机并存**，`kout << num` 用全局的、`kout << tmp_buff` 用 per-entry 的。

**"货不对版"实证：**

| 位置 | 现象 |
|------|------|
| `src/init/panic.cpp:48` | `dumpregisters` 里 `shift_hex()` 后**从不复位** → 其后 panic 输出全部变 hex |
| `src/init/pages_alloc.cpp:600,610` | 手动 `shift_hex()/shift_dec()`，但这区间内 `index`、`NumberOfPages`、`(size/1024) KB` **也一起被打成 hex**（本应是十进制计数） |
| `src/arch/x86_64/core_hardwares/PCIe/PCIe.cpp:23,46,68,...` | 多处 `shift_hex()`，靠后续调用自觉收尾 |

### 2.2 `kout_backend` 把三个 stage 的传输塞进一个 struct

`src/include/util/kout.h`：

```c
struct kout_backend {
    char name[64];
    uint64_t is_masked:1;
    void (*running_stage_write)(const char*, uint64_t);
    void (*running_stage_putchar)(char);
    void (*running_stage_num)(uint64_t, num_format_t, numer_system_select);
    void (*panic_write)(const char*, uint64_t);
    void (*early_write)(const char*, uint64_t);
};
```

**决定性证据** —— `src/arch/x86_64/core_hardwares/x86_arch/PortDriver.cpp:199`，同一个 COM1：

```c
running_stage_write   = backend_submit_write,   // runtime: 投 ring
running_stage_putchar = backend_submit_putchar,
running_stage_num     = backend_submit_num,
panic_write           = polling_puts,           // panic: busy-poll
early_write           = polling_puts,           // early: busy-poll
```

→ 这是**三套不同传输**被硬塞进一个 struct，靠 `uniform_puts` 每次挑选。

### 2.3 `uniform_puts` 每次 write 都读全局 stage 分支

`src/utils/kout.cpp:306` `switch(GlobalKernelStatus)`：EARLY_BOOT/MM_READY/PANIC_WILL_ANALYZE→early_write，SCHEDUL_READY→running，PANIC→panic。

- 阶段是**运行时全局变量**，每次写都读一次（非原子）。
- 转换点：`exec_env_prepare.cpp:36`(EARLY_BOOT)、`mem_init.cpp:384`(MM_READY)、`kinit.cpp:150`(SCHEDUL_READY)、`basic_init.cpp:18`(PANIC_WILL_ANALYZE)、`panic.cpp:120`(PANIC)。

### 2.4 kshell 输出串进了日志管线

- `src/firmware/uefi_kshell_commands.cpp` 等直接 `bsp_kout<< ...` → 命令回显/输出混入 dmesg ring（支撑 P1）。

### 2.5 具体实现缺陷（顺手清单，详见 §6）

- `register_backend` 用 `new`（`src/utils/kout.cpp:702`）→ 早期/panic 分配隐患，且后端非静态。
- `mask_backend`（`:722`）**语义反了**：只实现了 masked→unmasked。
- `unregister_backend`（`:713`）`delete` 后**不置空**槽位 → 悬垂 / 复用槽位时 double free。
- `tmp_buff` 满时**静默丢弃**（`src/utils/tmp_buff.cpp` 各 `operator<<` 满即 `return`），无 drop 计数。
- `defalut_KURD_module_interpator`（`src/utils/kout.cpp`）在 flush 持锁时回调 `bsp_kout` → ⚠️ 潜在自锁（需确认 spinlock 是否可重入）。
- `DmesgRingBuffer::putsk` 用 `spinrwlock_cpp_t`（`src/include/kcirclebufflogMgr.h`）→ 被当 always-writable 用时，IRQ 上下文**自旋死锁**隐患。

### 2.6 两套 kout 实现并存

`src/utils/kout.cpp`（新，backend 模型）与 `src/init/util/kout.cpp`（旧）各被 CMake 两个 target 编译（`CMakeLists.txt:150` 与 `:210`）。改一处易漏一处。

---

## 3. 目标架构（四层）

```
┌─────────────────────────────────────────────────────────────┐
│ 生产者（任意上下文）                                            │
│   LOG(lvl) << ...        // 建 Record（栈上，无线程安全）        │
└───────────────┬─────────────────────────────────────────────┘
                │  Record：{ level, entries[](自带 per-entry radix) }
                ▼
┌─────────────────────────────────────────────────────────────┐
│ 渲染器（唯一一份）：Record ──► 字节流                          │
│   含 KURD 解释、radix 渲染、now_time、endl                      │
└───────────────┬─────────────────────────────────────────────┘
                │  字节
        ┌───────┴────────┬────────────────────┐
        ▼                ▼                    ▼
  ┌───────────┐   ┌──────────────┐   ┌──────────────┐
  │ immediate │   │    ring      │   │   panic      │
  │  sinks    │   │ (原子安全)    │   │   sinks      │
  │ 直接写      │   └──────┬───────┘   │  always-alive│
  └───────────┘          │           └──────────────┘
                         ▼
                 ┌─────────────────┐
                 │ drainer kthread │  ← 可睡眠上下文
                 │  (creat_kthread)│
                 └────────┬────────┘
                          ▼
                 ┌─────────────────┐
                 │ deferred sinks  │  ← may-block：文件 / eth 慢路径
                 └─────────────────┘
```

### 3.1 Channel（面向调用者的 kout，按**角色**分，不按 stage）

| Channel | 面向 | sink 组成 | 备注 |
|---------|------|-----------|------|
| `log_kout` | 内核日志 | 随 init 里程碑增删（early 表→runtime 表） | **单一稳定对象**，无 current 指针 |
| `kshell_kout` | 交互 UI | GOP 文字控制台（`textconsole_GoP`） | 可能连 Record 都不走（需 UI 操作：清屏/定位/颜色） |
| `panic_kout` | 崩溃报告 | always-alive（fb / 预注册物理区段） | sink `min_level` 锁 0 |

> **决策点 D1**：确认"stage 下沉到 sink、channel 按角色分"取代"三阶段 kout 类 + `g_active_kout`"。

### 3.2 Sink（裸函数 ＋ 归属表）

- sink 接口保持**裸**：`void write(const char* buf, uint64_t len)`（可选 `putchar`）。
- **上下文契约由"注册进哪张表"决定**：

| 表 | 上下文契约 | 典型 sink |
|----|-----------|-----------|
| `immediate_sinks` | 任何上下文安全 | framebuffer、UART polling |
| `deferred_sinks` | 仅 drainer（可睡眠） | **文件日志**、ethernet 慢路径 |
| `panic_sinks` | always-alive、零分配零锁、min_level=0 | fb、预注册物理区段裸写 |

- `min_level` / `suppressed_count` 作为 **sink 条目字段**（不是函数签名的一部分）。
- 扩展方式＝**写一个符合签名的函数 + 注册进对应表**（直接满足 P4）。

> **决策点 D2**：sink 接口是否就定为裸 `write` ＋ 表承载属性？（备选：签名里带 `context_class` 参数）

### 3.3 上下文硬约束（P3 的落地）

- **atomic / IRQ / panic 上下文 → 只能落 immediate/panic 表。**
- **可阻塞 sink 永不直接被 producer 调用**，只由 drainer 喂。
- 结构上杜绝"中断里写文件"，不是靠纪律。

### 3.4 渲染器唯一

Record→字节的渲染（含 KURD 解释）**只实现一份**，被各 channel/sink 复用；channel 只做"薄适配 + 过滤"。避免"一个 god class 换成三个"。

### 3.5 等级过滤（编译期 ＋ 运行期两把工具）

- **编译期**：`#if` 砍死 TRACE/DEBUG，零成本、零二进制。
- **运行期**：per-sink `min_level` 阈值（`buff.level >= sink.min_level`）。
- 顶部宏短路：`if (lvl < g_min_level) return;` → 没人要就**连 Record 都不建**。
- `level_code` 现状：`INVALID=0 < INFO=1 < NOTICE=2 < WARNING=3 < ERROR=4 < FATAL=5`。
  ⚠️ INFO 以下**没有 DEBUG/TRACE**；若要让 dmesg/eth 比 console 更啰嗦，需补低端等级。

> **决策点 D3**：是否补 `TRACE < DEBUG`（低于 INFO）？是否引入编译期等级？

---

## 4. 与 SpDB 排期的关系

`Docs/Debug/spdb_roadmap.md` 要新增两个 sink：**runtime 以太网 logcat**、**panic 预注册物理区段裸写**。二者恰好落在 §3 的两端（deferred / panic），**本改造是 SpDB 的第 0 步**：先让 sink 模型就位，SpDB 只需"注册一个 sink"，不碰调用点。

---

## 5. 迁移计划（分步，每步可独立验证）

| 步 | 内容 | 可验证点 | 风险 |
|----|------|----------|------|
| **S0** | **冻结 kout 进制状态**：删 `curr_numer_system`/`shift_*`/`operator<<(numer_system_select)` 与数值重载，只留 `operator<<(tmp_buff&)`（过渡期留 shim：`bsp_kout<<X` 内部临时建 Record 再 flush，radix 来自 buffer） | panic 后不再"变 hex"；1188 处调用点语义不变 | shim 期间两套语义并存，须早迁 |
| **S0.5** | **修 §6 的即时 bug**（尤其 `DmesgRingBuffer` rwlock → 无锁/每核；register/mask/unregister） | IRQ 写 ring 不再死锁 | 改动面小，收益高 |
| **S1** | **拆 sink 归属表**：COM1 拆成 early/panic 的 polling sink ＋ runtime 的 ring sink；删除 `kout_backend` 的 5 指针，sink 收敛为 `write` | `uniform_puts` 的 stage 分支可删 | 需逐个迁移现有 2 个 register 点 |
| **S2** | **channel 化**：`log_kout` / `kshell_kout` / `panic_kout`；kshell 脱钩日志管线 | kshell 回显不再进 dmesg | kshell UI 操作需另走 GOP |
| **S3** | **ring ＋ drainer**：producer 一律进 ring；drainer 用 `creat_kthread` ＋ `kthread_sleep` | FS 未起时日志积压、起后落盘 | ring 背压 = 丢＋计数，绝不阻塞 producer |
| **S4** | **接 FS 文件 sink ＋ SpDB eth/panic sink**：写符合签名的函数注册即可 | 新 sink 零调用点改动 | — |

> **决策点 D4**：迁移顺序／是否先做 S0.5（低风险高收益）再进 S0。

---

## 6. 待修缺陷清单（可直接当 TODO）

1. `src/utils/kout.cpp:702` `register_backend` 用 `new` → 改静态分配（对齐 panic 零分配）。
2. `src/utils/kout.cpp:722` `mask_backend` 语义反了（只 unmask）。
3. `src/utils/kout.cpp:713` `unregister_backend` delete 后不置空 → 悬垂/double free。
4. `src/utils/tmp_buff.cpp` 满时静默丢 → 加 `dropped_count` ＋ 溢出标记。
5. `src/include/kcirclebufflogMgr.h` `DmesgRingBuffer` 用 rwlock → IRQ 死锁隐患，改无锁/每核。
6. `defalut_KURD_module_interpator` 在 flush 持锁时回调 `bsp_kout` → 核实自锁（spinlock 可重入性）。
7. 两套 kout 实现（`src/utils/` vs `src/init/util/`）→ 统一或明确边界。
8. `print_numer` 把格式化泄漏给 `running_stage_num` hook → sink 化后去掉 num hook。

---

## 7. 对齐点清单（**低带宽复核区**）

> 请直接在此圈点：✅ 同意 / ✏️ 改 / ❌ 否。带 ⚠️ 的是我建议你重点看。

- [ ] **D1** ⚠️ 用"channel 按角色分（log/kshell/panic）＋ stage 下沉到 sink"取代"三阶段 kout 类 ＋ `g_active_kout`"。
- [ ] **D2** sink 接口＝裸 `write` ＋ 归属表承载上下文契约／min_level。
- [ ] **D3a** 是否补 `TRACE/DEBUG`（低于 INFO）？
- [ ] **D3b** 是否引入编译期等级过滤？
- [ ] **D4** 迁移顺序：先 S0.5（修 bug）再 S0（冻结进制），还是并行？
- [ ] **D5** ⚠️ kshell 是否**完全**脱离 Record/渲染器（自带 UI 操作），还是仅独立 channel、仍复用渲染？
- [ ] **D6** panic 时是否把 dmesg 内容带进崩溃报告（读已写 ring，还是主动 dump）？
- [ ] **D7** drainer 的 ring：**每核无锁** 还是 **全局 irq-save 锁**？
- [ ] **D8** `log_kout` 的 sink 表在 init 里程碑的增删时机（哪些点 register/mask）。

---

## 8. 开放问题（待讨论）

- `PANIC_WILL_ANALYZE`(stage=2) 归哪个 channel？（现路由到 early_write）
- 渲染器如何同时服务"日志"（无控制序列）与"UI"（有控制序列）？是否需要 `UI` 专用 Record 类型？
- deferred sink 的**顺序性**：drainer 单线程即可保序，多 drainer 需分区。

---

*本文为 AI 草案，所有推断（⚠️）与决策点（D*）需设计方确认后升为 spec。*
