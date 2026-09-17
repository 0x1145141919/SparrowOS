# WRAITH（幽魂）—— TCG 下的幽灵疑似竞态

> **代号**：`WRAITH`（幽魂）“来无影”（只在宿主调度抖动下现身）·“去无踪”（只留一两字节的栈/指针损坏）·“会附身”（一旦命中异常入口即自噬成风暴）
> **状态**：`疑似竞态 / 未收口`（root cause 未钉死；已有多枚样本 + 工具链）
> **锚点用途**：人机协作时一句话即可定位——「看下 WRAITH」「WRAITH 有新样本」。本文档是唯一权威上下文。
> **建立**：2026-09-15（首战：commit `42f6bbe`+`55cef76` 之后）

---

## 0. 一句话

SparrowOS 在 **SMP 多核 + TCG 软件模拟** 下，于 **NVMe 并行初始化** 时段随机出现**内存损坏式**故障：
它只破坏**内核栈/指针的高位字节**（或整字清零），把执行带到野地址；故障点常常是**无辜的受害者**
（如 `HPET_driver::get_time_stamp_in_us`）。轻则降级到 kshell，中则 hang，重则 panic，
最重时把**异常入口自身**打坏 → `#PF→#DF` 自噬风暴（见 §3）。

---

## 1. 现象学：同一起因，三种结局

| 结局 | 表现 | 典型样本 |
|---|---|---|
| **降级到 kshell** | NVMe init `0/2~1/2 OK`，内核照常到 `kshell>` | 多数 KSHELL 跑 |
| **HANG** | 卡在 `[NVMe] identify_ctrl...` / `identify_ns` 无 panic 无 kshell | hp11/hp16（实为风暴被帽掐） |
| **PANIC** | 单点 `#PF/#GP`，fault 站点各异 | hp04/07/12/13/15/19、bt04/15/16 |
| **风暴（最重）** | 坏 RIP 取指失败 → **异常入口 asm 自身 fault → #DF → 回环**，百万次级到 3GB 帽 | hp11（2.47M×#PF）、hp16 |

**风暴机理（hp11 实证）**：CPU 连"异常帧都压不进去/取不出来" → `#PF@0x3c25e` 立刻升级 `#DF@0x3c25e`
→ `#DF` 入口回到同一坏点 → 无限自激。**当前 #PF/#DF 用当前栈、无 IST**，栈一脏就雪崩。

---

## 2. 证据链（高度收敛）

1. **首发多是 HPET 受害者**：`ktime::get_microsecond_stamp()` → `HPET_driver::get_time_stamp_in_us()`
   的 `this`/`regs` 被踩（`#PF` CR2=0/8/0x18，或 `#GP` 因非规范地址）。HPET 只是这条热点路径上
   恰被踩到的全局对象，**不是病因**。
2. **损坏形态 = 内核指针/返回地址的"单字节/高位"改写**：
   - hp16：返回地址 `0xFFFF800000058B40` → `0xFF00800000058B40`（byte6 `0xFF→0x00`）
   - bt04：锁指针 `0xFFFF8002CC5127D8` → `0x02FF8002CC5127D8`（高位 `0xFFFF→0x02FF`）
   - 或整字清零（hp13 `RIP=0`）、或野值（hp07 `RIP=CR2=0xFFFF8002DC6042C8`）
3. **时空分布**：崩点全在 **worker 运行的核**（3/4 号，BSP 通常 0），时机全在
   **NVMe `identify / io_queue_init`（建 I/O CQ）** 期间（serial 停在 `CQ N created` 前后）。
4. **panic 路径自身也会被踩**：bt15 panic 上下文全烂（CR0/CR3/CR4/EFER 变成栈地址）；
   bt16 `panic_frame` 自己 fault（CR2=0x111）。→ 损坏已蔓到栈/上下文。

---

## 3. 根因假设（候选，按证据强度排序）

**触发面**：`nvme_parallel_init_all()`（新并行路径）首次把「BSP 高频 sleep/wake 轮询」×「2 个
`device_init` worker」叠在同一时段。老 `Init()` 是 `zombie_observe` 忙等、**不睡**。

| # | 假设 | 锚点 |
|---|---|---|
| A | **调度序列非原子 / 可被 IPI 重入**：`sleep_tasks_wake` 的「出队→set_ready→入队」跨多把锁、段间开中断；`resched`（IPI，`[[noreturn]]`）同样走 `next_task_with_routine` → 嵌套调度 | `per_processor_scheduler.cpp:141/289`、`kthread_interfaces.cpp:212/294` |
| B | **`wakeup_thread` 只 `interrupt_guard`（关本核 IF），非跨核互斥** | `kthread_interfaces.cpp:256` |
| C | **GS 派生索引无边界**：`get_other_scheduler(pid)=&global_schedulers[pid]` 不校验；而任务会被 `sched()` 跨核偷取 | `per_processor_scheduler.cpp:297`、`sched()` :195 |
| D | **内存映射/TLB shootdown 跨核协议**：worker 建队列 ring 时 map UC 页 → 触发 shootdown；并发下映射/共享态被踩 | `out_surfaces.cpp:323`、`AddresSpace.cpp:1125`（PCID） |
| E | NVMe DMA/PRP 误指向（设备往错地址搬数据） | `NVMe_init_and_shutdown.cpp` / `PRPs.cpp` |

**受害链**：并发踩坏某核的栈/指针 → 该核下一次调 `ktime`（热点）就首爆 → 若命中栈/入口则升级。

---

## 4. 已确认 / 已排除

- ✅ **确认（用户实测）**：**KVM 下本 bug 概率远小于 TCG**。→ TCG 是复现档位；**别用 KVM 判"是否修复"**。
- ✅ **确认**：宿主负载是复现关键变量。空载 **0/60**；注入 12×`yes` 后 **2/20**。
  （TCG 竞态窗口对宿主调度抖动极敏感。）
- ✅ **确认**：**风暴**（hp11/hp16）是"异常入口自噬"；`#PF` 用当前栈、无 IST。
- ✅ **确认**：GS_BASE 在多数样本中 **sane**（在 GS 区内、4096 对齐）——此前"GS 被 resources_shift 清 0"
  本轮**未复现**（panic 期已注释 `resources_shift`）。
- ✅ **确认**：commit `42f6bbe` 的 IPI 空槽防御**生效过一次**（hp19 抓到 `[vec_demux] IPI_RETURNABLE with NULL func!`），
  但它**不是根因**。
- ✅ **确认（2026-09-16）**：**日志档位只能守 `in_asm` + `--dump-vmcore`**。逐执行级档位
  （`-d exec` / `-d cpu` / TCG plugin）**观测效应过强**（拖慢 100~1000×，且会把失败模式推去 AP bringup）
  → **判死路**；实测数据与归属替代法见 `TCG_TRACE_ARSENAL.md` §9。
- ❌ **排除**：内核堆 `kpoolmemmgr` 是 **per-HCB 上锁**的，非裸的（`spintrylock_*`）。
- ⚠️ **bisect（临时改动，见 §6）效果**：`kthread_sleep`→`microsecond_polling`（忙等）后
  **风暴 2→0**；总体异常率预-bisect `8/20` → post-bisect `5/120 ≈ 4%`。
  **但** post-bisect 的 panic 率很低且噪声大：小样本（20 跑）不可靠。
- ⚠️ **负载副作用**：12×`yes` 满载会把"最便宜的时序失败"顶出来——`ld18/ld20` 是
  **早期 AP 启动 IPI 超时**（`create_first_kthread: AP start failed`，KURD INTERRUPT/FATAL），
  **不是**我们要的 NVMe worker 竞态。降载（2~4 个 `yes`）待试。

---

## 5. 复现配方

```bash
# 工具：kernel/Tools/tcg-trace/tcg-trace.sh（TCG + trace + 体积帽 + 可选内存转储）
cd /home/PS/PS_git/OS_pj_uefi/kernel

# 单次（TCG -cpu max，SMP=6 由脚本内部 -smp 6；命中 PANIC/kshell/超时/超帽即停）
Tools/tcg-trace/tcg-trace.sh --tag probe --timeout 40 --cap-gb 1

# 连跑到抓到异常，并抓官方 vmcore（8G RAM，首选内存路径）→ <tag>.vmcore(+.regs)
Tools/tcg-trace/tcg-trace.sh --tag hunt --repeat 40 --dump-vmcore

# 只留 PANIC、其余即删（省盘）；产物落 /mnt/huge_data/sparrowos_debug/traces/
```

**关键变量**：`SMP=6`、`-cpu max`（TCG 软件模拟；KVM 会把竞态"压没"）、**宿主负载**（决定显形率）。

**构建**：被引导的是 `kernel.elf`（打进 `initramfs.img`，非 ESP 里的遗留 `init.elf`）；
改源码后须 `make kernel.elf initramfs`。（`make all` 会因 `src/fs` 的 `BlockDevice.h` 缺失而失败——既存问题，与 WRAITH 无关。）

---

## 6. 武器库（引用，不复制）

> WRAITH 用的武器是**通用资产**，权威文档在 **`Docs/Debug/TCG_TRACE_ARSENAL.md`**
> （TCG+trace 范式 · `tcg-trace.sh` / `qmp-dump-vmcore.py`（首选内存路径）· `vmcore-mkcore.py` /
> `trace-sym.py` 离线符号化 · 输出布局 · 复现技巧 · 已知坑）。
> 本节只留 **WRAITH 专用配方**；武器升级时只改武器库文档，不动战报。

**WRAITH 专用抓取配方**（工具见武器库 §3）：

```bash
cd /home/PS/PS_git/OS_pj_uefi/kernel
# 带官方 vmcore 的连跑（只留 PANIC/FAULT；建议叠 2~4×yes 负载）：
Tools/tcg-trace/tcg-trace.sh --tag w --repeat 40 --timeout 40 --cap-gb 1 --dump-vmcore
```

**WRAITH 特异点**（武器库没写的）：
- 风暴样本 `SIZECAP` 会写到帽，`.trace` 数 GB；`hp11` 的抓手是**异常入口自噬**（`#PF@entry → #DF@entry` 回环），
  定位时数向量 `#PF` vs `#DF`（hp11：2.47M×#PF / 871k×#DF）。
- 首发常在 `0x2C6A5`（`HPET.cpp` 受害者）与 `0xD2C3`（`spinlock_cpp_t::unlock`）两处之一。

---

## 7. 仓库改动状态（接手必读）

| 文件 | 改动 | 性质 |
|---|---|---|
| `src/arch/x86_64/core_hardwares/NVMe/NVMe_init_thread.cpp` | `poll_report_board` 的 `kthread_sleep(poll_us)` → `ktime::microsecond_polling(poll_us)`（标 `[BISECT]`）| **临时实验**，可 `git checkout` 回滚 |
| `Tools/tcg-trace/tcg-trace.sh` | 新增 `--dump-always/--dump-vmcore/--mkcore/--dump-fault-only/--selfcheck`；QMP 仅转储时挂；默认 outdir 切到 `/mnt/huge_data/...` | 工具增强 |
| `Tools/tcg-trace/qmp-memdump.py` · `ram-read.py` · `ram-mkcore.py` | **已删除**（2026-09-17：老式 `.ram`/pmemsave 路径整体废弃，减少接手者上下文污染）| 工具增强 → 废弃 |
| `Tools/tcg-trace/README.md` | 更新（**今起为薄壳速查**；权威在武器库）| 文档 |

> 注（2026-09-16 晚）：上述改动**均已提交**（HEAD 附近若干笔，含 `4b300a0` 的 `[BISECT]` 忙等）；本文档此前标「未提交」已过时。

### 7.1 ⚠️ 当前加固 —— 权宜之计（2026-09-16 晚已提交）

- **给 `#PF` 挂 `IST2`（借 #MC）、给 `#GP` 挂 `IST3`（借 #NMI）**，让异常入口永远有独立可写栈，
  把“栈脏 → 入口压栈即 fault → #DF 自噬风暴”**收敛成一次可打印的 panic**。
  同 IDT 语义已同步到 FRED `IA32_FRED_STKLVLS`（#PF→MC 级、#GP→NMI 级）。
- **⚠️ 这是临时止血，不碰竞态本身 ⇒ 复现率不变，只是让失败模式可读。**
  **一旦 `#PF/#GP` 根因修好，两个 `ist_index` 必须回落到 0（还原“用当前栈”）。**
- **顺带修掉的真 bug**：异常入口 asm 的 swapgs 判据 CS 偏移错误（`Sysdef_exception_entries.asm`）——
  两个宏漏算 RIP，分别读到了 RIP(带错误码)/shim(无错误码) 而非 CS；已统一改为 `[rsp+15*8+16]`（CS@136）。
  旁证：`vec_demux_common`（向量 32–255）一直是正确的 `VEC_OFFSET+16`。
- 验证：`make kernel.elf initramfs` ✓；空载 TCG 探针 `istcheck`/`istcheck2` → `KSHELL`，无回归。
- 残留疑点：FRED `STKLVLS` 的 NMI/#DF 级（2/3 → RSP2/RSP3=ist[2]/ist[3]）与 IDT 的 `#NMI=IST3 / #DF=IST1` **不一致**（既存；FRED 在 TCG 未启用，另立条目）。

### 7.2 旧魔法断点拆除 + 首爆冻结钩子（2026-09-17）

**拆掉的旧机制**：`breakpoint_cpp_enter`（`#BP`/int3）里的 `outb(0xDB, 0x80)`——它是
"往 guest 放 `int3` → QEMU 暂停 → 挂 GDB"那套老设计（`kvm_gdb_debug_notes.md`）的残留。
现改为不依赖外部补丁的 `cli;hlt` 干净停机（同 init.elf 侧）。同时删掉 `init_init.cpp` 里
注释残留的 `//outb(0xDB,0x80)` 与不再需要的 `#include "sys/io.h"`。

**新增首爆冻结钩子**（post-fault，零观测效应）：
- `src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm` 新增 `FAULT_FREEZE` 宏，
  挂在 `#PF`(`0x0E`)/`#GP`(`0x0D`)**入口**，`call` C handler **之前**；
- **默认关闭**，取消文件内 `;%define SPDB_FAULT_FREEZE` 注释即开启（跑取证必开）；
- 动作：串口 `#WF#` → `outb(0x80,0xDB)` → `cli;hlt` 兜底；
- 已实测（临时注入只读 `.text` 写）：`RESULT=FAULT REASON=MAGICBP`，256MB RAM 成功落盘，
  且默认构建（钩子关）回归到 `kshell>` 无影响。

**QEMU 侧**：ioport80 魔法断点补丁已固化为
**`Tools/tcg-trace/patches/qemu-ioport80-magic-bp.patch`**（+ README，含部署纪律）。
⚠️ **血泪教训**：旧补丁是手工 `cp` 进 `/usr/bin` 的构建，**2026-08-29 Arch 升 qemu 11.1.1
时被无声覆盖**，导致此前的 WRAITH 狩猎全程跑的都是没断点的原版。现在改用 `QEMU_BIN` 显式
路径 + `tcg-trace.sh --selfcheck` 自检（探针镜像 `outb(0x80,0xDB)` 后 `status==debug`）。

**host 侧**：`tcg-trace.sh` 新增 `QEMU_BIN` / `--selfcheck` / `FAULT` 结果类；
`qmp-wait-stop.py` 常驻监听 QMP `STOP` 事件（零轮询开销，避免每 0.2s 起 python 的宿主负载污染复现）。
武器库已同步（`TCG_TRACE_ARSENAL.md` §3.1/§3.3）。

---

## 8. 下一步实验清单

1. **降载细调**（首选）：`yes` 从 12→2~4（或 `taskset` 只压部分核），带 `--dump-vmcore` 连跑，
   目标是钓出 §3-D/E 那条 NVMe-worker 竞态，而非 AP-bringup 超时。
2. **啃现有样本**：`ld18/ld20` 有完整 8G RAM 镜像；用 trace 的 `CR3` 做 vaddr→phys，
   检查 AP 启动为何失败（可能仍是同一套 IPI/GS 机器）。
3. ~~**结构性防风暴**：给 `#PF/#DF` 配 **IST 独立栈**，把风暴收敛成一次可打印的 panic。~~
   ✅ **已做（权宜）**：`#PF→IST2`、`#GP→IST3`，见 §7.1；**根因修好后须回落 `ist_index=0`**。
4. **收口调度序列**：把 `sleep_tasks_wake`/`resched` 的「出队→set_ready→入队」整段原子化（一锁/一段关中断），
   堵掉 IPI 重入窗口。
5. **上限/断言**：`get_other_scheduler` 加边界检查；`kthread_sleep`/`resched` 入口加 `pid`/`task` 合法性自证
   （**绕过 kout 的单字节 UART**，保证风暴前能留证）。
   - 🟡 **通道已落地（2026-09-17）**：IRQ-safe 内存日志环 `interrupt_log_ring`
     （`util/debug_tmp_ring_buff.h`；`kernel_start` 在 AP bring-up 前用 FPA 4MiB + 主窗口 `PHYACC_VA` 出生；
     `print` 内部零加锁，临界区由调用方编排）。**断言本身尚未挂**——待把 §3 候选断言接到该环上，跑 t10_33 类场景回归。
6. **DMA 审计**：`io_queue_init`/`PRPs.cpp` 的 ring 物理地址与 PRP 打包。

---

## 9. 样本台账（2026-09-15 首战）

> 旧样本目录 `VMresources/traces/` **已清空**（省盘）；下列为当时普查结果。
> 新样本落在 `/mnt/huge_data/sparrowos_debug/traces/`。

| 批次 | 构建 | 轮数 | 异常 | 结局 |
|---|---|---|---|---|
| `hp01..20` | pre-bisect（`kthread_sleep`）| 20 | **8** | 6 PANIC（hp04/07/12/13/15/19）+ 2 风暴（hp11/16）|
| `bt01..20` | bisect（忙等）| 20 | **3** | PANIC：bt04（`unlock` 锁指针高位被踩）/bt15（#GP 风暴+panic）/bt16（panic_frame 自崩）|
| `pd01..40` | bisect | 40 | 2 | **被脚本解析 bug 误删**（结局未知）|
| `pk01..40` | bisect + QMP | 40 | 0 | — |
| `noq01..20` | bisect（无 QMP）| 20 | 0 | — （→ 排除 QMP 观测者效应）|
| `ld01..20` | bisect + 12×`yes` 负载 | 20 | **2** | `ld18`/`ld20` = AP 启动 IPI 超时（**非**目标竞态），各留 8G 内存镜像（当时为 `.ram`；老路径已废弃）|

**首战关键 PANIC 站点**：
- hp04：`#PF` @ `spinlock_cpp_t::unlock` ← `sleep_tasks_wake`（cpu1）
- hp19：`#PF` CR2=0 ← `HPET_driver::get_time_stamp_in_us` ← `poll_report_board` ← `nvme_parallel_init_all`
- bt04：`#GP` @ `spinlock_cpp_t::unlock`，`this=0x02FF8002CC5127D8`（高位被踩）

---

## 10. 关键源码坐标

| 位置 | 说明 |
|---|---|
| `src/arch/x86_64/core_hardwares/NVMe/NVMe_init_thread.cpp` | `nvme_parallel_init_all` / `poll_report_board`（触发面）|
| `src/arch/x86_64/time.cpp:46` | `ktime::get_microsecond_stamp` 分派（`readonly_timer->…`）|
| `src/arch/x86_64/core_hardwares/x86_arch/HPET.cpp:122-127` | 受害者函数 |
| `src/scheduler/per_processor_scheduler.cpp:141/289/297` | `sleep_tasks_wake` / `next_task_with_routine` / `get_other_scheduler` |
| `src/scheduler/kthread_interfaces.cpp:100/212/256/294` | `kthread_common_save` / `resched` / `wakeup_thread` / `kthread_sleep_cppenter` |
| `src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm` | 异常入口（原**无 IST**；现权宜挂 #PF/#GP，见 §7.1）|
| `src/arch/x86_64/Interrupts/exceptions_handler.cpp:25` | `page_fault_handler`（内核态→panic）|
| `src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp` | `idt_vec_demux_entry`（IPI 分发 + 空槽防御）；`early_init`/`fred_init_stklvls`（IST 槽位与权宜加固，§7.1）|
| `src/memory/out_surfaces.cpp:323` | `broadcast_invalidate_tlb`（TLB shootdown 等待循环）|
| `src/memory/arch/x86_64/AddresSpace.cpp:1125` | PCID 分配（`get_gs_base()->pcid_complex`）|
| `src/arch/x86_64/boot/kinit.cpp:147` | `create_first_kthread`（AP 启动 IPI，ld18/20 崩点）|
