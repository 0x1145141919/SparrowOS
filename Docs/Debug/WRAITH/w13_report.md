# WRAITH 样本 w13 · 后验根因分析报告

> 对象：`/mnt/huge_data/sparrowos_debug/traces/w13.*`（vmcore/core/serial/trace/符号化日志/map-report）
> 内核：`/home/PS/PS_git/OS_pj_uefi/kernel/kernel.elf`（运行基址 `0xffff800000000000` == ELF vaddr）
> 方法：只读工件 + gdb + 自写页表/物理扫描脚本（见同目录 `*.py`）。不改源码、不跑 QEMU、不提交。
> 结论级别：**关键事实已字节级钉死；"谁写坏了这一个字"未 100% 钉死（给出 top 候选 + 反证）。**

---

## 0. 一页速览

| 项 | 值（vaddr / 物理 / 符号） |
|---|---|
| 首个异常 | `EVENT #882  #PF e=0003 pc=0xffff80000000d289`（唯一一次 #PF） |
| 肇事指令 | `spinlock_cpp_t::lock()+0x33` = `86 02  xchg %al,(%rdx)`（**写**一把锁字节） |
| 写失败地址 | `CR2 = 0xffff80000002C8D8`（`d8 c8 02 00 00 80 ff ff`） |
| 该地址落区 | kernel `.text` 前 2MB（只读可执行）；页表：2MB 大页 `PDE=0x12001a1` → phys `0x1200000`，**W=0** ⇒ 写必 #PF |
| 调用链 | `sleep_tasks_wake`(line171) → `spinlock_interrupt_about_guard::ctor`(0xd2c8) → `lock`(0xd256) |
| 野 `this` | `0xFFFF80000002C700`（`= CR2 - 0x1d8`；`this+0x1d8 == &this->sched_lock`） |
| `this` 的真值 | **CPU0 调度器 = `0xffff8002cc512000`**（GS[5] / `global_schedulers`） |
| `this` 的存放处 | `sleep_tasks_wake` 栈槽 `[rbp-0x268]`：v `0xffff8002dc626578` / phys **`0x2a23578`** / 恒等别名 `0xffff800042a23578` |
| 该槽内容 | `0xffff80000002C700` = **`HPET_driver::get_time_stamp_in_us()+0x9a`**（0x2c6fe 处 `call *%rax` 的**返回地址**） |
| 触发线程 | CPU0 上正跑 **tid6**（`NOW_RUNNING_TASK=&tid6`，state=blocked，`block_if_equal` 路径） |

**一句话**：CPU0 在 `tid6` 调用 `block_if_equal` 阻塞后进入 `next_task_with_routine → sleep_tasks_wake`；该函数**第三条** `g(this->sched_lock)`（line171）读到的 `this` 竟是 `.text` 里的 `0x2c700`，于是对只读代码页 `0x2c8d8` 加锁 → #PF。`this` 的有效值应为 `0xffff8002cc512000`；被塞进去的 `0x2c700` 是一个 **HPET 取时调用的返回地址**——即"某个线程在取时路径上，把栈写进了 CPU0 的帧底"。

---

## 1. 首爆现场（字节级）

```
EVENT #882 v=0e e=0003 i=0 cpl=0 IP=0008:ffff80000000d289 pc=ffff80000000d289
           SP=0010:ffff8002dc626538 CR2=ffff80000002c8d8
RAX=1 RBX=0 RCX=0 RDX=RSI=RDI=ffff80000002c8d8 RBP=ffff8002dc626540 RSP=ffff8002dc626538
R8=ffff8002dc6265a8 R10=0x10286 R11=8 R12=ffff800040409000
GS=0030 ffff800000fe9000   GDT=ffff800000feb801   CR3=0x306000 CR4=0x707e8 CR0=0x80010033(WP=1)
```

- 唯一一次异常：整条 trace 里 `v=0e` **只出现 1 次**（事件 #882）。前 881 个事件全是 `0x20`(定时器)/`0xe2`/`0x21`/`0x22`/`fc`/`fe`（IPI/A PIC）。
- 页表铁证（本机 `kspace_up_half` 走的）：`va=0x2c700/0x2c8d8 → PDPTE[0]=0x301023 → PDE=0x12001a1`（bit7=PS，2MB 大页；**bit1 W=0**，NX=0）→ phys `0x1200000`。
  ⇒ 内核 `.text` 段是 **RX 只读**，`xchg` 写它必触发 `#PF P=1 W=1 U=0`（=e0003）。**与硬编码 CR0.WP=1 一致。**

---

## 2. Q1 — `this(=0x2C700)` 从哪来？

### 2.1 它确实是 `this`，且来自"栈上的保存槽"

`sleep_tasks_wake` 序言：`0x56ecb: mov %rdi,-0x268(%rbp)`（保存 `this`）。每处 `g(this->sched_lock)` 都**重新**读 `-0x268(%rbp)`。帧链（用 RBP 链从 #PF 帧复原，全部自洽）：

| 帧 | rbp(v) | [rbp] | [rbp+8] |
|---|---|---|---|
| `spinlock_cpp_t::lock` | `0xffff8002dc626540` | `0x...626560` | `0xffff80000000d328`(guard ctor) |
| `guard::ctor`(0xd2c8) | `0xffff8002dc626560` | **`0xffff8002dc6267e0`**（=sleep 的 rbp） | **`0xffff8000000570ba`** |
| `sleep_tasks_wake` | `0xffff8002dc6267e0` | `0x...626810` | `0`(见 §5 存疑) |

- `0x570ba` = `sleep_tasks_wake+0x1FA` = **line 171** 那条 `g(this->sched_lock)` 的 call 返回址（用 `objdump -dl` 对源码行确认：151=第一条，164=task_lock，**171=第三条**）。
- sleep 帧底 rsp=`0x626570`；**保存槽 `[rbp-0x268]=0x626578`** 就在帧底上方 8B，内容是 `0xffff80000002c700`。

### 2.2 真值 vs 野值

- 真值：CPU0 的 `per_processor_scheduler` = `0xffff8002cc512000`
  （GS 槽 5 = `0xffff8002cc512000`；`global_schedulers` 指针 = `0xffff8002cc512000`；`get_other_scheduler(0)` 同值）。
  旁证：同在 tid3 栈（phys `0x41c000`）上，`guard` 对象里成串出现 `0xffff8002cc512000` 与 `0xffff8002cc5121d8`(=`&sched[0].sched_lock`)——这是**正常** sleep/sched 帧的形态。
- 野值：`0x2c700` 在整条 tid6 栈里**只出现 1 次**（就在保存槽）。`nm -C` 命中 `HPET_driver::get_time_stamp_in_us()+0x9a`；`objdump` 显示它正是 `0x2c6fe: call *%rax`（目标 0x57d6）之后的**返回地址**（`0x2c700: mov -0x8(%rbp),%rbx`）。
- 全物理 RAM 扫该 8 字节字串：**19 处**，其中包括**每一条 task 栈各 1 处**（见 §4 表），以及多枚 hdstack。→ 这个值不是随机垃圾，而是"**取时路径（ktime→HPET）的返回地址**"这一天然栈值；凡是有线程执行过 `ktime::get_microsecond_stamp() → HPET::get_time_stamp_in_us()` 的栈上都会留下它。

⇒ 判定：**不是"某个 task/scheduler 指针被踩后派生"**（task 表、`global_schedulers`、GS 槽都完好，见 §3/§4；`get_other_scheduler(pid)=base+pid*0x200` 对任何 pid 都得不到 `0x2c700`）。而是 **`sleep_tasks_wake` 自己的保存-`this`栈槽，在函数执行途中被"越界/别名/跨核"的一次取时调用返回地址覆盖**。

### 2.3 为什么断定是"函数执行途中被覆盖"，而不是"入口就是野 this"

若入口 `this` 就是 `0x2c700`，则**第一条** `g(this->sched_lock)`（line151，call 返回址应为 `0x56f49`）就会先 #PF；先崩。实测崩在**第三条**（返回址 `0x570ba`），说明 line151 那次 `this` 是好值、加锁也成功了。⇒ 覆盖发生在**同一次调用**的 `line151 之后、line171 之前**——即**批量处理窗口**：

```
line151 guard(sched_lock)      // 此时 this 仍正确（否则先崩）
  ... 收集到 batch[] ...
line164 guard(batch[i]->task_lock);  // 该段之间 IF 是开的
line166 batch[i]->set_ready();       // set_ready/task_event_shift 会读时间(ktime→HPET)
line171 guard(sched_lock)      // ← 此时 [rbp-0x268] 已变成 0x2c700 → 崩
```

### 2.4 覆盖的"形态"（它是怎么发生的）

`0x626578` 往下的 0x30 字节，与"**ktime/HPET 取时帧**"的形态完全一致（在每一条 task 栈上都能找到同款）：

```
[+0x00] 0xffff80000002c700      // HPET 内层 call 的返回址
[+0x08] 0x36 (0x0/0x1/0x6/0x3a) // 各栈不同的小值
[+0x10] 0x86 (0x82)             // 小值
[+0x18] 0x0009xxxxxxxxxx00      // 形如时间戳/计数
[+0x20] 0x86 (0x82)
[+0x28] 0xffff8002dc62xxxx      // 指向本帧内的指针
   （再往前 -0x38 处有 0x3b9aca00=1e9，-0x10~-0x28 是 2^54-1/页掩码等 —— 典型的 ns↔us 换算帧）
```

即：**写进去的不是一个指针，而是一段"取时帧"**——覆盖点恰是 `sleep_tasks_wake` 帧底 `[rbp-0x268]`，写成值 `0x2c700`。

覆盖者的栈指针在此刻只能是 **v `0x626580`**（因为 `0x2c700` 是 `call` 压入的返回址，落在 `rsp_at_call-8`）。
但当前帧链里**没有任何活动帧 rsp=0x626580**（它在 sleep 帧底 `0x626570` 的**上方** 0x10，正常调用只会向下压栈）。
⇒ 这是一次**不遵循当前栈指针的写**（野 RSP / 页表别名 / 跨核写栈）。
⇒ 与 WRAITH「跨核/栈单字损坏」的既有画像一致；触发窗口与假设 A（调度序列跨窗口、段间开中断）吻合。

> 补充：tid6 栈页 phys `0x2a20000` 被**双重映射**（task 栈 v `0xffff8002dc623000` + 恒等窗口 v `0xffff800042a20000`，两者都 W=1）。这是 `phyaddr_window` 的正常别名，**本身不足以**造成此写，但它确实是"写栈点可被别名命中"的土壤。同物理页未被别的 task 栈复用（已查 §4）。

---

## 3. Q2 — 走 `task_pool::m_tree`：7 条 task 是否自洽？

`m_tree`（符号 `0xffff800000637090`，vptr@0，root@+8）root=`0xffff8002c0200718`；中序得 **7 个节点 tid0..tid6**（无重复、无缺号；`task` 大小 408，节点 480）。逐条解码（字段偏移用 gdb DWARF `ptype /o task` 核对：priv_ctx.core_ctx.idtctx.iret.rip@152 / rsp@176 / cs@160；priv_stack_base@208；pages@216；state@240；tid@248；task=408B，RBNode=480B）：

| tid | node | state | event | bpid | stack_base..+pages*4K | ctx.rip | ctx.rsp | rsp 在栈内 | 队列归属 |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 0x…c0200538 | running | run_kthread | 3 | 0x…dc601000..605000 | `0x574e`(ksystemramcpy.zero) | 0x…dc604608 | ✅ | 运行于 CPU3 |
| 1 | 0x…c0200718 | ready | offline | 2 | 0x…dc606000..60a000 | `0x5ce91`(kthread_self_blocked+7) | 0x…dc609778 | ✅ | **在 sched[2].ready_queue** ✅ |
| 2 | 0x…c02008f8 | running | run_kthread | 1 | 0x…dc60b000..60f000 | `0x5b75`(microsecond_polling+0x27) | 0x…dc60ede0 | ✅ | 运行于 CPU1 |
| 3 | 0x…c0200ad8 | blocked | sleep | 0 | 0x…dc610000..614000 | `0x5ce9a`(kthread_sleep+7) | 0x…dc613d38 | ✅ | **在 sched[0].sleep_queue** ✅（minwake 7 326 568） |
| 4 | 0x…c4200080 | blocked | init | 4 | 0x…dc619000..61d000 | `0x5ceb5`(block_if_equal+7) | 0x…dc61cf48 | ✅ | block_queue 上（不在调度 sleep_q） |
| 5 | 0x…c4200740 | running | run_kthread | 2 | 0x…dc61e000..622000 | `0x5ceb5`(block_if_equal+7) | 0x…dc621c78 | ✅ | 运行于 CPU2 |
| 6 | 0x…c2200018 | blocked | init | 4 | 0x…dc623000..627000 | `0x5ceb5`(block_if_equal+7) | 0x…dc626bb8 | ✅ | block_queue 上（minwake 7 626 414 = 阻塞于 panic 前 12.3 ms） |

结论：
- **全部自洽**：7 条 task 的 `rsp` 都落在自己的 `[priv_stack_base, +pages*4K)` 内；`priv_stack_base` 都在被精细映射的栈territory；**没有任何一条 task 的私栈或 `priv_ctx` 指向 `.text` / `0x2C700` 邻域**（`ctx.rip` 全在 `0x57xx/0x5bxx/0x5cxxx` 的合法函数/存根里）。
- 调度器侧也对得上：`sched[0].sleep_queue` 恰含 tid3；`sched[2].ready_queue` 恰含 tid1；其余队列空；idle 任务在各自 `sched[i]+0`。
- **task 表 + 调度器结构不是野 `this` 的源头**。
- 唯一的"状态/上下文不一致"是：CPU0 的 `NOW_RUNNING_TASK=&tid6`，而 tid6 已 `blocked`——这**正是** `block_if_equal → next_task_with_routine` 尚未走到 `sched()` 完成切换的正常中间态（tid6 阻塞后、切走前），不是 bug。

---

## 4. 关键佐证：`0x2c700` 在物理内存里的分布

经 `w13.vmcore` 全 DRAM 扫（脚本 `phys.py`）：共 **19 处**，其中每一条 task 栈各 1 处（另一类天然"取时帧"残迹）：

| tid | 物理 | 栈内偏移 | 对应 vaddr |
|---|---|---|---|
| 0 | 0x40f028 | 0x3028 | 0x…dc604028 |
| 1 | 0x417198 | 0x3198 | 0x…dc609198 |
| 2 | 0x41b808 | 0x3808 | 0x…dc60e808 |
| 3 | 0x41f748 | 0x3748 | 0x…dc613748 |
| 4 | 0x3203908 | 0x3908 | 0x…dc61c908 |
| 5 | 0x3403638 | 0x3638 | 0x…dc621638 |
| 6 | **0x2a23578** | 0x3578 | **0x…dc626578** ← 就是本次的保存槽 |
| hd | 0x1ee6bc8/0x1f0a9e8/0x1f139e8/0x1f379d8/0x1f379e8/0x1f409e8/0x1f649e8/0x1f6d9e8/0x1f9a9e8/0x1fbe9d8/0x1fc79e8/0x1feb9d8 | — | hdstack 区 |

⇒ `0x2c700` 是**取时路径的天然栈值**（不是 magic、kernel.elf 里也搜不到该常量），
  所以在"某次取时调用把返回址压到了 CPU0 帧底"这条解释下，值自然就是 `0x2c700`。

---

## 5. Q3 — 三件事对账：`this` 野值 / 被踩的锁指针 / task 表异常

- **`this` 野值** `0x2c700`：来源 = `sleep_tasks_wake` 帧底保存槽被"取时调用返回址"覆盖（§2）。
- **被踩的锁指针** `0x2c8d8`：**不是独立的损坏**，而是 `this+0x1d8` 的机械派生（`0x2c700+0x1d8`）。即"锁指针被踩"是 `this` 被踩的**后果**，二者同源。
- **task 表**：完好（§3），**无**指向 `.text` 的 task/栈指针。⇒ 野值不是从 task 表派生的。

**假设判定：**

| 假设 | 本样本证据 |
|---|---|
| **A**（调度序列非原子 / 段间开中断被 IPI 重入） | **吻合触发窗口**：崩点落在 `sleep_tasks_wake` 两把 `sched_lock` 之间的开中断窗口（line151 后、line171 前），且该段正好跑 `set_ready()`（读时间）。但需注意：重入本身不能"向上"改写本帧槽；真正成因是"**越界/别名/跨核的栈写**"。 |
| **B**（`wakeup_thread` 只关本核 IF） | 与本样本无直接冲突，无独立证据。 |
| **C**（`get_other_scheduler` 无边界校验 → 跨核偷取） | **不被支持**：`global_schedulers=0xffff8002cc512000` 完好；`get_other_scheduler(pid)=base+pid*0x200`，任何 pid（含越界）都**得不到** `0x2c700`；野 `this` 也不是 `&global_schedulers[pid]` 的形态。 |
| **D/E**（TLB shootdown / DMA） | 弱相关：本样本没有 shootdown/DMA 直接指纹；但 `phyaddr_window` 恒等别名让"栈页"存在第二个可写 VA，属易被误踩的土壤，值得并入观察。 |

**倾向**：w13 属于 WRAITH 的"**栈单字损坏 → 受害者是调度器锁**"族（与 hp04「`#PF@unlock ← sleep_tasks_wake`」同族）。触发面/窗口与 A 一致，但**根因是"写了 CPU0 栈的一次越界/别名/跨核写"**，而非 C 的"无校验索引派生野 scheduler"。

---

## 6. 未钉死 / 最大存疑

1. **"是谁写坏了 `0x2a23578` 这个字"未 100% 钉死**。已知：写入值是取时调用返回址、写入不遵循当前 rsp（§2.4）。候选：
   - (a) **其它 CPU 的野 RSP**：某线程栈指针被带偏到 `…dc626580`，其 `ktime→HPET` 内层 `call` 把返回址压进 CPU0 栈 → 最贴合"值=0x2c700 + 不遵循本 rsp"两点。
   - (b) **页表别名/写栈错位**：`phyaddr_window` 恒等别名使得"栈页"有第二条 W=1 通道（v `0xffff800042a23578`）；若某处用别名基址算错偏移，同样能命中。
   - (c) 同核重入（A）：**几何上不成立**（重入帧只会更低），故不足以单独解释，但可作为"开中断窗口"的解释。
2. `sleep_tasks_wake` 帧的**返回地址槽 `[rbp+8]` 读出为 0**（正常应为 `next_task_with_routine` 内的 `0x5770d`），且该值在整栈中不存在——帧上方已被更早的上下文切换/取时残帧混叠，**帧链上半段不可信**（不影响 §2 的结论：帧底保存槽 + 三条 guard 的返回址都是本帧当场写入的）。
3. `kspace_vm_table`（28 节点）与页表**存在可见分歧**：`0xffff8002cc200000..cc500000` 在树里是 `MAP_NONE`（占位），页表却有 phys（`0x500000/0x25f8000/0x2a00000…`）。按你给的"占位语义"，这**可能是设计**，但其"当 oracle 有假阳"的风险在本样本确认存在，需另立条目跟踪。
4. 守护页被**映射到越界物理**（如 `v 0xffff8002dc627000 → phys 0xc000008000`），靠 UC 缓存位规避——与本次 #PF 无关，但属可疑映射，建议记档。

---

## 7. 证据命令（可复跑，只读）

```bash
# 1) 崩溃现场 + 帧链
gdb -q -batch kernel.elf w13.core -ex 'info registers' \
    -ex 'x/3i 0xffff80000000d289' -ex 'x/16gx 0xffff8002dc626520'
# 2) 反汇编（源码行对齐）
objdump -dl --start-address=0xffff800000056ec0 --stop-address=0xffff800000057150 kernel.elf
objdump -d  --start-address=0xffff80000002c6c0 --stop-address=0xffff80000002c720 kernel.elf
# 3) task 表 / GS / 调度器 / vm_table  →  见本目录 walk.py / phys.py / walkva.py / scan2.py / scan3.py
gdb -q -batch -ex 'source walk.py' kernel.elf w13.core
python3 walkva.py      # 页表 walk（读 vmcore 物理）
python3 phys.py        # 全 DRAM 扫 0x2c700 / 读栈页
```

同目录脚本：`walk.py`(task 表+GS+调度器) · `walkva.py`(页表 walk) · `phys.py`(物理读+全 RAM 扫) · `scan2/3.py`(栈上目标字串定位)。

---

### 附：与既有样本族的关系
w13 的首爆族与 `hp04`（`#PF@spinlock_cpp_t::unlock ← sleep_tasks_wake`）**同族**；本例更"干净"——唯一一次 #PF、`this` 野值/锁地址/CR2/GS/页表全链条可复核，是"栈单字被取时返回址覆盖 → 命中调度器锁 → 只读代码页写"的**教科书式样本**。w04（HPET 受害者 + RIP 进数据）、w11/w21 亦属同一条"NVMe 并行初始化 × 取时热点"引爆面。

---

## 附录 A · 新批次档位（`-d cpu` 级，2026-09-16 定）

背景：in_asm 是"翻译即记"，对已持有源码/ELF 的场景冗余；升级到 **`-d cpu` 级**（每 TB 执行附带整份寄存器），用 guest PC + kernel.elf 反查代码。**本批 w13 的 trace/mem_dump 作废，本报告与脚本保留。**

### A.1 日志类别
```
-D <tag>.trace -d exec,cpu,nochain,int,guest_errors,unimp,cpu_reset,pcall
```
- **去掉 `in_asm`**（PC 用 `nm/objdump` 反查，见 `exec-sym.py`）。
- `exec,cpu` 必须成对：`exec` 出 `Trace` 行，`cpu` 出其后紧跟的寄存器块；判据同源（`CPU_LOG_TB_CPU|CPU_LOG_EXEC`）。
- **`nochain`**：否则链式 TB 不全（会看到 `Stopped execution of TB chain before ...`）。

### A.2 `Trace` 行字段（本机 QEMU 11.1.1 `log_cpu_exec()`）
`Trace <vCPU#>: <host_tb> [<cs_base>/<guest_pc>/<tb.flags>/<tb.cflags>] <symbol|hex>`
- 粒度 = **一次 TB 执行**；`-dfilter <lo>+<size>` 按 **guest_pc** 过滤（同时门控 cpu 块）。

### A.3 体量与观测效应（必须先量）
- `-d cpu` ≈ 1KB/TB；boot→PANIC 的 TB 执行数 ≈ 10⁷–10⁸ ⇒ **单跑数十~上百 GB**；日志本身拖慢 TCG，**可能压掉竞态**。
- 对照协议：baseline 档 / 新档 各跑 N 次，比异常率；记录宿主负载（WRAITH 已知关键变量）。**新档若 p≈0，说明档位把 bug 压没了**，需降档或换 `-accel tcg,thread=single` 对照。

### A.4 工具
- `trace-hi.sh`（默认 `MODE=cpu`，无 in_asm；`--dfilter/--keep-in-asm/--no-cap/--thread/--dry-run`）
- `exec-sym.py`：把 `Trace...`(+寄存器块) 符号化成 `cpu / guest_pc / pc_sym / rip / rip_sym / rsp / gs_base`。
- `w13-watch.so`：需要直接抓「谁写了某地址」时用（mem 回调带 cpu/pc/vaddr/value）。

### A.5 缺什么
`exec/cpu` 仍**不含访存地址/写入值** ⇒ 单靠它定位不到"踩栈那一笔 store"；那一笔仍需 plugin 的 mem 回调或 gdb watchpoint。exec/cpu 的作用是**给出"哪核·何时·执行了哪条 TB（+寄存器）"的时序**。
