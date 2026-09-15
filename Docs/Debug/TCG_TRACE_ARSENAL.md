# TCG_TRACE_ARSENAL —— TCG+trace 抓取武器库

> **定位**：SparrowOS 的**通用**内核崩溃取证能力（与本仓库某个具体 bug **解耦**）。
> 任何"随机/竞态/风暴"类问题都先来这里取武器。具体 bug 的战报另开文档
> （如 `Docs/Debug/WRAITH.md`），只引用本文档，不复制武器库。
> **建立**：2026-09-15。**配套代码**：`Tools/tcg-trace/`。

---

## 0. 一句话

让 AI（或任何人）在**不依赖会话记忆、不依赖人肉复现**的前提下，一把拿到一个**真实竞态崩溃样本**
（fault 前后的指令流 / 异常 / 寄存器 / **最终物理内存镜像**），然后离线符号化定位。

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

## 3. 工具

### 3.1 `Tools/tcg-trace/tcg-trace.sh`
复用 VMtest-nosudo 的 mtools 零-sudo ESP staging + QEMU 参数；叠加 `-D/-d trace`；
poll 串口，命中 `PANIC|kshell>` / 超时 / 超帽即停；`--repeat` 连跑到抓到异常样本。

| 参数 | 说明 |
|---|---|
| `--outdir DIR` | 输出目录（默认 `/mnt/huge_data/sparrowos_debug/traces`，不存在则回退 `$VM/traces`）|
| `--tag NAME` | 样本名（默认 `trace`）|
| `--timeout SEC` | 单次墙钟上限（默认 90，超时判 `HANG`）|
| `--cap-gb N` | trace 体积帽（默认 3GB，超帽判 `SIZECAP`）|
| `--stop-on RE` | 串口命中即停正则（默认 `PANIC\|kshell>`）|
| `--repeat N` | 连跑 N 次，抓到 PANIC/HANG/SIZECAP 即停 |
| `--base DIR` | 仓库布局根（默认 `/home/PS/PS_git/OS_pj_uefi`）|
| `--dump-mem MB` | **异常停止时额外转储 MB 物理内存 → `<tag>.ram`** |
| `--mem-base A` | 转储起始物理地址（默认 0，可 `0x…`）|
| `--dump-always` | 连正常样本也转储（基线用）|

环境变量：`SMP`（默认 6）。退出码 `10` = 抓到异常样本。
末行结构化输出：`TAG= RESULT= REASON= ELAPSED= TRACE= LINES= SERIAL= RAM=`。

`RESULT` 分类：`KSHELL`（正常到提示符）/ `PANIC` / `HANG`（无 panic 无 kshell）/ `SIZECAP`（风暴到帽）/ `OTHER`。

### 3.2 `Tools/tcg-trace/qmp-memdump.py` —— 最终内存镜像
停止瞬间经 **QEMU QMP**：`qmp_capabilities → stop（冻结全部 vCPU，一致快照）→ pmemsave(base,size,file) → quit`。
`pmemsave` 返回即表示写完。**必须在 QEMU 主循环仍活着的窗口调用**——
storm/panic 下 TCG 主循环是独立线程，故仍可用；这正是"把内存镜像和 trace 一起留下"的关键。

输出 `<tag>.ram` = 原始物理内存镜像（little-endian 直出），可用 `dd`/python 按 offset 取任意页。

---

## 4. 输出布局与生命周期（大工件仓）

**约定变量**：`SPARROW_DEBUG_STORE`（默认 `/mnt/huge_data/sparrowos_debug`）。
工具读它 → **仓库里不写死本机路径**（换机器/克隆只改环境变量）。

```
$SPARROW_DEBUG_STORE/            ← 默认 /mnt/huge_data/sparrowos_debug（独立 1.9T btrfs）
├── README.md    布局说明
├── INDEX.md     样本台账（tag → 结局 → 结论 → 保留否）—— 机器本地活索引
├── traces/      <tag>.trace / <tag>.serial / <tag>.ram
├── system_log/  系统日志
└── memdump/     独立/离线内存转储
```

**三层生命周期**（分开管，别混）：

| 层 | 内容 | 去处 | 周期 |
|---|---|---|---|
| A | 战报 / 武器库 / 结论 | **git 仓库** `Docs/Debug/` | 永久 |
| B | 样本台账 | store `INDEX.md` | 随样本，定期归档 |
| C | 原始 `.trace/.ram/.serial` | store `traces/` | **可弃**（可再生）|

**纪律**：狩猎只留 `PANIC` 的 `trace+ram`，其余即删；`.ram`（8G/个）结论提取后优先删；
风暴 `.trace`（GB 级）用后即删。**结论留存，镜像可弃**。

---

## 5. 离线定位流程

1. **看 serial**：结局 + `PANIC:` + `[KURD]` + 栈回溯（`#N RIP Symbol`）。
2. 在 `.trace` **定位首个异常事件**（`^\s*\d+: v=` 行）；其前一条 `IN:` 块即肇事 TB 反汇编。
3. 用 `kernel/kernel.elf` 反查 RIP 符号：**运行基址 `0xFFFF800000000000` 与 ELF vaddr 一一对应** →
   `addr2line -f -C -e kernel.elf <addr>` 直接命中 `file:line`。
4. 结合寄存器 dump（`CR2/CR3/GS/GDT/TR/CS`）判损坏类型：野指针 / GS 被清 / 页表 / 控制流劫持。
5. **计数异常向量**区分形态：`grep -oE 'new 0x.. ' trace | sort | uniq -c` → `#PF`/`#GP`/`#DF` 风暴 vs 单点。
6. **有 `.ram` 时**：用 trace 里的 `CR3` 走页表做 vaddr→phys，直接把坏地址/栈/页表在镜像里挖出来。

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
- `-qmp` 仅在有 `--dump-mem` 时才挂（避免给非转储轮多挂一个 chardev 扰动 TCG 交织）。
- QEMU `-no-reboot`：风暴不会重启，会一直写到帽。
