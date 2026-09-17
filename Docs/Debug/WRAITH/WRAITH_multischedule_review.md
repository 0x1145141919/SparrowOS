# WRAITH 穷尽排查 — 「双调度 / 多调度」风险路径与时序窗口

- **日期**：2026-09-17
- **性质**：**纯静态**（只读源码/文档 + 只读工具阅读）。**未运行任何被测代码**：没插电、没跑 QEMU/tcg-trace/selfcheck、没 make/ninja、没改内核源码。
- **输入**：样本报告 `analysis/w22_01_report.md`、`analysis/w13/w13_report.md`；战报 `Docs/Debug/WRAITH.md`；源码树 `/home/PS/PS_git/OS_pj_uefi/kernel`（HEAD `50f6c0e` 附近）。
- **目标**：穷举「**同一 task 重叠地在 ≥2 个核上被执行 / 被 ≥2 个核的调度器同时认领**」以及「**调度重入（sched 套 sched，含 IRQ 中 resched→sched）**」的所有代码路径 + 精确 `file:line` + 最小交错时序。
- **裁定原则**：把已钉死的三枚样本事实当**硬约束**，收窄到能同时解释它们的机制：
  - w22：`NVMe CQ 中断 → idt_vec_demux_entry(默认分支) → resched(raw_frame) → next_task_with_routine → sched`；`#PF@RIP=0` 首爆、异常帧被同栈 `sched()` 覆盖；**中断上下文里跑整段调度**。
  - w13：`sleep_tasks_wake` 帧底 saved-`this` 槽 `[rbp-0x268]` 被「取时帧」覆写 → 野 `this=0x2C700`（= `HPET_driver::get_time_stamp_in_us()+0x9a` 的内层 `call` 返回址）；**写不遵循本核 `rsp`**。
  - wh02 / wl02：同 HPET 函数 `regs`/`this` 被换；**CPU0/CPU1 双核同时中招**。

---

## 0. 结论（一段）

**「双调度/多调度」的真实形态不是"两个核同时 `sched()` 到同一 task 并双双 `set_running`"**（那会在 `per_processor_scheduler.cpp:274-275` 的 `set_running` 失败处 panic，样本都没崩在那），**而是"阻塞/退出者在自己的栈上先把状态发布出去、但它还没真正切走"的瞬态窗口**里，另一个核通过唤醒 + `sched()` 跨核偷取，把**同一个 task（同一片内核栈）**恢复执行。于是**同一物理栈被两核同时压栈**——较浅那一路（异核恢复者）的 `call` 返回址，正好落进较深那一路（原核仍在 `sleep_tasks_wake`/`sched` 帧里）的帧内槽。这精确解释 w13 的 `0x2C700`、wh02 的"双核同时"、w22 的"异常帧被 `sched()` 帧覆盖"，并与「NVMe 并行 init × 取时热点 × 宿主负载敏感 × KVM 压没」完全吻合。

代码级缺陷是**三类结构性缺口**叠加：
1. **发布即唤醒、先发布后切走**：`block_if_equal` / `kthread_sleep` / `kthread_self_blocked` / `kthread_exit` 都在**自己的栈上**改状态并（可选）入队，**之后**才 `next_task_with_routine()`；期间没有任何"我还没离开这片栈"的握手。
2. **唤醒不查"谁正在跑"**：`bq_flush_pending`（`bq_system.cpp:203-228`）对任意 task 无条件 `set_ready()`+入队；而 `task::set_ready()`（`task.cpp:27-35`）**允许 `running→ready`**——`task_state` 这个单字节**根本无法表达"正被别核执行"**。
3. **`sched()` 跨核偷取无 owner 校验**：`per_processor_scheduler.cpp:246-259` 抢别的核 ready 队列，`:263-293` 只靠 `set_running`（`ready→running`）当唯一闸门，**不校验"该 task 是否已被别核占用"**。

另外发现两条**同型但独立**的高危路径：`kthread_exit set_zombie` **先发布** + 别核 `release_kthread` **立刻释放栈**（栈 UAF → 物理页被双重当栈分配）；以及 `sched()→atomic_load()` **在 `task_lock` 之外**读 `priv_ctx`，与并发 `kthread_common_save` 的**整结构体赋值**竞态（撕裂 iret 帧 → `RIP=0` 型跳飞）。并确认 `cq_interrupt_handler` 的 `clamp` **跨轮不复位**（`NVMe_interrupts.cpp:176-187`）在 >64 等待者时产生**重复入队 / `arr[64]` 越界**（潜伏，NVMe 单 CQ 仅 2 waiter，故当前未显形）。

**诚实边界**：静态能证明"存在哪条窗口、窗口由谁打开、写不遵循本核 rsp"，**不能**证明"某次崩溃里确切是哪两个核"；`task_state` 无原子/无 owner 使"同任务几核在跑"**只有靠运行时探针**才能定量。§5 给出逐条对位的探针与断言。

---

## 1. 判据与模型（先把"什么算双调度"钉死）

### 1.1 task 状态机（`task.cpp`，单字节、无原子、无 owner）

```
set_ready()  : init | blocked | running  → ready      // ⚠️ running→ready 合法（task.cpp:27-35）
set_running(): ready                     → running     // 唯一"占用"闸门
set_blocked(): running                   → blocked
set_zombie() : running | blocked | ready → zombie
set_dead()   : zombie                    → dead
```

- `task_state` 是 `uint8_t`，`get_state()`/各 setter **全裸读写**（无 `atomic`），本身就是 data-race。
- **没有任何字段表达"这个 task 当前正被哪个核执行 / 是否在队列里"**：`belonged_processor_id`（`task.h:38`）只在 `sched():276` 被赋值，语义是"上次跑的核"，**不是所有权**。
- ⇒ 只要一个核把某 task 从 `running` 翻成 `ready` 或 `blocked`，**第二个核就立刻获得"可以认领它"的资格**，而第一个核**可能还在它的栈上跑**。

### 1.2 栈与恢复点几何

- 每个 task 有独立内核栈 `priv_stack_base[0 .. priv_stack_pages*4K)`，恢复点 `priv_ctx.core_ctx.idtctx.iret.rsp`。
- `kthread_sleep/block/yield` 走 `int 226`（软中断），`kthread_common_save` 把 **`int` 现场的整帧**存进 `task->priv_ctx`；恢复时 `idt_style_load(&priv_ctx)`（`task.cpp:111-126`）`iretq` 回到 **`int` 之后**（asm `ret` 处）——**这是栈的较浅位置**。
- 于是：**原核 A 此刻在 `next_task_with_routine→sleep_tasks_wake→sched`（更深，rsp 更低）；被异核 B 恢复的同一 task 从较浅恢复点起跑，向下压栈** → 两核在同一片物理栈上"对撞"。**这就是 w13 里"写不遵循 A 的 rsp、值又是 `call` 返回址"的唯一自洽几何**。

### 1.3 中断门与同核重入

- 向量 32–255 全部 `type=0xE`（中断门，`x86_vecs_deliver_mgr.cpp:113-116`）⇒ 入口即 `IF=0`。`int 226` 的调度软中断亦然。
- `spinlock_interrupt_about_guard`（`lock.cpp:61-84`）语义 = **保存当前 IF → `cli` → 取锁**；析构时**恢复**原 IF。
  ⇒ 在 `int 226` / 设备中断路径里，`next_task_with_routine` 内部的所有 `g(this->sched_lock)` **全程 `IF=0`**，**同核**不会因"两把 sched_lock 之间的窗口"重入。
- ⇒ **结论：同核重入被中断门挡住；真正的并发是"跨核"，且窗口由"发布→切走"的间隔决定，不受 IF 保护。**

### 1.4 锁序（源码注释声明）

`bq_system.cpp:7` 声明：`bq_lock(qlock) > task_lock > sched_lock`；`NVMe_interrupts.cpp:5` 声明：`cq_wq_lock(=qlock) > sq_lock`；`bq_system.cpp` 头部容器锁 `container_lock` 在读/写 BQ 池时最外层。**逐点核对未发现"顺序反转"**（详见 §4.4）；缺陷是**粒度**（多资源更新被拆成三个各自原子的临界区），不是死锁序。

---

## 2. 路径清单表（按 危害 × 可复现性 排序）

> 证据强度：**【字节级】**=样本镜像已钉死；**【代码级】**=源码可逐行指出窗口；**【推断】**=需运行时确认。
> 危害：`同栈并发`（同一内核栈被两核压栈）/ `重复执行`（同 task 两次 running）/ `栈 UAF` / `队列串扰` / `丢任务`。

| 优先级 | 编号 | 路径名 | 精确位置 | 触发条件 | 需要的时间窗口 | 危害 | 证据 |
|---|---|---|---|---|---|---|---|
| **P0** | **MS-1** | **阻塞者未切走 → 异核 `bq_flush_pending` 唤醒 → 异核 `sched` 偷取 → 同栈** | 发布：`kthread_interfaces.cpp:383-425`（`set_blocked`@410→`push_tail`@417→`next_task_with_routine`@424）；唤醒：`bq_system.cpp:197-228`（`set_ready`@208、`get_other_scheduler`@216、`insert`@218）；偷取：`per_processor_scheduler.cpp:246-259`、`set_running`@274 | NVMe worker 用 `block_if_equal` 等 CQ；CQ 中断打在**别的核** B 上（`interrupt_handle` 恒返 `TOKEN_FLAG_MASK_TOKEN_SCHEDULE`，`NVMe_interrupts.cpp:114-122`），B 立即 `resched` | A 在 `push_tail`(@417) 之后、**到 A 的 `sched():293 atomic_load()`** 之前；B 只要在此区间完成"pop→set_ready→insert→偷取→`atomic_load`" | **同栈并发**（w13 的 `0x2C700` 直接产物）；重复执行 | **【字节级+代码级】** w13 |
| **P0** | **MS-2** | **设备中断里 `resched` 整段调度 + 自身入 ready → 异核偷取 → 同栈 / 异常帧被 `sched()` 覆盖** | `x86_vecs_deliver_mgr.cpp:566-588`（默认分支 `resched(raw_frame)`@584）；`kthread_interfaces.cpp:238-300`（`resched`：save@289→`set_ready`@291→insert@297→`next_task`@299）；`per_processor_scheduler.cpp:226-294`（`sched`/`atomic_load`@293） | 任意设备向量（NVMe CQ、i8042、HPET…）中断；`interrupt_handle` 恒置调度位 | A 在 `insert_ready_task(interrupted_task)`(@297) 之后、`atomic_load()`(@293) 之前；B 偷取该 task（它已在 A 的 ready 队列，**`push_back` 在队尾**，A 自己的 `sched` 很可能先取别的任务，把它留给 B） | **同栈并发**；异常帧被 `sched()` 帧覆盖（w22） | **【字节级+代码级】** w22 |
| **P1** | **MS-3** | **退出者 `set_zombie` 先发布 → 异核 `release_kthread` 立刻释放栈 → 栈 UAF / 物理页被二次分配** | 发布：`kthread_interfaces.cpp:301-312`（`set_zombie`@308→`next_task`@311）；释放：`kthread_interfaces.cpp:494-517`（`release_kthread`：`get_by_tid`→查 zombie→`__wrapped_pgs_vfree(priv_stack_base)`@509）；观察：`task_pool.cpp:73-88`（`zombie_observe` 见 zombie 即 `ZOMBIE_DEAD`） | NVMe `nvme_parallel_init_all` 主线程 `zombie_observe` 轮询 + `release_kthread`（`NVMe_init_thread.cpp:250-267`）；worker 走 `allkthread_true_enter`→`kthread_exit` | worker 在 `set_zombie()`(@308) 之后、到它的 `atomic_load()` 切走之前；reaper 在此区看到 `zombie` 并 `vfree` | **栈 UAF**：退出者随后仍要在该栈上跑完整段 `next_task_with_routine→sleep_tasks_wake→sched`（**压栈近百字节**）；页被 `stack_alloc` 复用 → **同型"两 task 共用物理栈页"** | **【代码级】** |
| **P1** | **MS-4** | **`kthread_self_blocked` 纯握手缺失：`set_blocked` 先发布 → 异核 `wakeup_thread` 顶上 → 同栈** | 发布：`kthread_interfaces.cpp:313-324`（`set_blocked`@320→`next_task`@323）；唤醒：`kthread_interfaces.cpp:325-362`（`wakeup_thread`：`set_ready`@351、`insert_ready_task`@355） | 任何 `wakeup_thread(tid)`（`PortDriver.cpp:47/75/101`、`textConsole.cpp:407` 等）在另一核上，对本 task 调用；此时 `on_blockers_queue_bit==false`，**状态检查 `blocked` 通过** | A 在 `set_blocked()`(@320) 之后、到 `atomic_load()` 之前；B 在此区 `set_ready`+入队+偷取 | **同栈并发** | **【代码级】** |
| **P1** | **MS-5** | **睡眠者入 `sleep_queue` 后未切走；同核 `sleep_tasks_wake` 回收 + 异核偷取** | 发布：`kthread_interfaces.cpp:363-382`（`set_blocked`@371、`sleep_queue.insert`@377、`next_task`@381）；回收：`per_processor_scheduler.cpp:142-225`（`set_ready`@201、`insert_ready_task`@218）；偷取：`:246-259` | `kthread_sleep(offset)`（BSP 高频 `poll_report_board` 的每轮 `kthread_sleep(poll_us)`，`NVMe_init_thread.cpp:328`）；`offset` 小或 `min_wakeup_stamp<=now` 时同核立即回收 | A 在 `sleep_queue.insert`(@377) 之后、`sched():293` 之前：A 自己的 `sleep_tasks_wake`（本轮）或下一次调度把它 `set_ready`+进 A 的 ready → B 偷取 | **同栈并发**；`offset<=0` 时可有"同核立即回插"的伪唤醒 | **【代码级】** |
| **P2** | **MS-6** | **`sched()` 在 `task_lock` 之外 `atomic_load()` 读 `priv_ctx`，与并发 `kthread_common_save` 的整结构体赋值竞态 → 撕裂 iret 帧** | 读：`per_processor_scheduler.cpp:272-293`（`g1` 作用域 **`}`@291 关闭**，`set_clock_by_offset`@292、`atomic_load()`@293 在锁外）；`task::atomic_load`→`idt_style_load(&priv_ctx)`（`task.cpp:111-126`、`yield.asm:20-38`）；写：`kthread_common_save` 的 `task_ptr->priv_ctx = *frame`（`kthread_interfaces.cpp:154-158`） | 任一 task 处于"被本核选中执行"与"被别核 `save`/重入队"重叠 | `sched` 释放 `to_run->task_lock`(@291) 后、`iretq` 前；另一核同时对同一 `to_run` 执行 `kthread_common_save`（大结构体逐字赋值，非原子） | **撕裂上下文** → `iretq` 到野 `rip/cs/rsp`（**`RIP=0` 型跳飞的直接候选**） | **【代码级/推断】** |
| **P2** | **MS-7** | **`set_ready()` 允许 `running→ready`：状态机无法表达"正被别核执行"** | `task.cpp:27-35` | 任何对**正在别核运行**的 task 调 `set_ready`（现实入口：`bq_flush_pending:208`） | 只要 task 在别核 running 的任意时刻 | 打开 MS-1/MS-2/MS-4 的"第二所有者"闸门 | **【代码级】** |
| **P2** | **MS-8** | **`insert_ready_task` 只查 `state==ready`：不查 `on_blockers_queue_bit` / 是否已在队列 / 是否 running 在别核 → 重复入队** | `per_processor_scheduler.cpp:295-325`（唯一校验 `get_state()!=ready`→fail @304-307）；`list_doubly::push_back` **每次 `new node()`，不查重**（`Ktemplats.h:237-250`） | 同一 task 被两次 `insert_ready_task`（MS-10 的重复 flush、或两路唤醒） | 第二次 insert 时 task 仍为 `ready` | **重复执行**（两核各 pop 一个节点 → 第二个 `set_running` 失败 panic，或"一个 running 一个 ready"错象） | **【代码级】** |
| **P2** | **MS-9** | **`bq_flush_pending` 是唯一"不查运行核"的唤醒者** | `bq_system.cpp:203-211`（`t->set_ready();` **返回值被丢弃**，无 `running` 判定） | 见 MS-1 | 见 MS-1 | 把 running 的 task 翻成 ready 并入队（MS-7+MS-8 的使能项） | **【代码级】** |
| **P3** | **MS-10** | **`cq_interrupt_handler` 的 `clamp` 跨轮不复位：内层循环 + 外层重复 flush → 重复入队 / `arr[64]` 越界** | `NVMe_interrupts.cpp:129-188`（内层 `while(true){pop_all; if empty break; flush}`@176-182；**外层再次 `bq_flush_pending(&clamp)`@185-187**）；`bq_system.cpp:181-191`（`pop_all`：`arr[batch_count++]` **先写后判**，`>=64` 才 break）；`bq_system.h:9`（`arr[64]`） | 单个 block_queue **同一时刻 >64 个 waiter**（NVMe 单 CQ 仅 2 worker ⇒ **当前不显形**，但 i8042/text 订阅队列 + 未来并发可能触发） | 第 65 个 waiter 被 `pop_all` 写入 `arr[64]`（**越界 8B**）；或内层已 flush 的项被外层再次 `set_ready`+`insert`（重复节点） | **队列串扰/越界写**（写穿到 `cq_interrupt_handler` 栈上相邻槽）；**重复执行** | **【代码级】**（可达性=**推断**） |
| **P3** | **MS-11** | **`get_other_scheduler(pid)` 无边界校验；`pid` 来自 `task.belonged_processor_id`（task 可控/可腐）** | `per_processor_scheduler.cpp:369-372`；调用点 `bq_system.cpp:216`、`kthread_interfaces.cpp:341`、`:365`、`:169` | `belonged_processor_id` 被踩成 ≥ `logical_processor_count` 的野值 | 任意 | **越界读/写**：`&global_schedulers[pid]` = `base + pid*sizeof(scheduler)`；随后 `insert_ready_task` 会在该地址**链链表 + 取/放 `sched_lock`** | **【代码级】**（w13 已排除本例的值路径，见其 §2.2） |
| **P3** | **MS-12** | **`sleep_tasks_wake` 跨保护区读 task 字段（无 `task_lock`）** | `per_processor_scheduler.cpp:186-194`（持 `sched_lock` 读 `candidate_task->min_wakeup_stamp`）；写侧 `kthread_interfaces.cpp:368-377`（持 `task_lock` 写 stamp） | 同核 A 回收睡眠者 vs 该 task 的 stamp 由别处写 | 重叠即 8B 数据竞态 | 低（x86 对齐 8B 天然原子）；**状态机噪声** | **【代码级】** |
| **P4** | **MS-13** | **idle 被 `resched/yield` 无条件 `set_ready()`（running→ready），但按 idle 特例跳过入队** | `kthread_interfaces.cpp:291`、`:228`；`per_processor_scheduler.cpp:294-298`、`:231-235`（仅对非 idle 入队） | 中断/让权落在 idle 上 | — | 状态机噪声（idle 总是 fallback，不会丢；但 `running→ready` 仍打开 MS-7 的语义漏洞） | **【代码级】** |
| **P4** | **MS-14** | **`resched` 从设备向量 `[[noreturn]]` 走掉：ISR 的"正常返回/嵌套收敛"路径整体让位给调度器** | `x86_vecs_deliver_mgr.cpp:584`（`resched` 后不返回）；`Sysdef_exception_entries.asm:203-233`（`vec_demux_common` 的 post-call `iretq` 永不执行）；trampoline 的 `vec` 槽在恢复时被 `idt_style_load` 的 `add rsp,8` 跳过 | 任意设备中断 | — | **收敛性结构风险**：中断帧语义（IF/EOI/嵌套计数）完全依赖"调度器把帧原样存/还"；帧一旦被同栈覆盖（MS-1/MS-2）即静默 iretq 到野帧 | **【字节级+代码级】** w22 |

**排序说明**：MS-1/MS-2 是**唯一能同时解释三枚样本**的机制族（把"值是 `call` 返回址""双核同时""异常帧被同栈 `sched()` 覆盖"三点一次说完），且代码窗口逐行可见 ⇒ P0。MS-3/MS-4/MS-5 是**同一缺陷类**在不同入口的化身，危险度同级、只是样本尚未逐一命中。MS-6 是"`RIP=0` 上一跳"的最强代码级候选，故 P2 并列。

---

## 3. 逐路径最小交错时序（伪代码）

记 `A`=原核（受害），`B`=异核（施害/唤醒者），`T`=同一条 task，`R_T`=`T` 的栈区，`A.rsp` 随 A 执行**递减**。

### MS-1（P0，w13 的机理）— `block_if_equal` 发布后未切走 + 异核唤醒 + 跨核偷取

```
[A] T 在 A 上运行，rsp ≈ R_T(浅)
[A] T: int 226 (kthread_block_queue_if_equal)          ; IF=0
[A] kthread_common_save(T)   -> T.priv_ctx = frame     ; 恢复点 = int 之后(浅)
[A] T.set_blocked()          (running→blocked)         ; :410
[A] T.on_blockers_queue_bit=true; min_wakeup=now+5s    ; :413-414
[A] 释放 T.task_lock                                    ;  <── 窗口起点前
[A] waite_queue->push_tail(T)  (qlock 内)               ; :417 ★T 已"发布"，任何核可唤醒
[A] 释放 qlock                                          ; :422
[A] next_task_with_routine()->sleep_tasks_wake->sched() ; :424, A.rsp 降到 R_T 以下
     │
     │   ← 窗口 = [★ :417] .. [A 的 sched:293 atomic_load()]
     ▼
[B] NVMe CQ 中断落 B -> cq_interrupt_handler
[B]   pop_all(&clamp) 拿到 T                            ; T 已离队
[B]   bq_flush_pending: T.set_ready() (blocked→ready)   ; :208  ★未查"T 是否正被谁跑"
[B]   insert_ready_task(T) 到 get_other_scheduler(T.belonged=A) ; :216-218 ★进 A 的 ready 队尾
[B]   interrupt_handle 返回 1 → resched(frame)          ; :114-122 / :584
[B]   sched(): B.ready 空 → 偷 A 的 ready 队 → pop T     ; :246-259
[B]   set_running(T) (ready→running)                    ; :274 成功(此刻 T.state=ready)
[B]   T.atomic_load() → iretq 到 T.priv_ctx(浅 R_T)      ; :293
[B]   T 在 B 上继续跑 cmd_submit_and_process → ktime→HPET
      B.rsp 从浅处下降 ────────────────► 越过 A.rsp
结果：A、B 同压 R_T。B 的 HPET 内层 call 返回址 0x2C700
      落进 A 的 sleep_tasks_wake 帧槽 [rbp-0x268] → 野 this → #PF@只读.text（w13）
```

**为什么不会先 panic**：A 的 `sched()` 选中**别的** task（或 idle）——它并不会 `set_running(T)`；B 才是唯一把 T 置 running 的人。故 `:274` 的失败断言不触发，**静默同栈**。

### MS-2（P0，w22 的机理）— 设备中断内 `resched` 整段调度 + 自身入 ready + 跨核偷取

```
[A] T 在 A 上运行
[A] 设备 CQ 中断落 A（T 正阻塞等它? 或 T 被抢占）-> idt_vec_demux_entry 默认分支
[A]   local_tok.func() = cq_interrupt_handler (已 write_eoi)
[A]   res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE → resched(raw_frame)   ; :584 [[noreturn]]
[A] resched: kthread_common_save(T)  -> T.priv_ctx = frame        ; :289
[A]          T.set_ready()  (running→ready)                        ; :291
[A]          insert_ready_task(T) 到 A 自己的 ready 队列(队尾)      ; :297
[A]          next_task_with_routine → sched()
[A]   sched() 先看 A 自己的 ready：若队头是别的 task（T 在队尾），取别人
[B]  另一核 B 的 sched 偷 A 的 ready 队尾 → pop T                  ; :246-259
[B]  set_running(T) → atomic_load() → 从 T 的中断帧恢复在 B 上跑
[A]  A 的 sched 仍持/抢锁、跑 set_clock_by_offset 等（A.rsp 在 R_T 内）
结果：同栈两核；且 A 同栈后续的 sched() 帧书写覆盖了 A 之前那枚异常帧
      （w22：#PF@RIP=0 帧的 5 个 iret 源槽被 sched()→set_clock_by_offset 帧逐字节覆盖）
```

> 补充：若 A 的 `sched()` 恰好也选中 T（T 在队头），则 `set_running` 在第二次失败 → panic（样本未现）⇒ **实际形态必是"T 被留给 B、A 取别人"**，与 MS-1 同。

### MS-3（P1）— `exit` 发布 zombie 后栈被释放

```
[A] T 运行; T: allkthread_true_enter → 函数返回 → kthread_exit → int 226
[A] kthread_exit_cppenter: kthread_common_save(T); T.set_zombie()  ; :307-308
[A] next_task_with_routine()   ← A 仍要在这片栈上压 sleep/sched 帧   ; :311
     │
[B] reaper: zombie_observe(T) → ZOMBIE_DEAD → release_kthread(T)
[B]          __wrapped_pgs_vfree(T.priv_stack_base, pages)          ; :509
[B]          （页回 FPA；可能立刻被另一 kthread 的 stack_alloc 拿走）
结果：A 在**已释放/已再分配**的页上继续跑 → 栈 UAF / 两 task 共用物理栈页
```

### MS-4（P1）— `kthread_self_blocked` 纯握手缺失

```
[A] T: set_blocked()  (running→blocked)  ; :320 （无入队）
[A] next_task_with_routine()             ; :323
     │  ← 窗口
[B] wakeup_thread(T.tid)：取得 T.task_lock → state==blocked ✓
     on_blockers_queue_bit==false ⇒ 不被挡 ; :346-350
[B] T.set_ready(); insert_ready_task(T 到 T.belonged=A 队列)  ; :351-355
[B] B 的 sched 偷取 T → 在 B 上从 T 的恢复点起跑
结果：同栈并发
```

### MS-5（P1）— 睡眠者回收窗口

```
[A] T: kthread_sleep: set_blocked; min_wakeup=now+off; sleep_queue.insert(T)  ; :371-377
[A] next_task_with_routine()  ; :381
     │  ← 窗口：T 已发布(sleep_queue)，A 未切走
[A] 若本轮/下次 A 的 sleep_tasks_wake 见 min_wakeup<=now → set_ready@201 + insert@218
[B] B 的 sched 偷 A 的 ready → pop T → 在 B 上跑
结果：同栈并发（offset 小/负载抖动使 A 高频回收时窗口更常开）
```

### MS-6（P2）— `atomic_load` 与 `kthread_common_save` 撕裂

```
[A] sched(): 选 to_run=T'；持 T'.task_lock: set_running ✓; 释放锁(:291)
[B] 另一路径对同一 T' 调 kthread_common_save → T'.priv_ctx = *frame（~400B 逐字写）
[A] sched(): to_run->atomic_load() 读 T'.priv_ctx（无锁） ← 与 B 的赋值交错
结果：读到半新半旧的 iret 帧 → iretq 到野 rip/cs/rsp（RIP=0 型跳飞候选）
```

### MS-10（P3）— 重复 flush / 越界

```
cq_interrupt_handler(>64 waiters):
  iter1: pop_all → arr[0..63], batch_count=64, is_queue_empty=false
         bq_flush_pending(clamp)  → set_ready×64 + insert×64   ; 未复位 batch_count
  iter2: pop_all → arr[64]=…（越界 8B！）batch_count=65 → break; is_queue_empty=true
         bq_flush_pending(clamp)  → 又把 arr[0..64] 全部 set_ready+insert ⇒ 前 64 个重复入 ready
  退出内层 → 外层 if(batch_count>0) bq_flush_pending(&clamp)   ; :185-187 再重复一遍
结果：ready_queue 里同一 task 多个节点 → 两核各 pop 一个 → 第二个 set_running 失败 panic / 或"running+ready"错象
```

---

## 4. 锁 / 中断 / 状态机根缺陷 + 锁序

### 4.1 状态转换缺互斥（核心）

| 转换 | 谁做 | 缺什么 |
|---|---|---|
| `running→blocked` + 入队（block/sleep） | 任务**自己**（`kthread_sleep/block`） | 缺 **"我已离开这片栈"** 的完成信号；唤醒者不知道"发布者是否还在栈上" |
| `running→ready`（resched/yield/`bq_flush_pending`） | 中断/唤醒者 | `set_ready` **允许 running→ready**，且**无 owner_cpu 校验**（`task.cpp:27-35`） |
| `blocked→ready` + 入队（wake） | 唤醒者 | 缺 **"该 task 是否已被别核认领/正在跑"** 的检查（`bq_flush_pending` 尤其，`bq_system.cpp:203-211` 连返回值都丢） |
| `running→zombie` + 释放（exit） | 退出者 + reaper | 缺 **"已切走"** 才允许释放栈（`kthread_interfaces.cpp:308` vs `:509`） |
| `ready→running`（占位） | `sched()` | 只在 `task_lock` 下原子，但**只挡"两个 ready 同时跑"**，挡不住"一个 running 被翻 ready 后再跑" |

**根**：`task_state` 一个字节被当"所有权"，但**语义是单核假设**。多核下"ready/running"是**每核可并发的概念**，必须拆成 `state`（生命周期）+ `owner_cpu`（执行权，原子）+ `queued`（是否在队列）。

### 4.2 队列可被两核同时取？

| 队列 | 保护 | 两核同取的可能 |
|---|---|---|
| `per_processor_scheduler::ready_queue` | `sched_lock`（`sched` 本地 pop @238-242、跨核偷 @250-256、`insert_ready_task` @295） | **不可**同时取同一节点；**但**若同一 task 被 `insert_ready_task` **重复插入**（MS-8/MS-10），两个节点可被两核各 pop → 同 task 同跑 |
| `sleep_queue`（per-scheduler） | `sched_lock`（insert `kthread_interfaces.cpp:376-378`；回收 `per_processor_scheduler.cpp:186-194`） | 只有**属主核**碰它 ⇒ 不跨核；但"属主核回收"与"该 task 在别核被偷跑"是 MS-5 |
| `block_queue::inner_queue` | `qlock`（`push_tail` @417 在 `block_if_equal` 的 `g(waite_queue->qlock)` 内；`pop_all/pop_timeouts` 在 `cq_interrupt_handler`/sweeper 的 `g(qlock)` 内） | 节点本身独占；但 `clamp` 数组（栈上）可**携带同一 task 多次** ⇒ MS-10 |
| `container`（BQ 池）+ `container_lock` | `spinrwlock` | 无交叉 esp. |

### 4.3 哪个唤醒路径不查"运行核"？

- **`bq_flush_pending`（`bq_system.cpp:203-211`）**：`t->set_ready();` 不看状态、不检 `running`、不检 `on_blockers_queue_bit`。**唯一**如此"裸唤醒"的路径。（`wakeup_thread` 反而会查 `ready||running` 并拒（`kthread_interfaces.cpp:342-345`）——所以 B 类假设里"`wakeup_thread` 只 `interrupt_guard`"其实**有**状态检查；**真正无检查的是 `bq_flush_pending`**。）
- `sleep_tasks_wake` 对 sleepers：靠"若被随机唤醒则从 `sleep_queue` 摘除"隐式保证，但对"跨核偷跑"无感。

### 4.4 锁序是否被破坏？

逐调用点核对（`spinlock_interrupt_about_guard` = 关中断+取锁，析构恢复 IF）：

- `block_if_equal`：`container_lock`(read) → `waite_queue->qlock` → `blocked_task->task_lock`（`kthread_interfaces.cpp:391/402/408`）✓
- `bq_flush_pending`：`t->task_lock`（loop1）→ `target->sched_lock`（loop2），两 loop 分开（`bq_system.cpp:206/217`）✓
- `cq_interrupt_handler`：持 `cq.wait_queue.qlock` 内调 `bq_flush_pending`（⇒ `qlock > task_lock > sched_lock`，`:136/206/217`）；扫描 CQE 时 `qlock > sq_lock`（`:163`）✓ 与头部注释一致
- `sweeper`：`container_lock`(read) → `qlock` → (`task_lock>sched_lock`) ✓
- `wakeup_thread`：`interrupt_guard` → `task_ptr->task_lock` → `target_scheduler->sched_lock`（`:326/340/354`）✓
- `task_launch`：`t->task_lock` → `target->sched_lock`（`:203/214`）✓
- `kthread_sleep`：`task_lock` → `sched_lock`（`:368/376`）✓
- `sched`：`to_run->task_lock`（`:273`）**与任何 sched_lock 不重叠**（本地/偷取段在 lambda 内已释放）✓

**未发现顺序反转**。⇒ 问题不是死锁序，而是**"原子粒度不够"**：状态改变、入队、栈所有权移交被拆成多个互不重叠的临界区，中间态对外可见且可被另一核利用。

**另有一处"锁外读共享可变结构"**（不是顺序问题）：`atomic_load()` 读 `priv_ctx`（`per_processor_scheduler.cpp:293`）与 `task::task_event_shift` 等均**在 `task_lock` 之外**；`priv_ctx` 又是别处（`kthread_common_save`）整结构体写的目标 ⇒ MS-6。

### 4.5 `resched` 重入判据

- **同核**：`int/IRQ` 门 `IF=0` + guard 保存/恢复 IF ⇒ 同核不会在 `resched` 中途再进 `resched`。**判据 = 中断门类型 0xE**（`x86_vecs_deliver_mgr.cpp:113-116`）。
- **跨核**：**无任何判据**。`resched` 被设计为"每核串行、以 `atomic_load` 收尾"，但 ready 队列是**全局可偷**的 ⇒ "我 `resched` 到 T" 与 "B `resched` 到 T" 之间没有任何互斥。
- **IRQ 中 `resched` 的收敛性**：`resched` 是 `[[noreturn]]`，故 `vec_demux_common` 的 post-call `iretq` **永不执行**（`Sysdef_exception_entries.asm:203-233`）；中断帧的保存/恢复完全由 `priv_ctx` 承担（`vec` 槽由 `idt_style_load` 的 `add rsp,8` 跳过）。**任何对 `priv_ctx` 的并发写（MS-6）或对同栈的并发写（MS-1/2）都会让这条路径静默跳到野帧**——w22 正是如此。

---

## 5. 通电后验证清单（探针 / 断言，逐条对应路径）

> 原则：复用现有零 UART 的 `interrupt_log_ring`（`debug_tmp_ring_buff.h`）+ `wraith_probe.h` 原语；**命中先 `WRAITH_LOG` 留证、不 panic**（把竞态留在现场）；`wraith::probe_verbose` 当变量做消融对照。每次 `print` 在栈上吃 ~1KB（`vprintkv2.h`），**会加深栈占用、可能移动竞态**（Heisenbug），必须对照。

### 5.1 直证 MS-1/MS-4/MS-5「同任务跨核同栈」——最高优先

| 探针 | 位置 | 断言/观测 | 最小可证观测量 |
|---|---|---|---|
| **G1 落地核自证** | `sched()` 在 `to_run->task_lock`内、`atomic_load()` 前（`per_processor_scheduler.cpp:272-291`） | 给 task 加 `volatile uint32_t exec_cpu`；此处写"本核"；若上一次写入者 ≠ 本核且未观察到"离栈"→ `WRAITH_LOG("SWT-OCCUPY tid=%llx prev=%u now=%u …")` | 同一 `tid` 在**无中间切走痕迹**下被两核写 `exec_cpu` |
| **G2 阻塞侧** | `block_if_equal`/`kthread_sleep`/`kthread_self_blocked`：`set_blocked()` 后、`push_tail`/`insert` 前、`next_task_with_routine()` 前各一条 | `WRAITH_LOG("BLK tid=%llx cpu=%u sbase=%llx rsp=%llx off=0\n")`；离栈前再打 `off=1` | 同一 `tid` 出现 `BLK…off=0`（核 A）→ 随后 `SWT tid=该 tid`（**核 B**）且**看不到 A 的 off=1** |
| **G3 唤醒侧** | `bq_flush_pending:203-211` / `wakeup_thread:351` 前 | `WRAITH_LOG("WAK tid=%llx from=%u t=%llx")`，并记录 `exec_cpu`（若 G1 已加） | `WAK from=B` 时 `exec_cpu` 显示该 task 仍= A 且 `off=0` |
| **G4 栈域自证** | `sleep_tasks_wake` W0/W3 已有（`per_processor_scheduler.cpp:150/208`）**补** `priv_stack_base`+`wraith::in_range(rsp_now(),sbase,pages)` | 若"本核 rsp 落在别的 task 栈内"或"本帧槽被非本帧写"→ log | w13 的 `0x2C700` 类槽复现 |

**判定串**：`BLK(tid,T,cpu=A,off=0)` → `WAK(tid,T,from=B)` → `SWT(tid,T,now=B)`，且 A 侧无 `off=1` ⇒ **坐实 MS-1**。

### 5.2 直证 MS-3「栈 UAF」

| 探针 | 位置 | 断言 |
|---|---|---|
| **G5** | `kthread_exit_cppenter:307-308` 后：`WRAITH_LOG("EXIT tid=%llx sbase=%llx rsp=%llx")`；`release_kthread:509` 前：`WRAITH_LOG("FREE tid=%llx sbase=%llx state=%u")` | 离线对齐：若 `FREE` 的 `sbase` 与某 `EXIT` 相同且 `FREE` 早于该 task 的下一次 `SWT/atomic_load` ⇒ **栈被提前释放**；再加 `__wrapped_pgs_vfree` 后跟踪该 phys 段被 `stack_alloc` 复用的 `(phys,tid)` |

### 5.3 直证 MS-6「撕裂 iret 帧」

- **G6**：在 `atomic_load()` 入口（`per_processor_scheduler.cpp:293`）前断言：`to_run->priv_ctx.core_ctx.idtctx.iret.cs` ∈ {8}，`rip` 落内核 text，`rsp` ∈ `[priv_stack_base, base+pages*4K)`；不满足 → `WRAITH_LOG("IRET-BAD …")`（**先留证**）。
- 可选：临时给 `priv_ctx` 加"写者 cpu + 序号"，`kthread_common_save` 写、`atomic_load` 读校验序号一致。

### 5.4 证/伪 MS-10（重复 flush / 越界）

- **G7**：`bq_flush_pending` 入口 `WRAITH_LOG("CLAMP b=%u first=%llx last=%llx", clamp->batch_count, arr[0], arr[batch_count-1])`；`cq_interrupt_handler` 内层/外层调用点各加一个 tag，离线看**同一 clamp 是否被 flush 两次**、`batch_count` 是否 `>64`。
- 立即堵（不改默认行为）：`pop_all/pop_timeouts` 写 `arr[batch_count]` 前 `if(batch->batch_count>=64){ WRAITH_LOG("CLAMP-FULL"); return; }`；`cq_interrupt_handler` 外层 flush 前 `clamp.batch_count=0`（或删外层）。

### 5.5 伪阴/伪阳检查（MS-11）

- **G8**：`get_other_scheduler` 加 `if(pid>=logical_processor_count) WRAITH_LOG("GOS-OOB pid=%u",pid)`；调用方先打 `t->belonged_processor_id`。只记录不 panic。

### 5.6 差分复现（不改根因，直接检验 A）

1. 把 `NVMe_Controller::interrupt_handle` 的返回从**恒 1** 改成"仅确需唤醒时置 `TOKEN_FLAG_MASK_TOKEN_SCHEDULE`"（`NVMe_interrupts.cpp:121`）→ 观察 WRAITH 率（直接检验"IRQ 内整段 resched"是否为必要触发面）。
2. `probe_verbose=1/0/无探针` 三档对照异常率 + 宿主负载（2~4×`yes`）。
3. 命令沿用 `WRAITH.md §5/§8`：
   ```
   cd /home/PS/PS_git/OS_pj_uefi/kernel
   Tools/tcg-trace/tcg-trace.sh --tag sv --repeat 40 --timeout 40 --cap-gb 1 --dump-vmcore
   ```

---

## 6. 修法方向（只给方向与取舍，不写代码）

1. **显式"执行权 + 栈所有权"**：给 `task` 加 `owner_cpu`（原子）与 `on_cpu`/`off_cpu` 完成信号，取代"用 `task_state` 当所有权"。`set_ready` 对 `running` **要么拒绝，要么只在 owner 自证时允许**（拆分 `preempt_self()`）。
   - 取舍：改动面大，但这是**唯一能根治** MS-1/2/4/5/7 的方向；建议先用编译开关 + 断言落地（不改默认行为）。
2. **发布→切走的握手（handoff）**：block/sleep/exit 的"改状态+入队"与"真正离开栈"必须可被唤醒者观测。两种可行形态：
   - (a) 入队时带 `leaving` 标记，唤醒者只把 task 放进 ready 队列，但**直到 switcher 在 `atomic_load` 前显式 `mark_off_cpu()` 后**才允许被 `sched` 认领；
   - (b) 由 switcher 自己在"确定不再使用本栈"之后**统一**完成入队（即把 `next_task_with_routine` 内的所有 schedule 状态变更收敛到切走前的**单一临界区**）。
   - 取舍：(a) 侵入小、可增量验证；(b) 最干净但要重排 `sleep_tasks_wake/sched` 结构。
3. **`bq_flush_pending` 补状态闸门**：唤醒前校验 `state==blocked && !正在别核 running`（用 owner_cpu），并**检查/处理 `set_ready` 返回值**；`on_blockers_queue_bit` 语义统一。
4. **`insert_ready_task` 去重 + 拒 running**：以 per-task `queued` 位防重复节点；`state!=ready` 或 `owner_cpu` 占用时拒绝。列表节点分配移出调度临界区（避免 `new node()` 在 `IF=0`+持锁下做堆分配）。
5. **收敛中断内 `resched`**：设备 ISR **只置"需要调度"**（每核 `need_resched` 或发 `IPI_RESCHED`），真正的 `resched` 在**统一退中断点**做一次；或至少让 `resched` **不跨核偷取**（禁用 `sched()` 的跨核 steal，或在 steal 时校验 owner）。取舍：`IPI_RESCHED` 目前**无人发送**（grep 证实，仅接收侧 `/x86_vecs_deliver_mgr.cpp:562/637`），可顺势作为统一入口。
6. **`sched` 的"选到即占"原子化**：把"从某核 ready 队列 pop 出某 task"与"把该 task 置 running + 写 owner_cpu"做成**同一不可分割步骤**（例如 pop 时在节点上打 owner 戳），并让 `atomic_load` 读 `priv_ctx` 处于同一互斥（或校验序号）⇒ 关闭 MS-6。
7. **exit 的"离栈后才可回收"**：`release_kthread` 必须等到退出者 `off_cpu`（或 `state==dead` 仅由退出核在自己切走后设置）。
8. **边界与硬化**：`get_other_scheduler` 加界；`clamp` `batch_count` 复位/上限；`cq_interrupt_handler` 去掉重复外层 flush。
9. **保持无关加固**：`#PF/#GP` 的 IST（WRAITH §7.1）继续保留（改失败模式为可读），但**记录**在根因修好后回落 `ist_index=0`；`page_fault_handler` 应把"CS 既非 0 也非 3"当**内核故障**（当前静默放行是 w22 首爆被吞的放大器，勿只靠 IST）。

---

## 7. 一页速览 + 诚实标注

### 7.1 速度卡

- **要信的一件事**：样本里的"坏值"是 **`call` 压的返回址**（`0x2C700`=HPET 内层返回址）；**写不遵循受害核的 rsp** ⇒ 写者正在（某映射的）受害者栈上执行取时/调度代码。⇒ 只有"**同一片 task 栈被两核同时活着**"能一次解释三样本。
- **P0 两条**：MS-1（block↔wake 握手缺失 + `bq_flush_pending` 裸唤醒 + `sched` 跨核偷取）、MS-2（设备中断里整段 `resched` + 自身入 ready + 偷取）。二者共用同一"发布→切走"窗口。
- **同一缺陷类的其他化身**：MS-3（exit 栈 UAF）、MS-4（self_blocked 纯握手）、MS-5（sleep 回收窗口）。
- **能立刻静态断定**：`set_ready` 允许 `running→ready`（`task.cpp:27-35`）；`insert_ready_task` 只查 `state==ready` 且不去重（`:295-325`）；`get_other_scheduler` 无界（`:369-372`）；`bq_flush_pending` 丢 `set_ready` 返回值（`bq_system.cpp:208`）；`cq_interrupt_handler` 外层重复 flush（`NVMe_interrupts.cpp:185-187`）；`atomic_load` 在 `task_lock` 外读 `priv_ctx`（`per_processor_scheduler.cpp:293`）。
- **锁序**：**无反转**；问题是**原子粒度**。

### 7.2 诚实标注（静态推不出、需运行时确认）

1. **"某次崩溃确切是哪两核写谁"未定**：静态只能证明窗口存在 + 写不遵循本核 rsp；需 §5.1 G1-G4 的运行时串证。
2. **MS-6（撕裂 iret 帧）是否为 `RIP=0` 的上一跳**未定（w22 §6-1 的三候选之一）；需 G6 序号校验。
3. **MS-10 可达性**：需要**单个 block_queue >64 个并发 waiter**；NVMe 单 CQ 当前仅 2 worker ⇒ **当前大概率不显形**；i8042/text 订阅队列的并发上限未逐一测。列为**潜伏**。
4. **MS-3 是否真被 `nvme_parallel_init_all` 触发**：需 G5 的 `(tid,sbase,时间序)` 对齐 + FPA 复用跟踪。
5. **FRED 路径**：`fred_vec_demux_hw_dispatch`（`x86_vecs_deliver_mgr.cpp:644-650`）与 IDT 默认分支同型（也 `resched`），但 TCG 下 FRED 未启用 ⇒ 静态度量一致、运行时未验；`IA32_FRED_STKLVLS` 与 IDT IST 槽位不一致（WRAITH §7.1 已记）同样是"既存、未验"。
6. **探针观测效应**：`WRAITH_LOG/WRAITH_TRACE` 每次 ~1KB 栈占用，挂在深栈热路径上；**会移动竞态**（Heisenbug）——通电时必须做 `probe_verbose` 消融对照。

> 全程只读；未运行任何被测内核动作；除本报告外未改动仓库任何文件。
