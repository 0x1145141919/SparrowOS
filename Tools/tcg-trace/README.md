# TCG + trace + 体积帽 —— 「AI 无状态全自动调试」范式

> 权威总纲（含 QMP 内存转储、输出布局、已知坑）：`kernel/Docs/Debug/TCG_TRACE_ARSENAL.md`

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

## 符号化精读（trace-sym.py）＋ 物理内存读取（ram-read.py）

裸 `.trace` 是 `-d in_asm` 的裸汇编，且 init.elf / kernel.elf / UEFI 固件三段地址混排，直接读 64 万行不可行。两件配套工具：

**`trace-sym.py`** — 把 `.trace` 按链接域解析成「符号+偏移 @ 源文件:行」：
- 链接域：init.elf @`0x101000000`；kernel.elf @`0xffff800000000000`（含低半区 `0x4000–0x8000`）。
- 每行输出 = `<sym+off @ src:line> | <原始未解析行>`（原始行逐字保留，便于对照）。
- 立即数（含 large model 的 `movabsq $<十进制巨值>`）与 RIP-相对（`disp(%rip)`）里的地址也解析，尾注 `; addr[...]`；含护栏（无 DWARF 覆盖 / 偏移过大 → 判为非常量，不解析）。
- `--src`：在指令流里插 C++ 源码行批注（`── file:line [func]` + 该行源码；`.asm/.s` 自动跳过）。
- 事件行的 `#N` 是 QEMU **全局事件序号**，**不是 CPU id**。
```bash
trace-sym.py w22_01.trace --start kernel --only-domain kernel --src --out w22_01.kernel.src.log
```

**`ram-read.py`** — 从 `--dump-mem` 产出的 `<tag>.ram`（物理 0 起、小端直出）按**虚拟地址**取字节（x86-64 4 级页表 walk）：
```bash
ram-read.py w22_01.ram 0x306000 0xffff8002dc604218 -n 128 --walk --ascii   # CR3 取自 panic serial
```

**已知坑（旧 pmemsave 路径）**：`qmp-memdump.py` 的 socket 超时 30s —— 满负载下 8GiB `pmemsave` 常超时 → `DUMPFAIL`，且留**截断** `.ram`。**该路径已由下方官方 `dump-guest-memory` 取代。**

**`ram-mkcore.py`** — 把 `.ram`(+serial) 合成为 **GDB 可读的 ELF ET_CORE**（按 `assets_remap` 静态表 + 页表 walk 写成 PT_LOAD，GDB 直接按内核虚拟地址读）：
```bash
ram-mkcore.py w22_01.ram w22_01.serial --out w22_01.core --gdb-out w22_01.gdb
gdb kernel.elf w22_01.core     # 或 gdb -x w22_01.gdb -q kernel.elf w22_01.core
```
- 数据用**稀疏文件**（`p_offset=物理地址`），同物理页只落一次 → 实体占用 ≈ 被映射物理页之和（默认预算 1GiB 后截断）。
- 从 serial 抄寄存器写 `NT_PRSTATUS` NOTE → `info registers` / `x/i $rip` / `bt` 全活。
- 低端 `0..0x1100` 被 ELF 头覆盖（real-mode 区，无碍）。
- 校验：`x/8xb 0xffff8000000053a0` 应 = `49 89 ff 48 b8 f9 22 01`。

## 官方 vmcore + 虚拟视角 core（新，首选内存转储路径）

不再手捣 `pmemsave`/自拼骨架，改用 **QEMU 官方 `dump-guest-memory`** 拿物理 vmcore，
再用自造 `vmcore-mkcore.py` 折成【虚拟视角】GDB core：

- **`qmp-dump-vmcore.py`** —— QMP `dump-guest-memory{paging:false, format:elf,
  protocol:file:<out>, detach:true}` + 轮询 `query-dump`。自带 `vm_stop/resume`（**不用先 stop**）；
  只枚举 **RAM 段**（洞跳过），写全 CPU `NT_PRSTATUS`；结束时**校验文件 size**（不再有 30s 假失败）。
- **`vmcore-mkcore.py`** —— 以 **`kspace_up_half`**（高半 128TB 的**扁平 PDPTE 表**，
  索引 `(v-0xFFFF800000000000)>>30`，17 位）为**单一根**走 `PDPTE→PD→PT`，
  产出 `p_vaddr=虚拟 / p_offset=物理` 的稀疏 core；NOTE 段原样搬运。
  - **收页判据（两信号取交）**：`P=1 且 phys∈vmcore RAM 段 且 缓存∉{UC,UC-}`。
    排设备 MMIO（HPET/IOMMU/ECAM/NVMe BAR 全 UC）；保留 WC 帧缓冲（RAM-backed）；
    **切忌“==WB”一刀切**（WC 会误杀；`phyaddr_window` 巨别名本就是 WB）。
  - 越界/不可信页表项 → `<tag>.map-report.txt`（**WRAITH 探针**：树↔页表分歧/野帧）。

**一条龙**：
```bash
Tools/tcg-trace/tcg-trace.sh --tag w --repeat 40 --dump-vmcore --mkcore
# 产物: <tag>.vmcore(物理,~8G) + <tag>.core(虚拟) + <tag>.map-report.txt（仅命中异常时）
gdb kernel.elf <tag>.core     # 6×LWP + info registers + 虚拟栈 x/… 全活
```
（旧 `--dump-mem`(pmemsave)/`qmp-memdump.py` 保留兼容，但推荐新路径。）
