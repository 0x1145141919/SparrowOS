# WRAITH 静态审查 — 「取时路径指针栈槽被踩」的代码级候选机制

- **日期**：2026-09-17
- **性质**：**纯静态**（只读源码/文档 + grep/nm/objdump 式阅读）。**未运行任何东西**：没插电、没跑 QEMU/tcg-trace/selfcheck，没 make，没改内核源码。
- **输入**：样本报告 `analysis/w22_01_report.md` · `analysis/w13/w13_report.md`；战报 `Docs/Debug/WRAITH.md` · 武器库 `TCG_TRACE_ARSENAL.md` · 纪律 `KERNEL_DISCIPLINE.md`；源码树 `/home/PS/PS_git/OS_pj_uefi/kernel`。
- **目标**：把「同一次函数调用途中，指针型栈槽被改野」当**结果**，穷举「**谁能写到别的执行上下文的栈槽**」的**代码级**路径并按证据排序。本轮**没有新 trace/vmcore**，全部推理基于已钉死的样本事实 + 现行源码。

---

## 0. 结论（一段）

四枚样本的共性（**同一次调用内**、**帧内局部/入参槽**被换成「取时/调度路径的天然栈值」，且落点常在**帧底保存-`this`槽**或 `get_time_stamp_in_us` 的 `regs`/`this`）**无法用「单核重入」几何解释**（重入帧只会更低、不会写到当前 `rsp` 之上），必须是**另一个执行上下文写进了受害者的栈**。把这些栈值（`0x2c700` = `HPET::get_time_stamp_in_us` 内层 `call` 的返回址；w22 里成串的 `sched()/set_clock_by_offset` 帧）当成「写者的指纹」倒推，最自洽的一类机制是：**一个任务栈（物理页）在被某个核当作「已阻塞/可唤醒」发布之后、但它上面的核还没真正切走之前，被另一个核重新调度/继续执行** —— 于是**同一片栈被两核同时压栈**，较浅那一路的 `call` 返回址正好落进较深那一路的帧内槽位。现行调度/唤醒代码里有**三处结构性缺陷**共同打开这个窗口：(1) `block_if_equal`/`kthread_sleep` 在**自己的栈上**就把任务置 `blocked` 并挂进队列，**先发布后去调度**；(2) `task::set_ready()` 允许 `running→ready`，且唤醒路径（`bq_flush_pending`）**不检查**任务当前是否正跑在别的核上；(3) `sched()` 会**跨核偷取**别的调度器 ready 队列。样本的「NVMe 并行初始化 × 取时热点 × 宿主负载敏感 × KVM 几乎不复现」正是这类**跨核时序竞态**的画像。次要嫌疑是**同一批物理页被两次当栈分配**（FPA/恒等别名）与 **`get_other_scheduler(pid)` 无边界**（后者只能解释「指针被写成指针」，**解释不了** `0x2c700` 这种代码地址）。**结论级**：证据链最强的是「**跨核同栈并发写**」这一大类；**唯一未被正面证伪**的代码级入口是「阻塞/唤醒无 handoff 同步」这条链（下 §2-A/§2-B）。**诚实地讲**：静态无法 100% 钉死「谁写了那 8 字节」，下文给出候选表 + 不能排除项 + 通电验证清单。

---

## 1. 候选机制表（按证据强度排序）

| # | 机制 | 涉及文件:行 | 为什么能解释「同一次调用内栈槽被改」 | 与哪几枚样本吻合 | 反证 / 存疑 |
|---|---|---|---|---|---|
| **A** | **阻塞未完成即被异核唤醒/顶上 → 同一任务栈被两核同时压栈**（"提前唤醒 / 无 handoff 同步"） | `kthread_interfaces.cpp:383-425`（`block_if_equal_cppenter`：`set_blocked()`@410 → `push_tail`@417 → `next_task_with_routine()`@424）；`:363-381`（`kthread_sleep_cppenter`）；`:101-150`（`kthread_common_save`）；`:238-300`（`resched`）；`bq_system.cpp:197-225`（`bq_flush_pending`：`set_ready()`@208 → `get_other_scheduler`@216 → `insert_ready_task`@218）；`per_processor_scheduler.cpp:226-293`（`sched`）与 `:142-224`（`sleep_tasks_wake`）；`task.cpp:27-35`（`set_ready` 允许 running→ready） | 写者与受害者**在同一片栈的不同深度**：写者（异核恢复该任务后继续跑 NVMe/取时路径）执行 `call` 的返回址，恰好压进受害者（原核此刻仍在 `sleep_tasks_wake` 帧里）的帧内槽 —— 完美解释「**值是 HPET 内层 `call` 返回址**」+「**写不遵循当前 `rsp`**」（写者的 `rsp` 属于同栈另一路调用）。 | **w13**（受害者=`sleep_tasks_wake` 帧底 saved-`this` 槽，值=`0x2c700`=HPET 内层返回址）；**wh02**（CPU0/CPU1 双核同时中招 —— 只有"同栈两核"才自然产生"双核同时"）；**wl02**（`:137` `regs` 尚好、`:138` `this` 已 0 —— 单字/单槽中途被改写）；触发面=NVMe 并行 init、对宿主负载敏感、KVM 压没 —— 全是跨核时序竞态特征。 | ⚠️ w13 dump 里 tid6 仍是 `blocked`、不在 ready 队列（Dump 是快照，看不到瞬态）；若两核**都** `sched()` 到同一任务，`set_running()` 在 `to_run->task_lock` 下第二次会失败 → 会 panic（本样本没崩在这里）⇒ 重叠必须是**「阻塞者尚未切走」的瞬态窗口**，而非一次完整双重调度。需运行时探针确认（§4）。 |
| **B** | **同一批物理栈页被两次当成"任务栈"分配**（FPA 双重分配 / `alloc_available_space` VA 重叠 / 恒等窗口别名） | `out_surfaces.cpp:534-660`（`stack_alloc`：FPA alloc → vm_table alloc → enable_VMentry）；`kthread_interfaces.cpp:473-479/505-517`（`kthread_init` 分配栈、`release_kthread` 还栈）；`memory/FreePagesAllocator.cpp`、`all_pages_arr.h`、`page_frame_state_mgr.h`；别名基址 `main_phyaddr_access_window.h:24`（`PHYACC_VA`） | 若两枚任务栈拿到**同一物理页**（或同一 VA 段），则任务 X 在取时路径上的 `call` 直接把返回址压进任务 Y 的栈槽；「写不遵循本核 `rsp`」即"本核栈物理页其实属于别人"。 | w13（每任务栈各有一处 `0x2c700`——但本次受害槽是"另一路写进来的"）；wh02 双核；触发面 NVMe（worker 频繁建队列/释放就是 `stack_alloc/release` 高频期）。 | ⚠️ w13 §4 已查「同物理页未被别的 task 栈复用」；纯双重分配通常会**大面积**损坏（整页清零/多槽乱），难只坏一个 qword。仅作并列候选。 |
| **C** | **`get_other_scheduler(pid)` 无边界校验 + 跨核写 ready 队列** | `per_processor_scheduler.cpp:369-372`；`bq_system.cpp:216-221`；`kthread_interfaces.cpp:341`（`wakeup_thread`，用 `task_ptr->belonged_processor_id`）；`:451-456`（`task_launch`）；字段 `task.h:38`（`belonged_processor_id` 无固定 owner 校验，只在 `sched():276` 被赋值） | `pid` 若为野值 → `&global_schedulers[pid]` = `base + pid*0x200`；再 `insert_ready_task` 会**往该地址写链表指针 + 拿/放 `sched_lock`** ⇒ 一次「按（伪）索引算出地址」的**跨核/越界写**，可落在别核栈附近。 | 最能解释 **bt04**（锁指针高位被踩 `0xFFFF…→0x02FF…`）与 **hp04**（`#PF@unlock ← sleep_tasks_wake`）一类"**指针被换成指针**"的样本。 | ❌ 与 w13 的**值**不符：它写的是 **task/节点指针**，**得不到** `0x2c700`（这是 `.text` 地址、是 `call` 压的返回址）。w13 §2.2 也据此把假设 C 判为"不被支持"。⇒ 只解释"指针型"腐败，不解释"取时值/返回址"腐败。 |
| **D** | **NVMe DMA / 命令缓冲 / 设备 CQE 驱动的越界写** | `io_queue_cmd.cpp:56-101`（`create_io_cq`：FPA 分配 CQ ring → `PHYACC_VA` 清零@62-63 → 记录 `cq_ring`@98-101）、`:120-158`（`create_io_sq`，同型）；`NVMe_init_and_shutdown.cpp:459-472`（`sqs` 主窗口分配）、`:577-583`（`cqs`）、`:137-150`（HMB）、`:608-618`（admin_buffer）；`PRPs.cpp`（PRP List 经 `PHYACC_VA` 写）；`NVMe_interrupts.cpp:115-122`（`interrupt_handle` 恒返回 1）、`:130`（`cqs[qid]` 无界）、`:146/164`（`cq_ring[cursor]` 读 + `sqs[sq_id].complete_commands_bank[cmd_id] = entry`，`sq_id/cmd_id` 由**设备 CQE 提供、无界**） | 设备把数据 DMA 到「算了/录错」的物理地址；或设备返回的 `sq_id/cmd_id` 越界 → **在最坏情况下正好写进某栈页**。 | 触发面就是 NVMe init；w22 假设 E 未排除。 | ⚠️ 值不对：DMA/CQE 写进去的是**设备数据或 0**，不是"HPET 内层 `call` 返回址"。w22 §4-E 无 DMA 指纹。⇒ 只解释"整字/多字乱"，不解释"恰好是取时返回址"。 |
| **E** | **中断/异常帧嵌套 + IST 共享（#GP 与 #NMI 共用 IST3；#PF 与 #MC 共用 IST2）** | `x86_vecs_deliver_mgr.cpp:103-165`（`early_init`：NMI ist=3@118，DF ist=1@130，**PF ist=2@146，GP ist=3@147**，MC ist=2@150）、`:85-95`（`fred_init_stklvls`：PF=MC 级、GP=NMI 级）、`:575-584`（设备向量默认分支**中断里直接 `resched`**）；`Sysdef_exception_entries.asm`（入口用当前栈 + `FAULT_FREEZE` **当前是开的**）；`init_init.cpp:325-328`（每核 `tss.ist[1..3]`） | 若同一 CPU 在"刚用 ISTn 的路径上"又触发同级的另一事件，或设备中断在 `resched` 中途再压一帧，则**帧互相覆盖**。 | 可解释"崩溃点常常在受害函数"与风暴（hp11/hp16）。 | ⚠️ 值不对（会是异常帧/向量，不是 `0x2c700`）；**同核重入被"中断门清 IF"挡住**（`early_init` 里 vec 32-255 `type=0xE` 中断门；handler 全程 IF=0）⇒ 单核嵌套基本不成立。QEMU 不产 #MC/#NMI。存疑保留（WRAITH §7.1 已记 FRED STKLVLS 与 IDT IST 不一致）。 |
| **F** | **具体函数里的栈缓冲越界（次生放大器）** | `bq_system.h:9` `arr[64]` + `bq_system.cpp:181-195`（`pop_all` 在校验前先 `arr[batch_count++]`，**累计不重置时能写到 `arr[64]`**）+ 调用点 `NVMe_interrupts.cpp:175-183`（`clamp` 跨轮不复位）；`memory_base.h:80-102` `entryies[5]`；`out_surfaces.cpp:322` `done_bitmap[512]`；`debug_tmp_ring_buff.cpp` `char buf[LOG_LINE_MAX]`（=1024） | 本函数栈上就有的**真实 OOB 写**，会踩坏相邻局部槽（含保存的 `this`/指针）。 | 与"栈槽被踩"的**现象**同型，但**值**通常是指针/计数。 | ⚠️ 需要 >64 个同队列等待者（NVMe 只 2 worker），或 >5 段；值不对。列为"放大器/次要"。 |

> **读表要点**：只有把"**写入值 = 取时/调度路径的 `call` 返回址**"当硬约束，才能筛掉 C/D/E/F，收敛到 **A/ B** 这一类——**"写者正在（某映射的）受害者栈上执行取时/调度代码"**。

---

## 2. 最可疑的 2~3 处（精确位置 + 引用片段）

### 2.A 【首选】阻塞/唤醒无 handoff 同步：`block_if_equal` 在**自己栈上**先发布 `blocked` 再挂队列，异核可立刻把它顶起来

`src/scheduler/kthread_interfaces.cpp:383-425`：

```cpp
void block_if_equal_cppenter(x64_standard_context_v2 *context)
{
    per_processor_scheduler*scheduler=get_self_scheduler();
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    ...
    {
        spinlock_interrupt_about_guard g(waite_queue->qlock);
        if(*check_address==block_token)
        {
            {
            context->rax|=1;
            ...
            spinlock_interrupt_about_guard h(blocked_task->task_lock);
            kthread_common_save(context,true,blocked_task);   // 存 ctx
            if (!blocked_task->set_blocked()) ...            // <<< 置 blocked（此刻我还在自己栈上！）
            blocked_task->task_event_shift(qevt);            // 读时间（ktime→HPET）
            blocked_task->on_blockers_queue_bit = true;
            blocked_task->min_wakeup_stamp = ktime::get_microsecond_stamp() + 5000000;
            should_block=true;
            }
            waite_queue->push_tail(blocked_task);            // <<< 挂进 wait_queue（可被异核 pop 唤醒）
        }else{ context->rax&=(~1); }
    }
    if(should_block){ scheduler->next_task_with_routine(); } // <<< 到这里才真正切走
}
```

**写者在哪**：设备 CQ 中断在**另一个核**上跑 `idt_vec_demux_entry` 默认分支 → `cq_interrupt_handler` → `pop_all` → `bq_flush_pending`：

`src/scheduler/bq_system.cpp:197-225`：

```cpp
void bq_flush_pending(blocked_tasks_clamps_t *clamp, bool is_timeout){
    ...
    for (uint32_t i = 0; i < clamp->batch_count; ++i) {
        task* t = clamp->arr[i];
        spinlock_interrupt_about_guard gt(t->task_lock);
        t->priv_ctx.rax = rax_enc;
        t->set_ready();                       // <<< 不查状态！set_ready() 允许 running→ready
        ...
    }
    for (uint32_t i = 0; i < clamp->batch_count; ++i) {
        per_processor_scheduler* target = get_other_scheduler(t->belonged_processor_id); // <<< 无边界
        spinlock_interrupt_about_guard gs(target->sched_lock);
        KURD_t kurd = target->insert_ready_task(t, false);   // <<< 进 ready 队列
    }
}
```

于是：**原核还在 `block_if_equal_cppenter → next_task_with_routine → sleep_tasks_wake/sched`（同一片栈）**，另一核已经把该任务 `set_ready`+入队，并在自己的 `sched()`（`per_processor_scheduler.cpp:226-293`，注意 `:274 set_running()`、`:293 atomic_load()`）里 `iretq` **恢复到同一 `priv_ctx.fred.rsp`** → 该任务在另一核继续跑，`cmd_submit_and_process`/`task_event_shift` 一读时间就 `call HPET::get_time_stamp_in_us` —— **两核从此共用一片栈**。`0x2c700` 就这么写进原核 `sleep_tasks_wake` 的帧内槽。

### 2.B 【受害点】`sleep_tasks_wake` 的帧内 `this` 槽 + `sched()` 跨核偷取（无 owner 校验）

`src/scheduler/per_processor_scheduler.cpp:142-224`（`sleep_tasks_wake`）：`this` 由序言存到 `[rbp-0x268]`，**每条 `g(this->sched_lock)`（`:186` 第一条 / `:216` 第三条）都重新读它**。两次 sched_lock 之间的开中断窗口里 `set_ready()`→`task_event_shift()`（读时间）——**正是 w13 line171 崩点**。

`src/scheduler/per_processor_scheduler.cpp:236-293`（`sched`）：

```cpp
task* to_run=[&]()->task*{
    { spinlock(this->sched_lock); if(this->ready_queue.size()){...pop...} }
    for(uint64_t i=0;i<logical_processor_count;i++){
        per_processor_scheduler*other=get_other_scheduler(i);
        if(other==this)continue;
        { spinlock(other->sched_lock);                    // <<< 跨核偷别的核 ready 队列
          if(other->ready_queue.size()){ ...pop; return popped; } }
    }
    return &this->idle;
}();
...
{ spinlock(to_run->task_lock); if(!to_run->set_running()) panic... }  // 只挡"重复调度"，不挡"重复在跑"
```

`sched()` **没有任何"这个任务是不是正在别的核上跑"的校验**，`insert_ready_task` 也只查 `state==ready`；而 `task::set_ready()`（`task.cpp:27-35`）**把 `running` 也算合法前态**，于是"发布"与"占用"之间没有互斥量。

### 2.C 【触发面】NVMe：每次 CQ 中断强制 `resched`，且设备索引无界

`src/arch/x86_64/core_hardwares/NVMe/NVMe_interrupts.cpp:115-122`：

```cpp
uint64_t NVMe_Controller::interrupt_handle(interrupt_token_t *token){
    __uint128_t token_private = token->token_private;
    NVMe_Controller* dev = (NVMe_Controller*)token_private;
    uint16_t cqid = token->token_private >> 64;
    dev->cq_interrupt_handler(cqid);          // cqs[cqid] 无边界
    return 1; // TOKEN_FLAG_MASK_TOKEN_SCHEDULE   <<< 恒置位 ⇒ 每个 CQ 中断都跑整段 resched→sched
}
```

配套 `x86_vecs_deliver_mgr.cpp:575-584`（默认设备分支 `if (res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE) resched(raw_frame);`）与 `NVMe_interrupts.cpp:164`（`sqs[sq_id].complete_commands_bank[cmd_id] = entry;`，`sq_id/cmd_id` 来自设备 CQE，未做 `< sq_count` / `< num_of_entries` 校验）。这条链把"**中断上下文里跑整段调度**"变成常态，是 §2.A/§2.B 那枚竞态的**触发器**。

---

## 3. 不能仅靠静态排除的（诚实标注）

1. **"那 8 字节是谁写的"未定**：静态只能证明"写入值=某取时/调度路径的 `call` 返回址、且不遵循受害者本核 `rsp`"，不能证明写入者是哪一个核/哪一个任务。需要运行时探针（§4）。
2. **FPA 是否会双重分配同一物理页**：`FreePagesAllocator`/buddy/`page_frame_state_mgr` 的状态机未逐行证明无 double-free→double-alloc；`kspace_vm_table->alloc_available_space` 是否可能返回**与在用的栈 VA 段重叠**的区间，本轮未逐行证明。（w13 §6.3 还记录了 `kspace_vm_table` 与页表**存在可见分歧**——若"占位语义"被误当 oracle，可能产生假阳。）
3. **恒等窗口别名是否被"按别名基址+错偏移"写**：我只确认「`PHYACC_VA` 经 `mem_init`/`exec_env_prepare` 在 AP bring-up 前就绪、任务栈页确有 `0xffff800040000000+phys` 的第二条 W=1 通道」，但**没有找到一个明显"把 VA 当 PA 传给 `PHYACC_VA`"的代码点**。不能证明"不存在"，只能说"未发现"。
4. **`ksetmem_8`/`ksystemramcpy`/`ksetmem_64` 的全部调用点尺寸**未逐一核对（`ksetmem_8` 的 `rdx` 是**字节数**，`wheels.asm:160-168`）；NVMe 里 `ksetmem_8(buf,0,bytes/cq_bytes/4096*4)` 看着自洽，但不能排除某处单位/长度算错。
5. **IST 共享（#GP↔#NMI=IST3、#PF↔#MC=IST2）在真机才可能触发**；TCG 下大概率不显形 ⇒ 静态"无证据"≠"无害"。FRED STKLVLS 与 IDT IST 的不一致（WRAITH §7.1）同样是"既存、未验"。
6. **`blocked_tasks_clamps_t::arr[64]` 越界**与 `entryies[5]`、`done_bitmap[512]` 的越界**可达性**未逐路径证明（需要 >64 同队列等待者 / >5 段）。
7. **探针自身的观测效应**：现行 `WRAITH_TRACE/WRAITH_LOG` 每次 `print` 在**栈上**放 `char buf[LOG_LINE_MAX]`（**1024 B**，`vprintkv2.h:20`），且挂在 `resched/sched/sleep_tasks_wake/设备中断默认分支` 等**深栈热路径**上 —— 它**不解释** 2026-09-16 的样本（探针 09-17 才加），但它**会加深栈占用、可能移动/放大竞态**（Heisenbug 风险，`wraith_probe.h` 已自述），通电验证时必须把 `wraith::probe_verbose` 当变量对待。

---

## 4. 通电后的验证清单（探针 / watchpoint / 断点 + 配方）

> 原则：**优先复用现有** `interrupt_log_ring`（`util/debug_tmp_ring_buff.h`，IRQ-safe、零 UART）与 `wraith_probe.h` 原语（`WRAITH_LOG`/`WRAITH_TRACE`/`wraith::rsp_now/gs_now/now_running_task/kernel_va_ok`）；**加断言要落在"观测前"、且命中用 `WRAITH_LOG` 留证后决定**是否 panic（为了把竞态留在现场，别一命中就 panic）。**每条探针尽量只加一个 `%llx`，别跨"飞走"的 `sched()/atomic_load()` 持锁。**

### 4.1 直证 §2.A（握手缺失 → 跨核同栈）—— 最高优先
- **探针 P1（阻塞侧）**：在 `block_if_equal_cppenter` 的 `set_blocked()` 之后、`push_tail` 之前，以及在 `next_task_with_routine()` 入口前，各写一条：
  `WRAITH_LOG("BLK tid=%llx cpu=%u sbase=%llx rsp=%llx t=%llx\n", tid, cpu, priv_stack_base, rsp_now(), get_microsecond_stamp())`。
- **探针 P2（唤醒侧）**：在 `bq_flush_pending` 两个循环里、以及 `wakeup_thread` 的 `set_ready()` 前，各写：
  `WRAITH_LOG("WAK tid=%llx from_cpu=%u rsp=%llx t=%llx\n", ...)`。
- **探针 P3（落地点）**：在 `sched()` 的 `atomic_load()` 前，写：
  `WRAITH_LOG("SWT to=%llx cpu=%u irsp=%llx sbase=%llx t=%llx\n", to_run, cpu, to_run->priv_ctx.core_ctx.idtctx.iret.rsp, to_run->priv_stack_base, get_microsecond_stamp())`。
- **判定（离线对环）**：同一 `tid` 若出现「`BLK…cpu=A`」→「`SWT to=tid cpu=B`」且**两条之间没有该核切走的迹象**（B 的 `t` 比 A 的 `next_task_with_routine` 还早，或 A 侧 `sleep_tasks_wake` 的 W0 面包屑与 B 的 SWT 在时间上重叠），即坐实"**阻塞者未切走就被异核顶上**"。再叠加：`t.priv_ctx.fred.rsp` 是否落在"另一核此刻的 `rsp≈同区域"。

### 4.2 直证 §2.B（同栈两核）—— 零成本探针
- **探针 P4（栈占用自证）**：给 `per_processor_scheduler::sleep_tasks_wake` 现有的 W0/W3 面包屑**补上 `priv_stack_base` 与"本核 `rsp` 是否落在本任务栈内"**（用 `wraith::in_range(rsp_now(), sbase, pages)`），并再加一条"**本帧槽 vs 本核 `rsp`**"的自证。命中即说明"有别的上下文在写本栈"。
- **watchpoint（若上 KVM/慢速复现）**：一旦知道受害 `tid`，对其栈页（如 w13 的 phys `0x2a23578`）下 **gdb 硬件写 watchpoint**（`watch *(long*)0xffff8002dc626578`），抓"哪条指令、哪个 `rip/cs`、`cr3` 写的"。**TCG 下请改用 TCG plugin mem 回调**（会话既有资产 `analysis/w13/w13-watch.c/.so`，正是"mem 回调带 cpu/pc/vaddr/value"）——它对 **guest 写地址** 敏感，能直接给出"谁写了那个字"。

### 4.3 证伪/证实 §2 其余候选（并行做，代价低）
- **P5（C：跨核越界写）**：在 `get_other_scheduler` 里加 `if (pid >= logical_processor_count) WRAITH_LOG("GOS-OOB pid=%u ...")`；在 `bq_flush_pending`/`wakeup_thread` 调它之前先打印 `t->belonged_processor_id`。**只记录不 panic**。
- **P6（B：栈页双重分配）**：`stack_alloc` 记录 `(phys_base, vbase, tid)`；`release_kthread` 记录 `(priv_stack_base, phys)`；离线查**phys 段重叠**或 **VA 段重叠**。（顺带查 `kspace_vm_table` 与页表的占位分歧是否会造成 VA 复用。）
- **P7（D：NVMe 设备/索引）**：在 `cq_interrupt_handler` 加 `if (sq_id >= sq_count || cmd_id >= sqs[sq_id].num_of_entries) { WRAITH_LOG("CQE-OOB ..."); continue; }`；记录所有 ring/缓冲 `{cq_ring_pa, sq_ring_pa, admin_buffer.pbase(), hmb_buffer.pbase()}`，离线比对**是否任一段落在某任务栈 phys 段内**。另在 `asynchronized_cmd_submit` 记录 `sq_ring.vbase()`。
- **P8（E：IST/异常栈）**：在 `#PF/#GP` 入口（已有 `FAULT_FREEZE`、`Sysdef_exception_entries.asm`）用 `wraith::log_exc_frame` 记录 **入口 `rsp` 落在哪个 IST**（比对 `tss.ist[1..3]`）与 `cs/lo`；单独核对每核 `tss.ist[2]/[3]` 是否被写成同一地址。
- **P9（F：栈缓冲越界）**：`pop_all`/`pop_timeouts` 写 `arr[count]` 前加 `if(batch->batch_count>=64) { WRAITH_LOG("CLAMP-FULL ..."); return; }`（先堵再证）；`split_vinterval_to_pages`/`entryies` 已有 `idx<5` 保护，抽查其在栈 pak 上的写。

### 4.4 配方（沿用武器库 §3，按 WRAITH §5/§8）
```bash
cd /home/PS/PS_git/OS_pj_uefi/kernel
# 1) 冻结现场档（抓"首个 #PF/#GP"的全 RAM + 全核寄存器）：FAULT_FREEZE 已在 asm 打开
Tools/tcg-trace/tcg-trace.sh --tag sv --repeat 40 --timeout 40 --cap-gb 1 --dump-vmcore
# 2) 观测竞态档（把探针环留全，别让 FAULT_FREEZE 早停）：
#    - 消融面包屑带宽：把 wraith::probe_verbose 置 0 跑对照；
#    - 保留 P1/P2/P3/P4（命中即留证、不 panic），跑 N 次比异常率与"跨核同栈"命中率。
# 3) 关键变量：SMP=6、-cpu max（TCG）、宿主负载 2~4×yes（12×会把 AP-bringup 超时顶出来，见 WRAITH §8-1）
```
- **对照协议**：`probe_verbose=1`（面包屑全开） vs `=0` vs **无探针基线**，各 N 跑，记录异常率 + 宿主负载。若"全开"把竞态压没，说明探针栈占用（1KB/次）改变了时序 —— 这本身就是"栈深敏感"的旁证。
- **结构性验证（修，不是探）**：加一个 **`-DWRAITH_ONCPU_GUARD`** 编译开关：给 `task` 加 `volatile uint32_t on_cpu`，在 `sched():293 atomic_load()` 前写"本核"，在 `sleep_tasks_wake` 看见"目标核已在跑该任务却被再次入队"时只是 `WRAITH_LOG` 不 panic。**这是把 §2.A 从"推断"变成"可判"的最短路径**（不改默认行为）。

---

## 5. 一页速览（给下一个接手的人）

- **要信的一件事**：受害槽里的值是 **`call` 压的返回址**（`0x2c700`=HPET 内层返回址；w22 里是 `sched()/set_clock_by_offset` 的帧）⇒ **写者此时此刻正在（某映射的）受害者栈上执行取时/调度代码**。任何"写的是指针/设备数据/零"的机制（C/D/E/F）都**不能**成为主因。
- **最可能**：**同一任务栈在两核上同时活着**（A：握手缺失；B：物理页被双分配）。A 的"发布即唤醒、先发布后切走"窗口在 `block_if_equal_cppenter`/`kthread_sleep_cppenter`/`bq_flush_pending` 里**代码级可见**，且与"NVMe 并行 init × 取时热点 × 负载敏感 × KVM 不复现"完全吻合。
- **最该先加的三条探针**：P1（BLK，阻塞侧）/ P2（WAK，唤醒侧）/ P3（SWT，落地侧）—— 三者一串就能判"跨核同栈"。
- **能立刻证的**：`bq_flush_pending` 的 `set_ready()` 不查状态 + `get_other_scheduler` 无边界（C 的机械基础）；`sched()` 跨核偷取无 owner 校验（A/B 的机械基础）。
- **别忘**：`tools/tcg-trace` 级联抓现场；`FAULT_FREEZE` 当前是**开**的（取证档），跑"观测竞态"要用常规档；探针每次 print 吃 **1KB 栈**，是变量。

> 全程只读；未运行任何东西；除本报告外未改动仓库任何文件。
