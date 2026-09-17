# WRAITH 单样本根因分析 — `w22_01`

- **日期**：2026-09-16
- **样本**：`/mnt/huge_data/sparrowos_debug/traces/w22_01.{trace,serial,ram}`
- **构建**：`kernel/kernel.elf`（含未提交 `[BISECT]` 忙等改动；`poll_report_board` 用 `ktime::microsecond_polling`）
- **分析范围**：只读现场三件套 + 源码；分析脚本写在 `/mnt/huge_data/sparrowos_debug/analysis/`
- **GDB 后验 core（首选）**：`w22_01.core` + `w22_01.gdb`（寄存器按 serial 注入 NT_PRSTATUS，内存按内核 VA 直读）。**本文字节级证据优先用 GDB 命令+输出**；`ram-read.py`/自写脚本作为交叉复核。
- **一句话**：崩溃核在 **NVMe CQ 设备中断 → `resched()`（中断上下文里整段嵌套调度）→ `sched()`** 这条链上，发生 `#PF@RIP=0` 取指缺页；**该 `#PF` 的异常帧被同一栈上的 `sched()` 栈内容覆盖**，导致 `page_fault_handler` 因帧 `CS` 低位=01 落入"既非内核(=0)也非用户(=3)"的空档而**静默返回**，随后 `iretq` 把野值 `CS=0x9d51` 装进 `CS` → `#GP(err=0x9d50)` → panic。
  **即：panic 报的 `#GP` 是次生伤，首发是内核根本没上报的 `#PF@RIP=0`。**

---

## 0. 结论

1. **崩溃核 = logical processor 2**（GS_BASE `0xffff800000ff3000` = GS 复合体基址 `0xffff800000fe9000` + 2×0x5000；与 panic `processor_id=2` 一致）。
2. **首发故障**：`#PF`（`v=0e e=0010`，I/D 取指位，`pc=0x0`，`CR2=0`，`CPL=0`，`IF=0`），事件 `#1219`。
3. **死因链**：`#PF@RIP=0` → 异常帧被踩 → `page_fault_handler` 静默返回 → 返回 `iretq` 装 `CS=0x9d51` → `#GP e=9d50`（事件 `#1225`）→ panic。
4. **触发面**：`NVMe_Controller::interrupt_handle` 恒返回 `TOKEN_FLAG_MASK_TOKEN_SCHEDULE`；`idt_vec_demux_entry` 默认（设备向量）分支据此**立即调用 `resched(raw_frame)`** ⇒ **每个 NVMe CQ 中断都在中断上下文里跑一整段 `resched → next_task_with_routine → sched`**。崩溃核栈上正有这条调用链。
5. 与 WRAITH §3 **假设 A**（调度序列非原子 / 可被 IPI 重入）**吻合度最高**；其余假设 A–E 逐条见 §4。

---

## 1. 现场事实核对（含命令）

事件编号由 `Tools/tcg-trace/trace-sym.py` 生成；原始事件可用 `grep -nE '^\s*[0-9]+: v=' <tag>.trace` 复核。

```bash
cd /mnt/huge_data/sparrowos_debug/traces
grep -nE '>>> EVENT #12(1[0-9]|2[0-5])' w22_01.kernel.sym.log
```

关键行（`sym.log`）：

```
54349:>>> EVENT #1219 EXC v=0e(#PF) e=0010 i=0 cpl=0  pc=0x0
54510:>>> EVENT #1225 EXC v=0d(#GP) e=9d50 i=0 cpl=0  pc=0xffff80000003c364  [kern] ..@23.skip_gs_back+0x1b @ .../Sysdef_exception_entries.asm:117
```

`#1219` 寄存器（`sym.log`，与 raw trace 一致）：

```
RAX=00000000013169ac RBX=0 RCX=0000000000000838 RDX=0
RSI=00000000013169ac RDI=0000000000000838 RBP=0 RSP=ffff8002dc604240
R8 =ffff8002dc6040c8 R10=0000000000010206 R11=8
RIP=0000000000000000 RFL=00000046 [---Z-P-] CPL=0
CR2=0000000000000000 CR3=0000000000306000 GS =0010 ffff800000ff3000
```

`#1225` 寄存器：

```
RIP=ffff80000003c364  RSP=ffff8002dc604218  CR3=0x306000  GS_BASE=0xffff800000ff3000
RFL=00000086  CS=0008  SS=0010
```

panic serial（`w22_01.serial`）：

```
kernel_context cause #GP(General Protection)
[PANIC ON] processor_id=2 x2apicid=0x2
... RIP: 0xFFFF80000003C364  RSP: 0xFFFF8002DC604218  CR2: 0x0  CR3: 0x306000
... GS_BASE: 0xFFFF800000FF3000
Kernel panic at ..@23.skip_gs_back + 1B
```

serial 的 NVMe 段（触发面背景）：`[NVMe] CQ 1 created` → `[NVMe] create_io_cq qid=2 failed` → 旋即 `PANIC`（即崩点在 `io_queue_init` 的 **建 I/O CQ** 循环里）。

### 1.1 GDB 后验 core（后文首选工具）

```bash
cd /mnt/huge_data/sparrowos_debug/traces
gdb -q -batch -x w22_01.gdb /home/PS/PS_git/OS_pj_uefi/kernel/kernel.elf w22_01.core \
    -ex "info registers" -ex "bt" -ex "x/8xb 0xffff8000000053a0"
```

```
(gdb) rip            0xffff80000003c364   <..@23.skip_gs_back+27>
      rsp            0xffff8002dc604218
      eflags         0x10086              [ PF SF RF ]
      cs             0x8                 8
      ss             0x10                16
      gs_base        0xffff800000ff3000
(gdb) bt
#0  .@23.skip_gs_back () at .../src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm:117
#1  0x0000083800000000 in ?? ()
#2  0x00000000000c9d51 in ?? ()
#3  0x00000000017125aa in ?? ()
#4  0xffff8002dc604250 in ?? ()
#5  0xffff80000002e360 in x2apic::x2apic_driver::raw_config_timer_init_count ... lapic.cpp:22
(gdb) x/8xb 0xffff8000000053a0
0xffff8000000053a0 <_kernel_Init>:  0x49 0x89 0xff 0x48 0xb8 0xf9 0x22 0x01
```

> `_kernel_Init` 读出字节与 `sym.log` 首条指令 `49 89 ff 48 b8 f9 22 01…` 一致 ⇒ core 的 VA→PA 取数可信（生成器 `Tools/tcg-trace/ram-mkcore.py`：assets_remap 静态表 + 页表 walk）。**注意：GDB 的 `bt` 本身就把被踩坏的异常帧当调用链打印出来（见 E4）。**

---

## 2. 崩溃核（pid2）时间线

用自写脚本按 GS 基址归因到 CPU（`GS = 0xffff800000fe9000 + pid*0x5000`）：

```bash
python3 analysis/parse_events.py traces/w22_01.trace    # 见 §7
```

pid2 全程仅 14 个事件；崩溃前最近的：

```
#1175 v=20 pc=0xffff80000000574e rsp=0xffff8002dc604608   # 定时器中断落在 ksystemramcpy 的 ret 上
#1185 v=20 ...同 pc/rsp...
#1198 v=20 ...同 pc/rsp...
#1217 v=20 pc=0xffff80000000574e rsp=0xffff8002dc604608   # 崩溃前最后一个 pid2 中断
#1219 v=0e pc=0x0                rsp=0xffff8002dc604240   # <<< 取指 #PF
#1225 v=0d pc=0xffff80000003c364 rsp=0xffff8002dc604218   # <<< iretq 上 #GP
```

- `#1217` 之前 RFL 的 `IF=1`；`#1219` 的 `RFL=0x46` ⇒ `IF=0` ⇒ 崩溃核此刻在**中断/临界区**里。
- 其它 CPU（pid3/5/…) 同期的 `v=20/0x21` 与 `v=e2`(调度软中断) 交错，是 TCG 多核日志交织，属正常。
- 崩溃核栈（VA `0xffff8002dc604000` ⇔ PA `0x40f000`，走 CR3=`0x306000` 页表）里存在**中断内嵌套调度**的整条链（见 §3-E5）。

---

## 3. 证据链

### E1 崩溃核 = pid2

`GS_BASE=0xffff800000ff3000`；`conjunc_GSs: vaddr=0xffff800000fe9000 size=0x1e000`（6 核 × 0x5000）⇒ `(0xff3000-0xfe9000)/0x5000 = 2`。panic 亦报 `processor_id=2`。

### E2 首发 = 取指 #PF@RIP=0（内核没报）

`#1219 EXC v=0e e=0010 i=0 cpl=0 pc=0x0`，`CR2=0`。错误码 `0x10` = 第 4 位（I/D）置位 = **取指缺页**。即控制流被带到地址 `0`，取指即 fault。

### E3 `#PF` 处理路径：静默返回，不是 panic

trace 显示 `#PF` 入口完整跑了一遍但没有进 panic 分支。用 GDB 反汇编 `page_fault_handler` 的 `CS` 判定：

```bash
gdb -q -batch -x w22_01.gdb kernel.elf w22_01.core -ex "x/8i 0xffff800000037dbb"
```

```
0xffff800000037dbb <page_fault_handler+46>:  and    $0x3,%eax
0xffff800000037dbe <page_fault_handler+49>:  cmp    $0x3,%rax
0xffff800000037dc2 <page_fault_handler+53>:  je     0xffff800000037e8e   ; ==3(用户)→空分支
0xffff800000037dc8 <page_fault_handler+59>:  mov    -0x118(%rbp),%rax
0xffff800000037dcf <page_fault_handler+66>:  mov    0x88(%rax),%rax       ; IDT_CS(frame)
0xffff800000037dd6 <page_fault_handler+73>:  and    $0x3,%eax
0xffff800000037dd9 <page_fault_handler+76>:  test   %rax,%rax
0xffff800000037ddc <page_fault_handler+79>:  jne    0xffff800000037e8e   ; !=0(既非内核也非用户)→空分支
```

源码（`exceptions_handler.cpp:25`）：

```c
static void page_fault_handler(frame,errcode,liner_addr){
    if((IDT_CS(frame)&0x3)==0x3){/*用户态 TODO*/}
    else if((IDT_CS(frame)&0x3)==0x0){ /*内核态 panic*/ }
}
```

⇒ **`IDT_CS(frame)&3 == 1`**：既非 `3` 也非 `0`，两个分支都不进 → 直接返回。也就是说，**内核态 `#PF` 因为帧里的 `CS` 被踩成低位=01 的值，被内核悄悄"放行"了**。

### E4 字节级铁证（GDB）：`#GP e=9d50` 就是"帧里被踩出来的假 `CS=0x9d51`"

**① GDB `bt` 直接把 `iretq` 要弹出的返回帧当作调用链打印**——它正是栈上被覆盖的那 5 个字：

```bash
gdb -q -batch -x w22_01.gdb kernel.elf w22_01.core -ex "bt"
```

```
#0  .@23.skip_gs_back () at .../Sysdef_exception_entries.asm:117   ; iretq
#1  0x0000083800000000 in ?? ()          <- RIP 槽
#2  0x00000000000c9d51 in ?? ()          <- CS  槽  ← 非法段选择子
#3  0x00000000017125aa in ?? ()          <- RFLAGS 槽
#4  0xffff8002dc604250 in ?? ()          <- RSP 槽
#5  0xffff80000002e360 in x2apic::x2apic_driver::raw_config_timer_init_count ... lapic.cpp:22
```

**② 直接读帧槽 + `#GP` 帧**：

```bash
gdb -q -batch -x w22_01.gdb kernel.elf w22_01.core \
  -ex "x/6gx 0xffff8002dc6041e0" -ex "x/5gx 0xffff8002dc604218"
```

```
0xffff8002dc6041e0: 0x0000000000009d50  0xffff80000003c364   ; #GP errcode, RIP=iretq
0xffff8002dc6041f0: 0x0000000000000008  0x0000000000010086   ; CS=8, RFLAGS=0x10086
0xffff8002dc604200: 0xffff8002dc604218  0x0000000000000010   ; RSP, SS
0xffff8002dc604218: 0x0000083800000000                    ; <- iretq 从这里取 RIP
0xffff8002dc604220: 0x00000000000c9d51                    ; <- iretq 从这里取 CS
0xffff8002dc604228: 0x00000000017125aa
0xffff8002dc604230: 0xffff8002dc604250
0xffff8002dc604238: 0xffff80000002e360
```

**③ 错误码闭环**：

```bash
gdb -q -batch -x w22_01.gdb kernel.elf w22_01.core -ex "p/x (0xc9d51 & ~0x7)" -ex "p/d 0x9d51 >> 3"
```

```
$1 = 0xc9d50      ; 低 16 位 = 0x9d50 == 观测到的 #GP errcode (e=9d50)
$2 = 5034         ; 选择子 index=5034 ≫ GDT limit(0x3F/8 = 8 项) ⇒ 非法
```

⇒ **`iretq` 的 `CS` 来源 = 栈槽 `[0xffff8002dc604220]`**，于是 `#PF` 异常帧基址 = `0xffff8002dc604210`（`CS` 在帧内偏移 +0x10）。该槽"应为 `0x0008`，实为 `0x00000000000c9d51`"。同法可证：全镜像内满足"低 16 位 `&~7 == 0x9d50`"且非 `#GP` errcode 自身的 qword 只有这一个（`mem_scan.py` 复核）。

`iretq` 五个源槽（VA · 值，GDB `x/` 读出）：

| 槽 | 应为（CPU 压入的取指 #PF 帧） | 实为（镜像读出） |
|---|---|---|
| +0x00 `[0x4210]` 错误码 | `0x10` | `0x00000000017125aa` |
| +0x08 `[0x4218]` RIP | `0x0` | `0x0000083800000000` |
| +0x10 `[0x4220]` CS | `0x0008` | `0x00000000000c9d51` ← **非法选择子 0x9d51** |
| +0x18 `[0x4228]` RFLAGS | `0x46` | `0x00000000017125aa` |
| +0x20 `[0x4230]` RSP | `0xffff8002dc604240` | `0xffff8002dc604250` |
| +0x28 `[0x4238]` SS | `0x0010` | `0xffff80000002e360` |

> 说明：QA 里"`RIP=0` 但帧 RIP 槽读出非 0"不是矛盾——`#PF` 的 RIP 槽（应=0）**也被同一股写入覆盖了**；全内存都搜不到 `(0x10,0,0x8,0x46)` 这个"原始 #PF 帧"（见 §6），说明帧在镜像定稿时已被覆盖。

### E5 崩溃核栈 = "NVMe 中断 → demux → resched → 嵌套 sched"（GDB `info symbol`）

对崩溃核栈 `0xffff8002dc6042a8…43b8` 逐槽取字并符号化：

```bash
gdb -q -batch -x w22_01.gdb kernel.elf w22_01.core \
  -ex "x/gx 0xffff8002dc6042a8" -ex "x/gx 0xffff8002dc6042c8" \
  -ex "x/gx 0xffff8002dc604308" -ex "x/gx 0xffff8002dc604328" \
  -ex "x/gx 0xffff8002dc604388" -ex "x/gx 0xffff8002dc6043b8" \
  -ex "info symbol 0xffff80000005cd43" -ex "info symbol 0xffff80000005749a" \
  -ex "info symbol 0xffff800000057720" -ex "info symbol 0xffff80000005848c" \
  -ex "info symbol 0xffff800000039ee9" -ex "info symbol 0xffff80000006314e"
```

```
0xffff8002dc6042a8: 0xffff80000005cd43   info symbol -> task::atomic_load() + 91
0xffff8002dc6042c8: 0xffff80000005749a   info symbol -> per_processor_scheduler::sched() + 334
0xffff8002dc604308: 0xffff800000057720   info symbol -> per_processor_scheduler::next_task_with_routine() + 50
0xffff8002dc604328: 0xffff80000005848c   info symbol -> resched + 364
0xffff8002dc604388: 0xffff800000039ee9   info symbol -> idt_vec_demux_entry + 380
0xffff8002dc6043b8: 0xffff80000006314e   info symbol -> NVMe_Controller::interrupt_handle(interrupt_token_t*)
```

自内向外（地址递增 = 外层）：`atomic_load ← sched ← next_task_with_routine ← resched ← idt_vec_demux_entry ← NVMe_Controller::interrupt_handle`。

即 **设备中断处理函数里直接调用 `resched()`，再 `next_task_with_routine()`(= `sleep_tasks_wake`+`sched`)——中断上下文里的整段嵌套调度**。

### E6 源码：每个 NVMe CQ 中断都强制 `resched()`

`src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp:543-551`（`idt_vec_demux_entry` 的默认=设备向量分支）：

```c
default:{
    interrupt_token_t local_tok; { ... local_tok = self->tokens[vec]; }
    if (local_tok.func) {
        uint64_t res = local_tok.func(&local_tok);
        x2apic::x2apic_driver::write_eoi();
        if (res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE)
            resched(raw_frame);          // <<< 中断里直接 resched
    }
    return;
}
```

`src/arch/x86_64/core_hardwares/NVMe/NVMe_interrupts.cpp:115-122`：

```c
uint64_t NVMe_Controller::interrupt_handle(interrupt_token_t *token){
    ...
    dev->cq_interrupt_handler(cqid);
    return 1;                            // 1 == TOKEN_FLAG_MASK_TOKEN_SCHEDULE
}
```

⇒ `interrupt_handle` **恒**返回调度标志 ⇒ **每次 NVMe CQ 中断都触发一次 `resched()`**。

`resched`（`kthread_interfaces.cpp:212`）接着 `kthread_common_save(frame,…)` + `next_task_with_routine()`；`sched()` 末尾 `set_clock_by_offset(20000)` 后 `to_run->atomic_load()`（→ `idt_style_load(&priv_ctx)` 做 `iretq` 落地到目标任务）。触发面 `nvme_parallel_init_all` 同时跑 2 个 worker，`cmd_submit_and_process` 用 `block_if_equal` 阻塞等待、由 CQ 中断唤醒——**收发两侧都压在同一套 `resched/sched` 机器上**。

### E7 `#PF` 帧被 `sched()` 栈帧覆盖（帧被踩的直接形态）

把崩溃核栈与**另外两个核**的同位置栈逐 qword 对齐（崩溃栈 `0x40f210` ↔ 他核 `0x427af0` / `0x42f760`）：

```bash
python3 analysis/…（见 §7 diff 脚本）
```

```
+0x00 0x00000000017125aa  0x00000000017125aa  0x00000000017125aa   (三核全等)
+0x08 0x0000083800000000  0x0000083800000000  0x0000083800000000   (三核全等)
+0x10 0x00000000000c9d51  0x00000000000c9d51  0x00000000000c9d51   (三核全等)
+0x18 0x00000000017125aa  0x00000000017125aa  0x00000000017125aa   (三核全等)
+0x20 <核内栈指针 0x…4250> <…61cb30>          <…6267a0>            (仅指针不同)
```

`0xffff8002dc604218/4220/4228` 这三个"`iretq` 源槽"里的值（`0x838<<32 / 0xc9d51 / 0x17125aa`）**与另外两核栈上一模一样的 `sched()→set_clock_by_offset(20000)` 帧**逐字节相同（`…02e360`=`raw_config_timer_init_count+0x22`、`…4e20`=`20000`、`…02edd9`=`set_clock_by_offset+0x89`、`…05cd43`=`atomic_load`、`…05749a`=`sched+0x14e`、`…05848c`=`resched+0x16c` 等三核全等）。

⇒ 这些值**不是** `#PF` 该有的帧内容（应 `RIP=0 / CS=8 / RFLAGS=0x46 / SS=0x10`），而是 **`sched()` 在中断内的栈帧**；**同一栈上 `sched()` 的栈写入覆盖了 `#PF` 异常帧**——这就是"异常帧 → 返回目标被劫持"的字节级机理，也与 WRAITH"栈/指针被踩"签名一族同源（本例被踩的是整 qword，非单字节）。

> GDB 侧同一事实的体现：`bt` 的 `#1..#5` 恰好就是 `[0x…4218..4238]` 这 5 个被踩槽（`E4①`），且 `#5` 落在 `raw_config_timer_init_count`（即 `sched()→set_clock_by_offset` 路径）—— 互相印证。

### E8 物理地址取数：静态 `[assets_remap]` 表（权威）+ 页表 walk（回退）

`w22_01.serial` 内含 `bootstrap` 打印的 **`[assets_remap]` 静态 v→p 映射表（9 段）**，比运行时页表 walk 更权威。工具升级后 `ram-read.py` 支持 `--assets <serial>`（优先直译）与 `--list`（打印表）：

```bash
cd /mnt/huge_data/sparrowos_debug/traces
python3 /home/PS/PS_git/OS_pj_uefi/kernel/Tools/tcg-trace/ram-read.py w22_01.ram 0x306000 --list --assets w22_01.serial
# kernel_code v=0xffff800000000000 p=0x01200000 size=0x200000
# kernel_data v=0xffff800000200000 p=0x011dd000 size=0x1000
# kernel_rodata v=0xffff800000201000 p=0x01400000 size=0x3ff000
# kernel_bss v=0xffff800000600000 p=0x01800000 size=0x600000
# gop_framebuffer v=0xffff800000c00000 p=0x80000000 size=0x3e8000
# hpet_mmio v=0xffff800000fe8000 p=0xfed00000 size=0x1000
# gs_complexes v=0xffff800000fe9000 p=0x01ec0000 size=0x1e000
# hdstacks v=0xffff800001007000 p=0x01ede000 size=0x10f000
# phyaddr_window v=0xffff800040000000 p=0x00000000 size=0x280000000
```

**交叉校验（符号链 ↔ 物理镜像一致）**：核入口 `_kernel_Init` 的 VA `0xffff8000000053a0`（`sym.log` 首条指令 `49 89 ff …`）经静态表落到 PA `0x12053a0`，读出的字节与 trace 完全一致：

```bash
python3 …/ram-read.py w22_01.ram 0x306000 0xffff8000000053a0 -n 32 --assets w22_01.serial
# => pa=0x12053a0 (assets_remap:kernel_code)  len=32
# 0x0000012053a0  49 89 ff 48 b8 f9 22 01 00 00 80 ff ff bf 77 02
```

**崩溃栈不在静态表内 → 自动回退页表 walk**：`phyaddr_window` 上界 = `0xffff800040000000+0x280000000 = 0xffff8002c0000000`，而崩溃栈 VA `0xffff8002dc604218` **超过**该上界，故静态表不覆盖它；工具自动回退到 4 级页表 walk（CR3=`0x306000`）：

```bash
python3 …/ram-read.py w22_01.ram 0x306000 0xffff8002dc604218 -n 16 --assets w22_01.serial
# => pa=0x40f218 (4KiB 页)  len=16
# 0x00000040f218  00 00 00 00 38 08 00 00 51 9d 0c 00 00 00 00 00   # == 0x0000083800000000, 0x00000000000c9d51
```

⇒ 与 §3-E4/E7 用页表 walk 得到的同一 PA 复核一致（栈页基 PA `0x40f000`，`[0x…4218]=0x838<<32`、`[0x…4220]=0xc9d51`）。**E4 的字节级结论不受取数方式影响**。

---

## 4. WRAITH §3 假设 A–E 逐条评估

| # | 假设 | 本样本判定 | 依据 |
|---|---|---|---|
| **A** | 调度序列非原子 / 可被 IPI 重入；`resched` 嵌套调度 | **被证据支持（最强）** | E5/E6：崩溃核栈 = 设备中断内 `resched→next_task_with_routine→sched`；E7：同一栈上 `sched()` 帧覆盖了活着的 `#PF` 异常帧 ⇒ 栈/帧被重入的调度写踩 |
| **B** | `wakeup_thread` 只 `interrupt_guard`（非跨核互斥） | **无直接证据** | 本样本崩点路径是 `interrupt_handle→resched`，不是 `wakeup_thread`；代码缺陷仍在，但与 w22_01 无因果链证据 |
| **C** | `get_other_scheduler(pid)=&global_schedulers[pid]` 不校验 | **无直接证据（仅潜在）** | 栈上未见越界 pid；`sched()` 里确有 `get_other_scheduler(i)` 无边界校验（源码在），但本样本未抓到越界实例 |
| **D** | 映射/TLB shootdown 跨核协议 | **无证据** | 未在 trace/栈/镜像抓到 shootdown 相关被踩；崩点也在调度链而非映射路径 |
| **E** | NVMe DMA/PRP 误指向 | **无证据** | 未发现 device→错物理地址的搬数痕迹；崩点与 NVMe worker 的 CQ 中断调度相关，非 PRP 打包 |
| — | "异常入口无 IST 自噬成风暴" | 本样本**未**成风暴 | 单 #PF→#GP，未 `#PF/#DF` 回环；`#PF` 用当前栈 |

---

## 5. 下一步可验证实验

1. **结构性防风暴（优先）**：给 `#PF/#DF` 配 **IST 独立栈**，并在 `page_fault_handler` 里把"CS 既非 0 也非 3"当作**内核故障**处理（当前会静默放行，是"首发 #PF 被吞"的放大器）。
2. **断言/留证**：在 `resched()` / `idt_vec_demux_entry` 默认分支入口断言 `frame->core_ctx.idtctx.iret.cs==0x8`、`iret.rip` 在合法内核文本段、`rsp` 落在该核栈区间；命中即 panic 前先落证（绕过单字节 UART）。
3. **收敛嵌套调度**：让"设备中断内 `resched`"这条路径**不可重入**（如每核 `resched_in_progress` 标志 / 整段关中断），或设备中断**只置位调度需求**、真正调度统一在退中断前一次性做，堵掉"中断里再跑整段 sched"。
4. **差分复现**：把 `NVMe_Controller::interrupt_handle` 的返回值从恒 `1` 改为"仅在确需唤醒时"置 `TOKEN_FLAG_MASK_TOKEN_SCHEDULE`（差分 A/B），观察 w22 竞态率是否下降——直接检验 A。
5. **`interrupt_handle` 里 `resched` 的影子栈探针**：在 `sched()` 入口记录 `RSP`，在 `atomic_load()` 前比对本次 `sched` 的 `RSP` 与"当前异常/中断帧基址"是否重叠，量化 E7 的"栈覆盖"。
6. **降载细调**：按 WRAITH §8-1，`yes` 由 12 → 2~4，带 `--dump-mem` 连跑，钓更多"真 WRAITH + 完整 .ram"样本做横向对拍（当前仅 w22_01 的 8G 镜像完整）。

---

## 6. 存疑 / 未决（必须诚实标注）

1. **RIP 变 0 的"上一跳"未能逐指令钉死**：崩溃核最后被日志记录的整块是 `ksystemramcpy.zero`（`0x574e`, `ret`），那是**控制台/Gfx 滚动**（`DmesgRingBuffer::putsk → textConsole → GfxPrim::MoveUp → ksystemramcpy`，见 `resolve_stack` 低页）的常见落点，未必是崩溃核当次的最后一跳（QEMU `-d in_asm` 只记**首次翻译**的 TB，崩溃核关键 TB 早已翻译、不再记录）。`RIP=0` 的候选来源：① `sched()→to_run->atomic_load()→idt_style_load(&priv_ctx)` 的 `iretq` 落到被踩/清零的 `priv_ctx.iret.rip`；② 栈上被踩的返回地址 `ret` 到 0；③ 被踩的间接调用目标。三者当前无决定性证据。
2. **`#PF` 原始帧不复存在**：全 8G 搜不到 `(0x10,0,0x8,0x46)` 原始帧；镜像定稿时 `0xffff8002dc604210` 起 0x30 字节已被 `sched()` 帧覆盖（E7）。因此"CPU 压帧 → 帧被覆盖"的**覆盖者**是否为"同一次崩溃里的嵌套 `sched()`"仍属推断（无法从静态镜像区分"覆盖发生在取帧前"还是"dump 更晚"）。
3. **`#PF` 帧基址-8 的解读**：`#GP` 帧（PA `0x40f1e0`，字节级匹配 panic dump）与 QEMU `-d int` 报告的 `SP` 存在 8 字节约定差；`#PF` 侧我用"`iretq` 的 `CS` 必须来自 `[0x4220]`（否则 `e` 不会=0x9d50）"反推帧基=`0x4210`。GDB `bt`（`#1..#5`）也**直接**把 `[0x…4218..4238]` 当作 `iretq` 的弹出帧打印（栈帧方向自洽），故该反推可信；仅 QEMU `SP` 字面记法需在工具侧再核。
4. **触发与受害的精确因果**：我钉死的是"崩点落在 NVMe CQ 中断→嵌套 `resched/sched`"，以及"异常帧被 `sched()` 栈踩"；但**是"谁的写"踩了谁**（同一核嵌套写的自踩？还是别的核/别的对象）**未到不可反驳**。需要 §5-2/5-5 的运行时探针。

---

## 7. 复现命令清单（只读）

```bash
# ① GDB 后验 core（首选）
cd /mnt/huge_data/sparrowos_debug/traces
gdb -q -batch -x w22_01.gdb /home/PS/PS_git/OS_pj_uefi/kernel/kernel.elf w22_01.core \
    -ex "info registers" -ex "bt" \
    -ex "x/6gx 0xffff8002dc6041e0" -ex "x/5gx 0xffff8002dc604218" \
    -ex "p/x (0xc9d51 & ~0x7)" -ex "p/d 0x9d51 >> 3" \
    -ex "x/8i 0xffff800000037dbb" -ex "x/8xb 0xffff8000000053a0"
# ② 事件时间线/按核归因
python3 analysis/parse_events.py     traces/w22_01.trace
# ③ 静态 [assets_remap] 表（权威）+ 页表 walk 回退
python3 Tools/tcg-trace/ram-read.py  traces/w22_01.ram 0x306000 --list --assets traces/w22_01.serial
python3 Tools/tcg-trace/ram-read.py  traces/w22_01.ram 0x306000 0xffff8000000053a0 -n 32 --assets traces/w22_01.serial
python3 Tools/tcg-trace/ram-read.py  traces/w22_01.ram 0x306000 0xffff8002dc604210 -n 0x30 --walk --ascii --assets traces/w22_01.serial
# ④ 交叉复核（自写脚本）
python3 analysis/resolve_stack.py    0x40f140 0x40f400 0xffff8002dc604140   # nm 栈符号化
python3 analysis/stack_diff.py                                             # 三核栈 diff (E7)
python3 analysis/mem_scan.py                                              # 全内存模式搜索
# ⑤ 关键事件上下文
sed -n '54349,54560p' traces/w22_01.kernel.sym.log
sed -n '80936,81145p' traces/w22_01.kernel.src.log
```

分析脚本全部位于 **`/mnt/huge_data/sparrowos_debug/analysis/`**：`parse_events.py`、`resolve_stack.py`、`mem_scan.py`、`stack_diff.py`。工具 `kernel/Tools/tcg-trace/{ram-read.py, ram-mkcore.py}` 与 `traces/w22_01.{core,gdb}` 均为已有资产；未改动仓库任何源文件（`kernel/` 下的 `M`/`??` 均为 WRAITH 既有会话改动，含未提交的 `[BISECT]` `NVMe_init_thread.cpp`）。
