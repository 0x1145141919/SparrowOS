# bsp_kout 肢解与权力真空填补草案（原「输出栈重构草案」）

> 状态：**draft（待设计方确认）**
> 版本：**v6 —— 补「printk 接口契约 & level 标记」**（v5 `37761f8`，v4 `a0cb251`，v3 `3574cd5`，v2 `06ad24a`，v1 `0f077df`）
> 作者：Raven（AI 草案）
> 日期：2026-09-11
> 关联：`Docs/Debug/spdb_roadmap.md`、`Docs/Debug/debug_infra_decision_log_draft.md`、`Docs/kshell_framework_design.md`、`Docs/text_console_design.md`、`Docs/kout_tmp_buff.md`、`Docs/KERNEL_DISCIPLINE.md`
> 标注约定：事实带 `file:line`；推断打 ⚠️；待拍板汇总在 **§10 对齐点清单**。

---

## 0. 性质变更（v4 → v5）

v4 及以前的所有版本，前提都是「**bsp_kout 要保留，重构它内部的流式输出栈**」（Record/Sink/Channel/Drainer 四层、场景 kout、静态后端编排）。

v5 的结论相反：**bsp_kout 不要了**。任务据此重述为两步：

1. **肢解** —— bsp_kout 越权接下的多摊活，逐一判归其主；
2. **填补权力真空** —— 它撤走后，被它顺手占着的「终端设备」「日志路由」「early/panic 直写」出现空位，必须有人接手。

促成这次转折的两个认识：

- **(a) 能力翻转**：C++ `operator<<` 方案（把格式解决放到**编译期**）本质是 **bsp_kout 时代 agent 太弱时的拐杖**——不必写运行时 formatter。如今 agent 足够强，**printk（运行时格式）全面胜出**：一次函数调用 = 一条语义完整语句，每个数码格式**当次无状态**指定；丢失的编译期类型安全由 `__attribute__((format(printf,…)))` 找回。→ **v4 的箭二/箭三随 C++ 层一起作废**。
- **(b) Linux 模型足够简**：dmesg/printk = **纯内存 ring + 读侧过滤 + 拉取**；panic/落盘有 `kmsg_dump` + `pstore/zone`（定长记录 + magic + counter + 回绕 + 开机恢复）现成范式可抄。

---

## 1. 根因（修正版）

- 历史：`bsp_kout` 是**前调度器时代（2025-08）为「非中断轮询串口」写的**（提交 `b6b6751`），比调度器（`7072c3f`, 2026-02-11）早约 **5.5 个月 / 54 提交**。
- 但真正的病不在"历史"本身，而在**越权**：它把三种**正交**的关注点焊死在一个类里——
  - **A 格式化**（数据 → 文本）
  - **B 传输 / 持久化**（文本 → 目的地）
  - **C 交互终端控制**（设备状态：光标 / 清屏 / echo）
- **量化**（全树审计，`bsp_kout` 调用点 **1188 处 / ~55 文件**）：

| 用途分桶 | 调用点 | 占比 |
|---|---|---|
| **kshell / UI（越权最重）** | 524 | **44%** |
| panic | 100 | 8% |
| tests | 74 | 6% |
| 其余内核代码（drivers / boot / memory / init） | 490 | 41% |

→ **kshell 一家吃掉 44%**：越权不是零星，是主干。

---

## 2. 肢解：bsp_kout 名下的 7 摊活与归属

| # | 现活 | 现状证据 | 归属 |
|---|------|----------|------|
| 1 | 格式化引擎（`operator<<` 家族 / `print_numer` / KURD 解释 / `now_time`） | `src/include/util/kout.h`；`src/utils/kout.cpp:164-260,676` | **printk formatter**（KURD → `kurd_str()`；时间戳由 ring 提供） |
| 2 | 进制状态机（`shift_*` + `curr_numer_system`） | `src/include/util/kout.h:47`；约 51 处调用 | **删除**（format 串取代） |
| 3 | 路由 / 分发（`register/mask/unregister_backend` + `uniform_puts` 按 `GlobalKernelStatus` 选 stage + `backends[]`） | `src/include/util/kout.h`；`src/utils/kout.cpp` `uniform_puts` | **删除**（路由下沉到读者） |
| 4 | 传输后端（uart ring / textConsole ring / dmesg / USER_MODE） | `src/arch/x86_64/core_hardwares/x86_arch/PortDriver.cpp:199-203`；`src/utils/kout.cpp:559-569` | **reader / dumper**（见 §3、§4） |
| 5 | 交互终端 UI（行编辑 / `\b` / `\a` / 清行 / 命令输出） | `src/utils/kshell.cpp:24-45,691-706`；`src/arch/x86_64/core_hardwares/i8042/i8042_kshell.cpp:397,625,701` | **kshell 框架**（独占屏幕 + 键盘） |
| 6 | 统计 `statistics` | `src/utils/kout.cpp:670`（仅自读） | **并入 printk/ring 计数，或删** |
| 7 | USER_MODE stdout/stderr | `src/utils/kout.cpp` 11 处 `write(1/2)` | host 侧 printk stub |

---

## 3. 权力真空与填补

bsp_kout 撤走后，留下 **3 个真空 + 1 个缺口**：

- **真空①：终端设备（屏幕 + 键盘）无人持有。** 今天它被 bsp_kout 的 sink 隐式占用（uart ring + textConsole ring）。→ **kshell 框架接管**，且 **runtime 独占**（见下方三段所有权）。
- **真空②：日志路由 / 分发无人做。** → **不做**：printk 只认 ring；分发下沉到"读者注册表"（推式：UART / eth / dump；拉式：kshell 新命令）。
- **真空③：early / panic 的直写通道。** 今天靠 backend 的 `early_write` / `panic_write`（`PortDriver.cpp` 的 `polling_puts`）。→ **统一 printk 接口 + 早期降级实现**（UART polling）；panic 走 dumper。
- **缺口：ring 没有读 API。** `src/utils/kcirclebufflogMgr.cpp` 只有 `Init` / `putsk`，**没有任何 reader**（本次审计确认）→ 必须补：**记录头 `{len, level, seq, ts}` + 读 API + 每读者 cursor**。

**屏幕所有权按阶段切三段**（"kshell 独占"只在 runtime 成立）：

```
early boot ：boot 直驱 textconsole_GoP（kshell 未起）→ UART polling 兜底
runtime    ：kshell 框架【独占】屏幕 + 键盘
panic      ：panic 路径直驱（kshell 不在）
```

---

## 4. 目标架构（两个接盘侠 + 两个共享设施）

```
printk(fmt, ...)  ──►  dmesg ring（纯内存，全阶段统一，记录头 {len, level, seq, ts}）
                          │
                          ├─ 拉取 reader ──► kshell 新命令（如 dmesg / log）
                          ├─ 推式 reader ──► UART drainer（串口调试）
                          ├─ 推式 reader ──► eth logcat（SpDB）
                          └─ dumper      ──► chunked store（panic / 落盘）

textconsole_GoP（物理渲染原语：PutChar / Clear / 光标）
   ├─ kshell 框架（runtime：独占屏 + 键盘；编辑态 + 历史 + transcript）
   ├─ early 直驱
   └─ panic 直驱

kshell transcript ──► chunked store（与 panic dump 共用同一引擎）
```

- **接盘侠①：dmesg ring** —— 纯内存、全阶段统一、写侧全收、**读侧过滤**（level 只在读者生效）。
- **接盘侠②：kshell 框架** —— 独占 runtime 屏幕 + 键盘；行编辑 + 光标 + 历史。
- **共享设施 A：`textconsole_GoP`** —— 物理渲染原语，被 kshell / early / panic 三方共用，**不归任何一方**（现成：`src/include/util/textConsole.h` 网格模型；`src/arch/x86_64/boot/kinit.cpp:148`、`exec_env_prepare.cpp:177-180`）。
- **共享设施 B：chunked record store** —— 定长记录 + magic + counter + 回绕 + 开机恢复；**kshell transcript 与 panic dump 共用**（**别造两套引擎**）。

---

## 5. printk 接口契约 & level 标记（v6 新增）

### 5.1 签名（显式 level：偏离 Linux **编码**、同 Linux **语义**）

- 核心（唯一 formatter 入口）：`void vprintk(log_level lvl, const char* fmt, va_list args)`
- 面层：`printk(log_level lvl, const char* fmt, ...)` → `__printf(2, 3)` 保类型安全
- 宏层（调用点手感对齐 Linux）：
  ```cpp
  #define pr_info(fmt, ...)  printk(log_level::INFO, fmt, ##__VA_ARGS__)
  #define pr_err(fmt, ...)   printk(log_level::ERR,  fmt, ##__VA_ARGS__)
  ```

**与 Linux 的取舍**：Linux 把 level 焊进格式串前缀（`"\0016..."`，arg0 仍是字符串）；我们**用显式 arg0**。差异只在**编码**——

| | 编码 | 优点 | 缺点 |
|---|---|---|---|
| Linux 式 | `KERN_INFO "..."` = `"\0016..."` 焊串 | 单变参、生态熟 | `\001` **魔法字节**（源码隐形、易漏）；**每条运行时解析前缀**（`printk_parse_prefix`）；忘写即静默降级 |
| **本方案** | `printk(LOG_INFO, "...", ...)` | 无魔法字节、**可 grep**、level 是**编译期常量**、**免前缀解析** | 多一个参数（可忽略） |

**语义完全对齐 Linux**：level **存进记录**（`{len, level, seq, ts}`）、**写侧全收**、**读侧过滤**。

### 5.2 level 枚举（复用现有，钉死方向）

- 直接复用 `src/include/abi/os_error_definitions.h:86` 的 `level_code`：`INVALID=0 < INFO=1 < NOTICE=2 < WARNING=3 < ERROR=4 < FATAL=5`。
- ⚠️ 方向**与 Linux 相反**（Linux 0=EMERG 最严重；我们 5=FATAL 最严重）→ 阈值判定**统一写 `level >= threshold` 才输出**（沿用 v4 §3.3 的约定），文档钉死。
- ⚠️ 现枚举**无低于 INFO 的 `DEBUG/TRACE`**；若要把高频调试赶出 ring，需补低端等级（见 5.4）。

### 5.3 early / runtime / panic 的唯一化

- **一个 formatter、一个核心签名**（early / runtime / panic 共用）。**禁止**为 early 分叉独立 API 或独立格式化代码——那正是"两套 kout"（`src/utils/` vs `src/init/util/`）的老账。
- `early_printk(fmt, ...)` 只是**薄包装**：
  ```cpp
  #define early_printk(fmt, ...) printk(log_level::INFO, fmt, ##__VA_ARGS__)
  ```
  - **early 全部 INFO**（设计方 2026-09-11 定）。
  - 传输顺序：**先写 dmesg ring，再发配屏幕 + polling UART**（三处各一份；早期三处均为 immediate）。
  - ⚠️ ring 未就绪窗口：`DmesgRingBuffer::Init`（`exec_env_prepare.cpp:160`）之前 ring 不存在 → `if (ring_ready) 写 ring;` 再发屏 + UART（降级）。
- **panic 走同一变参面**（对齐 Linux），但受三条约束：
  1. formatter 与 runtime **共享**——**无锁、无分配、无 `%f`**；
  2. panic 传输 = **dumper**（QR + 全量内存转储是 dumper 的活，见 `Docs/Panic/panic_qr_dmesg_dump_draft.md` / `panic_nvme_dram_dump_draft.md`）；
  3. ⚠️ panic 时 ring 可能已回绕覆盖头部 → dumper 接受头部丢失，或双区 / 冻结。

### 5.4 写侧全收 vs 高频调试（能力保留）

- 默认**写侧全收**（除 panic 抑制），过滤只在读侧。
- 编译期常量红利：`if constexpr (lvl >= kMinStored) return;` 的**零成本门控能力保留**，但**默认不开**；仅当"TRACE 确实不该进 ring"时启用（对应 Linux `dynamic_debug`）。

---

## 6. kshell 缓冲的三分（关键：只有 transcript 落盘）

kshell 现在**没有任何输出缓冲**，输出直灌 bsp_kout（`src/utils/kshell.cpp`）。它已有的状态只有 `line_editor_t{line,cap,len,cursor}` + `history_pool[64][256]`（`src/utils/kshell.cpp:18-58`）。所谓"独立缓冲区"要拆成三层语义：

| 层 | 语义 | 落盘 |
|---|------|------|
| **编辑态**（行 / 光标） | 实时 UI，可回退可擦（是"状态"不是"序列"） | ❌ |
| **历史**（命令） | 小、固定 64×256 | 随 transcript |
| **transcript**（会话流水） | **只追加**（`提示符 + 定稿命令 + 输出`） | ✅ **chunked store 的对象** |

- **渲染复用 `textconsole_GoP`**（网格 / 光标 / 清屏），kshell **不自造网格**、也**不走 dmesg**。
- ⚠️ **编辑过程不进 transcript，只有定稿行进** —— 这正好让 transcript 成为干净的可持久化序列。

---

## 7. 与 SpDB 排期的关系

- `Docs/Debug/spdb_roadmap.md`：**SpDB（e1000e）为唯一调试基座**（`Docs/Debug/debug_infra_decision_log_draft.md` 已确认，xDCI 否决）。
- 本方案下，SpDB 的 **eth logcat = dmesg ring 的一个推式 reader**；**panic 物理区段落盘 = dumper**。→ **本改造成 SpDB 第 0 步**：模型就位后，SpDB 只需"注册一个 reader / dumper"。

---

## 8. 迁移计划

| 步 | 内容 | 可验证点 |
|----|------|----------|
| **S0** | 修即时缺陷（§9 保留项）；补 **ring 读 API + 记录头** | ring 可被读；IRQ 写不死锁 |
| **S1** | **printk 接口落地**（§5 契约：显式 level arg + 宏层 + 唯一 formatter）；驱动 / boot / memory 调用点迁移 | 输出等价、状态消失 |
| **S2** | dmesg ring **全阶段写**（含 runtime）＋ 记录头 + cursor | runtime 也能 dmesg |
| **S3** | kshell 框架接管屏幕 + 键盘（直连 `textconsole_GoP`）＋ 新增 `dmesg` 命令 | kshell 脱离日志管线；日志可查 |
| **S4** | chunked store；接 kshell transcript + panic dump | 崩溃后能恢复 |
| **S5** | **删除 bsp_kout**（两套实现）＋ `backends` / `uniform_puts` / `shift_*` / `tmp_buff` 收尾 | 调用点清零 |

---

## 9. 缺陷清单（标注：随肢解自动消失 / 需保留处理）

| # | 缺陷 | 去向 |
|---|------|------|
| 1 | `register_backend` 用 `new` | **随 #3 删除消失** |
| 2 | `mask_backend` 语义反了（只 unmask） | **随 #3 消失** |
| 3 | `unregister_backend` delete 不置空 → 悬垂 / double free | **随 #3 消失** |
| 4 | `tmp_buff` 满时静默丢 | **随 tmp_buff 删除消失**（或并入 printk 计数） |
| 5 | `DmesgRingBuffer` 用 `spinrwlock_cpp_t` | **需保留处理**（改无锁 / irq-save / 每核） |
| 6 | `defalut_KURD_module_interpator` 在 flush 持锁回调 `bsp_kout` → 自锁隐患 | **随 `uniform_puts`/backends 消失** |
| 7 | 两套 kout 实现（`src/utils/` vs `src/init/util/`） | **随肢解消失** |
| 8 | `print_numer` 把格式化泄漏给 `running_stage_num` hook | **消失**（printk 自带 formatter） |

---

## 10. 对齐点清单（**低带宽复核区**）

> 请直接圈点：✅ 同意 / ✏️ 改 / ❌ 否。

- [ ] **D0** 归因 = **越权（三关注点焊死）+ agent 能力翻转**（"历史债务"叙事降为背景）
- [ ] **D1** **bsp_kout 删除**（非重构）
- [ ] **D2** printk 对齐 Linux：纯内存 / 全阶段统一 / 写侧全收 / **读侧过滤** / 拉取
- [ ] **D3** kshell 框架**接管屏幕 + 键盘**，runtime 独占
- [ ] **D4** kshell **常驻**（避免"退出后屏幕无主"的边界）
- [ ] **D5** transcript = `提示符 + 定稿命令 + 输出`；**只有它落盘**
- [ ] **D6** chunked store **共用引擎**（kshell transcript + panic dump）
- [ ] **D7** 访问内核日志 = **kshell 新命令**（如 `dmesg`）
- [ ] **D8** serial **不给** kshell（只给 printk）
- [ ] **D9** ring 记录头 `{len, level, seq, ts}` + 读 API + 每读者 cursor
- [ ] **D10** early / panic 直驱 + 三段所有权交接点
- [ ] **D11** printk 签名 = **显式 level arg0 + 宏层**（偏离 Linux 编码、同 Linux 语义）
- [ ] **D12** **early 全部 INFO**；`early_printk` = 薄包装（**不造第二个 formatter/API**）
- [ ] **D13** formatter **唯一**（early/runtime/panic 共用）；panic 走同变参面
- [ ] **D14** level 枚举**复用** `os_error_definitions.h`；阈值方向 `level >= threshold`

---

## 11. 开放问题

- ⚠️ 仓内已有**未跟踪** `src/include/util/kstream.h`：探索"backend 按上下文重分类"（`udp_style` / `syn_thread` / `syn_mechain`）。**与本方案的"删路由"方向互斥** → 需设计方裁定**保留 / 废弃**。
- `PANIC_WILL_ANALYZE`(stage=2) 归属？（现路由到 `early_write`）
- KURD 呈现：独立 `kurd_str()` vs printk 格式指令（`%K`）
- ring 锁策略：全局 irq-save vs 每核无锁
- early 屏幕 → kshell 的**具体交接点**（`DmesgRingBuffer::Init` @ `exec_env_prepare.cpp:160` 之后、kshell 起之前，谁驱屏？）

---

*v6 由 AI 按设计方 2026-09-11 论点重构（v5：肢解 + 权力真空；v6：补 printk 接口契约——显式 level arg + 宏层 + early=INFO + panic 变参）。推断（⚠️）与决策点（D*）需设计方确认后升为 spec。*
