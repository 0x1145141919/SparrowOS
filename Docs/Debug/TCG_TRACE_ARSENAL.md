# TCG_TRACE_ARSENAL —— TCG+trace 抓取武器库

> **定位**：SparrowOS 的**通用**内核崩溃取证能力（与本仓库某个具体 bug **解耦**）。
> 任何"随机/竞态/风暴"类问题都先来这里取武器。具体 bug 的战报另开文档
> （如 `Docs/Debug/WRAITH.md`），只引用本文档，不复制武器库。
> **建立**：2026-09-15。**配套代码**：`Tools/tcg-trace/`。

> ## ⚠️ 权威声明（强一致 · 交接用）
> 本文档是 TCG+trace 取证工具集的 **唯一权威**。`Tools/tcg-trace/README.md` 只是
> 同目录的**薄壳速查**（一两行用法 + 回指本文），**不复述细节**。
> **纪律：改工具 ⇒ 同一回合改本文**；两处若冲突，以本文为准并立刻修正薄壳。
> **最后对齐**：2026-09-17（对齐 `Tools/tcg-trace/` 7 件工具；**老式 `.ram`/pmemsave 路径已铲除**）。

---

## 0. 一句话

让 AI（或任何人）在**不依赖会话记忆、不依赖人肉复现**的前提下，一把拿到一个**真实竞态崩溃样本**
（fault 前后的指令流 / 异常 / 寄存器 / **最终内存镜像**），然后离线符号化定位。

---

## 1. 为什么是 TCG（关键前提）

- 目标崩溃是**随机 SMP 竞态**：`SMP=2` 干净、`SMP=6` 才显形。
- **KVM 会把竞态"压没"，或把失败形态改写**（panic ↔ hang）；**实测 KVM 下本类 bug 概率远小于 TCG**。
- → **TCG（`-cpu max`，软件模拟）是复现档位**；TCG 的时序异于 KVM，恰恰是宽竞态窗口的来源。
- ⚠️ **TCG 竞态显形率对宿主调度抖动极敏感**：空载可能 0/60，注入宿主负载后 2/20。
  → 复现时要**控制/注入宿主负载**（见 §6）。

---

## 2. 三件套

1. **TCG（`-cpu max`）** —— 复现竞态/死锁的档位。
2. **trace 类别 `in_asm,int,guest_errors,unimp,cpu_reset,pcall`**
   - `in_asm`：翻译即记、只记唯一 TB → 体积可控（正常 ~27MB/次）；
   - `int`：异常/中断 + **完整寄存器 dump**（CR2/CR3/GS/GDT/TR）→ 事故现场本体；
   - ⛔ **禁用 `-d cpu`**：每 TB 寄存器 dump ≈ 330MB/s，一次 boot 几十 GB。
3. **体积帽**：故障风暴（异常自旋）会把 `-D` 写到十几 GB；对 trace 文件设帽（默认 3GB），超帽即杀判 `SIZECAP`。

---

## 3. 工具矩阵（`Tools/tcg-trace/`）

**共 7 件。角色与状态一览：**

| 工具 | 角色 |
|---|---|
| `tcg-trace.sh` | 抓取总控：单次/连跑 · 停止判据 · 转储编排 |
| `qmp-dump-vmcore.py` | 官方 vmcore（QMP `dump-guest-memory`）→ `.vmcore`(+`.regs`) |
| `vmcore-mkcore.py` | `.vmcore` → **虚拟视角** `.core`（GDB 可读） |
| `trace-sym.py` | `.trace` → 符号 + **源码行批注**日志 |
| `qmp-wait-stop.py` | QMP `STOP` 事件常驻监听（零轮询开销） |
| `qmp-status.py` | 一次性 `query-status`（供 `--selfcheck`） |
| `patches/*` | QEMU ioport80 魔法断点补丁 + 部署纪律 |

> 内存转储**只有一条路**：官方 `dump-guest-memory`（§3.2）。旧 `pmemsave`（`qmp-memdump.py`
> → `.ram`，及 `ram-read.py`/`ram-mkcore.py`）**已于 2026-09-17 整体铲除**——那是带 30s 超时硬伤的
> 遗留路径，留着只会污染接手者的上下文。

### 3.1 `tcg-trace.sh` —— 抓取总控

复用 VMtest-nosudo 的 mtools 零-sudo ESP staging + QEMU 参数；叠加 `-D/-d trace`；
轮询串口，命中停止判据 / 超时 / 超帽即停；`--repeat` 连跑到抓到异常样本。

| 参数 | 说明 |
|---|---|
| `--outdir DIR` | 输出目录（默认 `$SPARROW_DEBUG_STORE/traces`，store 缺省 `/mnt/huge_data/sparrowos_debug`；父目录不存在则回退 `$BASE/VMresources/traces`）|
| `--tag NAME` | 样本名（默认 `trace`）|
| `--timeout SEC` | 单次墙钟上限（默认 90，超时判 `HANG`）|
| `--cap-gb N` | trace 体积帽（默认 3GB，超帽判 `SIZECAP`）|
| `--stop-on RE` | 串口命中即停正则（**默认 `PANIC\|kshell>\|#WF#`**）|
| `--repeat N` | 连跑 N 次（样本名补零 `tag01…`），抓到**任一异常样本**即停 |
| `--base DIR` | 仓库布局根（默认 `/home/PS/PS_git/OS_pj_uefi`）|
| `--dump-always` | 连正常样本也转储（基线用）|
| `--dump-fault-only` | 仅对 guest 首爆冻结（`RESULT=FAULT`）转储（避开 AP 类噪声 panic）|
| `--skip-noise-re RE` | 串口命中该正则的样本判 `NOISE`：**不转储、不算异常、连跑继续**（用于 12× 等高压下顶出的 AP 启动 IPI 超时等假阳性）|
| `--dump-vmcore` | 异常停止时抓官方 vmcore → `<tag>.vmcore`(+`.vmcore.regs`) |
| `--mkcore` | 在 vmcore 基础上合成虚拟视角 core → `<tag>.core`(+`.map-report.txt`)（隐含 `--dump-vmcore`）|
| `--selfcheck` | 只校验 `QEMU_BIN` 是否带 ioport80 魔法断点补丁（不启动 VM）；`SELFCHECK=PASS/FAIL` |
| `-h\|--help` | 用法 |

**环境变量**：`SMP`（默认 6）· `BASE`（同 `--base`）· `SPARROW_DEBUG_STORE`（大工件仓根）·
`QEMU_BIN`（默认 PATH 里的 `qemu-system-x86_64`；⚠️ 跑取证请显式指向打过
`patches/qemu-ioport80-magic-bp.patch` 的构建，并用 `--selfcheck` 自检）· `VMCORE_TIMEOUT`（默认 900s）。

**停止判据（任一命中即停）**：串口命中 `--stop-on` / 串口 `#WF#`（独立于 `--stop-on`）/
QMP 观测到 `STOP`（guest 冻结，经 `qmp-wait-stop.py`）/ 超时 / trace 超帽。
对应 `REASON` = `MATCH` / `MAGICBP` / `MAGICBP` / `TIMEOUT` / `SIZECAP`。

**`RESULT` 分类**：`KSHELL`（正常到提示符）· `PANIC` · `FAULT`（guest 首爆即冻结，见 §3.4）·
`HANG`（无 panic 无 kshell）· `SIZECAP`（风暴到帽）· `NOISE`（命中 `--skip-noise-re`，非目标）· `OTHER`。

**转储门控**（决定哪种结局才转储）：`--dump-always` ⇒ 一律；
`--dump-fault-only` ⇒ 仅 `FAULT`；否则 ⇒ 除 `KSHELL/OTHER/NOISE` 外都转。
⚠️ `-qmp` 只在 **`--dump-vmcore`** 时才挂（避免给非转储轮多挂一个 chardev 扰动 TCG 交织）。

**产出**：`<tag>.trace`（QEMU `-D`）· `<tag>.serial`（串口）· 视选项另有
`<tag>.vmcore`(+`.regs`) / `<tag>.core`(+`.map-report.txt`)。
末行结构化输出：`TAG= RESULT= REASON= ELAPSED= TRACE= LINES= SERIAL= VMCORE= CORE=`。
**退出码**：`0` = 正常（KSHELL/OTHER）；`10` = 抓到异常样本（PANIC/FAULT/HANG/SIZECAP）。

### 3.2 `qmp-dump-vmcore.py` —— 官方 vmcore（唯一内存路径）

```
qmp-dump-vmcore.py <qmp-sock> <outfile> [--paging] [--begin A --length L]
                   [--timeout SEC] [--no-quit]
```
经 QMP `dump-guest-memory{paging, protocol=file:<out>, format=elf, detach:true}`，
轮询 `query-dump` 到 `completed`（默认上限 900s，超时删残件）。
- **产物**：`<outfile>`（标准 ELF：每 RAM 段一个 `PT_LOAD`，**只枚举真实 RAM、洞跳过**；
  全 CPU `NT_PRSTATUS` → GDB 直接 `info threads/registers`）+ `<outfile>.regs`（HMP
  `info registers -a`：全 CPU 段基址等）。
- **语义**：`dump_init` 自带 `vm_stop(SAVE_VM)`、结束 `vm_start` ⇒ **不用先 stop**；
  `detach=true` 让 dump 在独立线程跑、QMP 立即返回。
- **默认非 paging**（`p_paddr=物理`，`p_vaddr` 回落物理）。⚠️ `--paging` 会把
  `phyaddr_window`（10G 别名）一并落盘 → 体积暴涨，慎用。
- 结束时**校验文件 size**（大镜像不再有假失败）。
- 退出码：`0` 成功 · `2` dump 失败/超时（已删残件）· `3` 连接/协议失败 · `64` 用法错误。

### 3.3 后处理：把 raw 工件变成"可读"

#### 3.3.1 `vmcore-mkcore.py` —— `.vmcore` → 虚拟视角 `.core`
```
vmcore-mkcore.py <vmcore> <serial> [--kernel KERNEL_ELF] [--out CORE]
                 [--pdpt-phys 0x..] [--skip-alias] [--with-phys]
                 [--report FILE] [--baseline FILE] [--max-suspicious N]
```
为什么：官方 vmcore 是**物理**视角，GDB 按虚拟地址读不到内核栈/堆。本工具以
**`kspace_up_half`**（高半 128TiB 的**扁平 PDPTE 表**，`index=(v-0xFFFF800000000000)>>30`，
17 位）为**单一根**走 `PDPTE→PD→PT`，把虚拟→物理写成 `PT_LOAD`
（`p_vaddr=虚拟 / p_offset=PA_BASE+物理`）；NOTE 段原样搬运。
- **文件布局**：`[0,PA_BASE)` = ehdr/段表/NOTE；`[PA_BASE,..)` = **物理内存窗口**
  （稀疏；所有段——内核虚拟视图 + 恒等窗口——共用这一个物理窗口）。
  `PA_BASE = 对齐4K(note_end) ≥ 0x1000`。
- **收页判据（两信号取交）**：`P=1 且 phys∈vmcore RAM 段 且 缓存∉{UC,UC-}`（PAT idx 2/3/6）。
  排设备 MMIO（HPET/IOMMU/ECAM/NVMe BAR 全 UC）；保留 WC 帧缓冲；**切忌"==WB"一刀切**。
- **恒等窗口默认保留**（很多资产如 `fpa_bitmaps`/`pages_arr` 经 `phyaddr_window` 恒等别名访问）
  ⇒ `.core` 会胀到≈全 RAM；`--skip-alias` 丢弃巨别名换瘦身（~20MB）。可由 `.vmcore` 随时重清洗。
- 越界/不可信页表项 → `--report`（**WRAITH 探针**：树↔页表分歧/野帧）；`--baseline` 喂一份
  健康态 report，则其中的非 RAM 映射视为预期、不再报异常；`--max-suspicious` 默认 40。
- **输出**默认 `<vmcore>.core` ⇒ `gdb kernel.elf <out>`。

#### 3.3.2 `trace-sym.py` —— `.trace` → 符号 + 源码批注
```
trace-sym.py <trace> [--init init.elf] [--kernel kernel.elf]
             [--start auto|init|kernel|<line>] [--only-domain init|kernel]
             [--regs none|exc|all] [--short-loc] [--src]
             [--out FILE] [--summary-only]
```
裸 `.trace` 是 `-d in_asm` 裸汇编，且 init.elf / kernel.elf / UEFI 三段地址混排；本工具按
**链接域**把指令行、事件行解析成 ``<sym+off @ 源文件:行>``（DWARF 由 `addr2line` 提供）：
- **链接域**：init.elf @`0x101000000`（宽松上界 `0x102000000`）；
  kernel.elf @`0xffff800000000000`（含低半区 `0x4000–0x9000`）。
- 每行输出 = `<sym+off @ src:line> | <原始未解析行>`（原始行逐字保留，便于对照）。
- 立即数（含 large model 的 `movabsq $<十进制巨值>`）与 RIP-相对（`disp(%rip)`）里的地址也解析，
  尾注 `; addr[...]`；含护栏（无 DWARF 覆盖 / 偏移过大 → 判为非常量，不解析）。
- `--src`：在指令流里插 **C++ 源码行批注**（`── file:line [func]` + 该行源码；`.asm/.s` 自动跳过）。
- **输出**默认 `<trace>.sym.log`；`--summary-only` 只打印符号化后的**异常事件时间线**到 stdout。
- ⚠️ 事件行的 `#N` 是 QEMU **全局事件序号**，**不是 CPU id**。
- 链接域只有 **init / kernel** 两个（`--only-domain` 亦仅此二者）；其余（固件/loader）地址归 `other`，**不批注**。

### 3.4 首爆冻结：魔法断点 + `qmp-wait-stop.py` / `qmp-status.py`

**问题**：风暴（异常自噬）在 host 侧 0.2s 轮询粒度下已经写了几百 MB；且 panic/风暴
会把现场毁掉。要"以首爆速度"止损并保住全核快照，必须在 **guest 侧**自己停。

**机制**（两层，默认安全）：
1. **guest 侧钩子**：`src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm` 的
   `FAULT_FREEZE` 宏，挂在 `#PF`(`0x0E`)/`#GP`(`0x0D`) **入口**（post-fault，零观测效应）。
   默认关闭，取消文件内 `;%define SPDB_FAULT_FREEZE` 注释即启用（或 `nasm -DSPDB_FAULT_FREEZE`）。
   动作：串口打 `#WF#` → `outb(0x80,0xDB)` → `cli;hlt` 兜底。
   （⚠️ 默认关闭 ⇒ 跑取证**必须显式开启**。）
2. **QEMU 本地补丁**：`ioport80_write` 见 `0xDB` → `vm_stop(RUN_STATE_DEBUG)`：
   **暂停整机、进程不退出** → QMP 仍可用 → 可 dump。补丁/基线/构建/部署纪律见
   **`Tools/tcg-trace/patches/README.md`**（含"别 cp 进 `/usr/bin`，会被 pacman 覆盖"血泪）。

**host 侧停止判据**（任一命中即停，命中后按 `want_dump` 门控转储）：
- 串口 `#WF#`（零轮询开销，首选）；
- `qmp-wait-stop.py <sock> <flag> <timeout>` 常驻监听 QMP `STOP`/
  `RESET`/`SHUTDOWN`/`GUEST_PANICKED`/`POWERDOWN` 事件（或连接时已 `paused`/`debug`），
  命中即 `touch <flag>` —— **不要**每 0.2s 起 python 去 `query-status`（那是宿主负载，
  而宿主负载是竞态复现的关键变量）。
- `qmp-status.py <sock>`：一次性 `query-status` 打印状态字符串（供 `--selfcheck`）。

**自检配方**：`QEMU_BIN=<补丁构建> ./tcg-trace.sh --selfcheck` → 必须 `PASS`。

### 3.5 产出文件一览

| 文件 | 生产者 | 何时有 | 内容 |
|---|---|---|---|
| `<tag>.trace` | QEMU `-D` | 总是 | in_asm 反汇编 + int 事件 + 寄存器 dump |
| `<tag>.serial` | QEMU `-serial` | 总是 | 串口（结局/panic/KURD/栈回溯）|
| `<tag>.vmcore` | `qmp-dump-vmcore.py` | `--dump-vmcore`/`--mkcore` 且门控命中 | 官方 ELF（RAM 段 + 全 CPU NT_PRSTATUS）|
| `<tag>.vmcore.regs` | 同上 | 同上 | HMP `info registers -a`（全 CPU 段基址）|
| `<tag>.core` | `vmcore-mkcore.py`（`--mkcore`）| 有 `.vmcore` 时 | 虚拟视角 GDB core（稀疏，物理窗口）|
| `<tag>.map-report.txt` | 同上（`--report`）| 有 `.vmcore` 时 | 可疑页表项清单（WRAITH 探针）|

---

## 4. 输出布局与生命周期（大工件仓）

**约定变量**：`SPARROW_DEBUG_STORE`（默认 `/mnt/huge_data/sparrowos_debug`）。
工具读它 → **仓库里不写死本机路径**（换机器/克隆只改环境变量）。

```
$SPARROW_DEBUG_STORE/            ← 默认 /mnt/huge_data/sparrowos_debug（独立 1.9T btrfs）
├── README.md    布局说明
├── INDEX.md     样本台账（tag → 结局 → 结论 → 保留否）—— 机器本地活索引
├── traces/      <tag>.trace / <tag>.serial / <tag>.vmcore(+.regs) / <tag>.core(+.map-report.txt)
├── system_log/  系统日志
└── memdump/     独立/离线内存转储
```

**三层生命周期**（分开管，别混）：

| 层 | 内容 | 去处 | 周期 |
|---|---|---|---|
| A | 战报 / 武器库 / 结论 | **git 仓库** `Docs/Debug/` | 永久 |
| B | 样本台账 | store `INDEX.md` | 随样本，定期归档 |
| C | 原始 `.trace/.vmcore/.core/.serial` | store `traces/` | **可弃**（可再生）|

**纪律**：狩猎只留 `PANIC/FAULT` 的 `trace+vmcore`，其余即删；`.vmcore`（8G/个）结论提取后
优先删；风暴 `.trace`（GB 级）用后即删。**结论留存，镜像可弃**。

---

## 5. 离线定位流程

0. **看结局**：`grep '^TAG=' <log>`（**别用 `tail -1`**，见 §8）。
1. **看 serial**：结局 + `PANIC:` + `[KURD]` + 栈回溯（`#N RIP Symbol`）。
2. **精读 trace**：`trace-sym.py <tag>.trace --start kernel --only-domain kernel --src --out <tag>.kernel.src.log`
   —— 得到「符号+偏移 @ 源文件:行 + C++ 源码行」的可读流。
   原始定位：在 `.trace` 找首个异常事件（`^\s*\d+: v=` 行），其前一条 `IN:` 块即肇事 TB 反汇编。
3. **反查 RIP**：运行基址 `0xFFFF800000000000` 与 ELF vaddr 一一对应 →
   `addr2line -f -C -e kernel.elf <addr>` 直接命中 `file:line`。
4. **登记事故现场**：读寄存器 dump（`CR2/CR3/GS/GDT/TR/CS`）判损坏类型：野指针 / GS 被清 /
   页表 / 控制流劫持。
5. **落地到内存**：`vmcore-mkcore.py <tag>.vmcore <tag>.serial --kernel kernel.elf --out <tag>.core --report <tag>.map-report.txt`
   → `gdb kernel.elf <tag>.core`（`info threads` / `bt` / `x` 全活）；`--map-report` 给出树↔页表分歧。
6. **计数异常向量**区分形态：`grep -oE 'v=[0-9a-fA-F]+ ' <tag>.trace | sort | uniq -c` →
   `#PF`/`#GP`/`#DF` 风暴 vs 单点。

⚠️ **`in_asm` 是翻译序不是执行序**：定位以 `int` 事件（时间序 + 寄存器）为准，`in_asm` 佐证。

---

## 6. 复现技巧

- **宿主负载注入**：TCG 竞态窗口受宿主调度抖动影响。`for k in $(seq 1 N); do yes > /dev/null & done`
  可在同一构建上把显形率从 0/60 拉到 ~2/20。**但别过载**：满载会顶出"最便宜"的时序失败
  （如 AP 启动 IPI 超时），掩盖真正的目标竞态 → 建议 2~4 个核的负载（或 `taskset` 只压部分核）。
- **KVM 对照**：同一 bug 在 KVM 下概率远低、形态或异；**别用 KVM 判"是否修复"**。
- **SMP**：`SMP=2` 干净、`SMP=6` 显形；提高 SMP 可加压。

---

## 7. 构建要点（被引导的到底是谁）

- 被引导的是 **`kernel.elf`**（经 `initramfs.img` 内的 `/kernel.elf` 加载），**不是** ESP 里的遗留 `init.elf`。
- 改源码后：`make kernel.elf initramfs`（`initramfs` 会重打包 `kernel.elf`）。
- ⚠️ **`make all` 会因 `src/fs` 缺 `BlockDevice.h` 失败**（既存问题，与抓取无关）→ 只构建所需目标。

---

## 8. 已知坑

- 异常样本时，工具末行之后会再打一行 `>>> 抓到样本: <tag>`；**包装脚本解析结果要用 `grep '^TAG='`**，
  别用 `tail -1`（否则会把 RESULT 解析成空 → 误删样本）。
- `-qmp` 只在 **`--dump-vmcore`** 时才挂（避免给非转储轮多挂一个 chardev 扰动 TCG 交织）。
- QEMU `-no-reboot`：风暴不会重启，会一直写到帽。
- `qmp-dump-vmcore.py` 结束时校验文件 size ⇒ 大镜像（8G）无假失败；`--timeout`（默认 900s）到点删残件。
- `trace-sym.py` 只支持 init / kernel 两个链接域；固件/loader 地址归 `other` 不批注（见 §3.3.2）。
- **本文档是唯一权威**；`Tools/tcg-trace/README.md` 若与本文冲突，以本文为准并即时修正。

---

## 9. ⛔ 档位死路：只保留 `in_asm`（2026-09-16 定）

**结论**：WRAITH 这类时序敏感 SMP 竞态，日志档位**只守 `-d in_asm,...` + `--dump-vmcore`**；
`-d exec` / `-d cpu` / TCG plugin（逐 TB）等**向上升档一律否决**。

**实测（TCG + SMP6）**：

| 档位 | 现象 |
|---|---|
| `-d exec(+nochain)` | 90 s 仅 5.9 GB，仍停在 OVMF（连 BdsDxe 都没到） |
| `-d cpu` | 26 s / 5.3 GB 仍在固件；叠 `-dfilter` 只圈内核映像，到 `[FPA::Init]` 就 20 GB+ |
| TCG plugin（insn 级 `arm=create_first_kthread`，逐 TB 带 cpu/rip/rsp） | 61 s / 1.4 GB、14.6 M 行；且**挂上后两次都停在 AP bringup** |

**判据**：逐执行级把 VM 拖慢 **100~1000×**，**"观测效应过强"会直接改写失败模式**（把竞态推去
AP 启动超时）⇒ 比"磁盘不友好"致命得多。代价：时序信息只能靠 `int` 事件的 **GS/GDT/SP 指纹**
+ 事后 vmcore 复原。

**执行者归属的替代手段**（`in_asm` 档下）：`int` 事件带 `GS=<base>`（每核 GS 复合体
`0xffff800000fe9000 + k*0x5000`，slot[1]=cpu id）与 `GDT=`(=GS+0x2801) 双指纹定 CPU；`SP` → 栈
→ task/hdstack（再对 `task_pool::m_tree` / `belonged_processor_id`）。
（备而不用：`-d exec` 行自带 vCPU 号 `Trace %d:`；`-dfilter <lo>+<size>` 按 guest PC 过滤且同时门控 `-d cpu`。）
