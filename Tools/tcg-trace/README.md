# TCG + trace + 体积帽 —— 「AI 无状态全自动调试」范式

> 目的：让 AI（或任何人）在**不依赖会话记忆、不依赖人肉复现**的前提下，
> 一把拿到一个**真实的竞态崩溃样本**（含 fault 前后的指令流/异常/寄存器），
> 然后逐条反汇编 + 符号化定位。

## 为什么需要这套

- SparrowOS 的崩溃是**随机 SMP 竞态**：`SMP=2` 干净、`SMP=6` 约 25% 异常 → 人肉复现又慢又不稳。
- 传统 gdb 单步在竞态下会改变时序，且「当前状态」难以描述给 AI。
- 需要一种：**可自动连跑 → 命中即停 → 现场可离线复盘**的抓取方式。

## 三件套

1. **TCG（`-cpu max`）** —— 软件模拟，时序与 KVM 不同，是**复现竞态/死锁的档位**
   （KVM 常把竞态"压没"或改写失败形态：panic ↔ hang）。
2. **trace 类别 `in_asm,int,guest_errors,unimp,cpu_reset,pcall`**
   - `in_asm`：每个**唯一** TB 的反汇编（翻译即记）→ 体积可控（正常 ~27MB/次）；
   - `int`：异常/中断 + **完整寄存器 dump**（含 `CR2/CR3/GS/GDT/TR`）→ 事故现场本体；
   - `guest_errors`/`unimp`/`cpu_reset`/`pcall`：非法访问 / 未实现 / reset / far-call 补充。
   - ⛔ **禁用 `-d cpu`**：每 TB 寄存器 dump ≈ **330MB/s**，一次 boot 就是几十 GB，不可用。
3. **体积帽** —— 故障风暴（异常自旋）会把 `-D` 写到**十几 GB**；脚本对 trace 文件设帽
   （默认 3GB），超帽即杀，结果判 `SIZECAP`。

## 用法

```bash
# 单次：到 kshell> / PANIC / 超时 / 超帽 即停
Tools/tcg-trace/tcg-trace.sh --tag myrun

# 连跑直到抓到 panic / hang / 风暴样本（抓到即停）
Tools/tcg-trace/tcg-trace.sh --tag hunt --repeat 30 --timeout 90 --cap-gb 3
```

输出：`<outdir>/<tag>.trace`（QEMU `-D`）、`<outdir>/<tag>.serial`（串口）；
末行打印 `TAG= RESULT= REASON= ELAPSED= TRACE= LINES= SERIAL=`；退出码 `10` = 抓到异常样本。

## 事后定位流程（可全自动）

1. **看 serial**：结果 + `PANIC:` + `[KURD]` + 栈回溯（`#0..#N RIP Symbol`）。
2. 在 `.trace` 里**定位首个异常事件**（`^\s*\d+: v=` 行）；其前一条 `IN:` 块即肇事 TB 的反汇编。
3. 用 `kernel/kernel.elf`（`nm -n -C` / `objdump -d`）反查 RIP 符号 ——
   **运行基址 `0xffff800000000000` 与 ELF vaddr 一一对应**（可直接 `nm` 命中）。
4. 结合寄存器 dump（`CR2/GS/CR3/GDT/TR`）判定损坏类型：野指针 / GS 被清 / 页表 / 控制流劫持。
5. **计数异常向量**区分形态：`grep -oE 'v=.. ' trace | sort | uniq -c` →
   `#PF`/`#GP`/`#DF` 风暴 vs 单点。

## 成本与纪律

| 场景 | 用时 | trace 体积 |
|---|---|---|
| 正常到 kshell | ~5–6 s | ~27 MB |
| panic 路径 | ~5–6 s | ~250 MB+ |
| 故障风暴 | 到帽为止 | GB 级 |

- **trace 吃盘：用完即删**（可再生；样本只留结论与 serial）。
- **帽子必备**：没有体积帽的自动抓取迟早把盘写爆。
- **`in_asm` 是翻译序不是执行序**：定位以 `int` 事件（时间序 + 寄存器）为准，`in_asm` 佐证。

## 已验证产出（2026-09-15，SparrowOS NVMe 竞态）

一把抓到并定位了多处 panic 路径缺陷：
- `Panic::panic` 读野 `context->gs_base` → `#GP` 自旋（"panic 里的 panic"）；
- `idt_vec_demux_entry` 的 IPI 空槽 → `call 0` → `RIP=0` 风暴；
- AP 的 `GS_BASE` 被 `resources_shift()` 的 `mov gs,sel` 清 0（长模式描述符 base=0）；
- `Panic::panic` 收尾单条 `hlt` 被唤醒后 fallthrough 到 `leave;ret` → 返回栈上野地址 → NX `#PF` 风暴。
