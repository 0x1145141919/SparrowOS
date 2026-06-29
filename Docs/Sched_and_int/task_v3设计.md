# task v3 设计

> 2026-06-19 — 调度器/线程接口改造第三阶段
>
> v2 模型的核心错误：认为"所有任务都是内核线程，用户态/vCPU 是内核线程的度假目标"。
> 实际 ISA 不对称：iretq/eretu/erets/sysexit/sysretq 是纯加载指令，不保存任何返回点。
> 内核执行流是短暂的 CPL0 片段，不是连续的内核线程。
>
> v3 反转模型：用户线程/vCPU 是主角，内核上下文是受托管的临时状态。

---

## 一、核心模型：用户态/vCPU 是主体，内核是短暂的 CPL0 片段

```
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  每个 task 的主体是一个用户态执行流（或 vCPU 执行流）。     │
│  内核上下文不是 task 的"常驻身份"，而是调度切换时的         │
│  临时缓冲区。                                               │
│                                                             │
│  ISA 的现实:                                                 │
│    用户→内核: int/syscall  → 硬件保存跳转（有保存点）      │
│    内核→用户: iretq/sysret → 硬件纯加载跳转（无保存点）    │
│    内核→Guest: VMRESUME     → 硬件纯加载                    │
│    Guest→VMM: VM-exit       → 硬件保存跳转                  │
│                                                             │
│  内核执行流是"借来的片段"——中断进来开始，依赖当前 CPU       │
│  的状态处理业务，处理完跳回去。内核没有"自己是线程"的      │
│  概念。                                                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 生命周期

```
task::basic_constructor() → task 诞生
  uctx = nullptr
  vcpu_ctx = nullptr
  priv_ctx = 全零（basic allocator zero-on-alloc 保证）
  priv_stack_base = nullptr
  priv_stack_pages = 0

task::launch() — 纯机械：加载 priv_ctx 开始执行。
  调用方必须在 launch 前填好 priv_ctx、分配好 priv_stack。
  launch 本身不做任何分配/填充决策。

  launch 后运行的是 priv_ctx 里的入口代码。该代码：
  ├─ 可以 new u_ctx_t → 填充 → iretq → 成为用户线程
  ├─ 可以分配 VMCS + vcpu_ctx → VMRESUME → 成为 vCPU
  └─ 也可以只做一次性内核业务 → 做完 exit / yield

  中断/VM-exit 回来 → 处理事件 → 可能再次 iretq/VMRESUME，可能 yield 切走
```

**v2 说的"内核线程是主体"在哪错了：**

```
v2 认为:  内核线程 → iretq → 用户态 → 中断 → 回来继续
           ↑                                   ↓
         （连续的内核执行流，用户态是暂停点）

事实:     iretq 是纯加载，不保存内核返回点
  
          内核: syscall_handler  ← 新 CPL0 片段
                │
                ↓ iretq
          用户态: 连续执行流    ← 这才是"主体"
                │
                ↓ #PF
          内核: pf_handler     ← 另一个 CPL0 片段，无关的
```

---

## 二、v2 → v3 关键改进

| 方面 | v2 | v3 |
|---|---|---|
| **主体身份** | 内核线程是主角 | 用户线程/vCPU 是主角 |
| **priv_ctx 语义** | task 的内联内核身份 | 调度切换的临时缓冲区（__attribute__((transient))）|
| **uctx 分配** | 硬编码 this+4K 偏移 | 指针，独立 new/delete |
| **priv_stack** | 24KB 硬编码在 32KB 块内 | 指针 + 独立 alloc/free，按需分配 |
| **32KB统一块** | task+uctx+stack 打包分配 | 拆开，各部分独立生命周期 |
| **FPU 管理** | 未明确 | 惰性，per-CPU fpu_domain_tid 跟踪 |
| **u_ctx_t 字段管理** | 所有字段"存/取" | 大部分字段只有 load，没有 save（内核唯一作者）|
| **syscall 入口** | 不明确 | 立即保存用户上下文到 uctx |
| **vCPU 上下文** | 未入设计文档 | VMCS 自动 + VMM 手动混合管理 |
| **调度器视角** | 知道三种任务类型 | 完全透明，只看 priv_ctx 和 choose |
| **启动路径** | launch → 内核线程 → 选择去用户/guest | launch 是纯机械加载（不决策），决策在 priv_ctx 的入口代码中 |

---

## 三、数据结构：指针化、解耦

### task

> 📐 栈布局图解见 [`priv_stack_layout.md`](priv_stack_layout.md)

```cpp
class task {
    // ── 调度核心字段 ──
    task_state_t            task_state;
    uint32_t                belonged_processor_id;
    uint64_t                tid;
    reentrant_spinlock_cpp_t task_lock;
    task_blocked_reason_t   blocked_reason;

    // ── 时间账本 ──
    miusecond_time_stamp_t  current_event_start_stamp;
    event_type_t            current_event;
    miusecond_time_stamp_t  min_wakeup_stamp;   // 超时唤醒时间戳。当 current_event 为
                                                    // sleep/wait_io/wait_other 且当前时间 > 此值
                                                    // 时，任务应被强制唤醒（超时）。0 表示不限时。
    // 时间账本：accumulates_bank[event_type]，按事件类型索引
    enum event_type_t {
        init,
        run_kthread,
        run_uthread,
        run_vCPU,
        offline,
        sleep,
        wait_io,
        wait_other,
        wait_mutex,
        event_type_COUNT
    };
    miusecond_time_stamp_t  accumulates_bank[event_type_COUNT];

    // ── 内核上下文（调度切换缓冲区，仅切换时有意义） ──
    // inline：每个 task 必然有 priv_ctx，独立分配无意义
    x64_standard_context_v2 priv_ctx;
    vaddr_t                  priv_stack_base;   // __wrapped_pgs_alloc(pages)，4K对齐，即栈顶
    uint32_t                 priv_stack_pages;  // 总页数（含 guard page）
    // 栈布局（高位→低位）：
    //   priv_stack_base                                              — 栈顶
    //   [priv_stack_base - 4K, priv_stack_base)                        — guard page（未映射，#PF not-present）
    //   [priv_stack_base , priv_stack_base + 4K * priv_stack_pages-64B) — 可用栈空间
    //   初始RSP = priv_stack_base + 4K * priv_stack_pages-64B（留64B作RBP回溯缓冲区，FRED兼容）

    // ── 交出执行流时的上下文类型（由 task_save 写入，resume 只读） ──
    enum ctx_choose { priv, u_ctx, vCPU };
    ctx_choose              choose;

    // ── 主体上下文（关键创新：这才是 task 的身份） ──
    u_ctx_t*                uctx;          // new u_ctx_t，仅用户线程
    void*                   vcpu_ctx;      // 独立分配，仅 vCPU

    // ── waiters ──
    Ktemplats::list_doubly<task*> waiters;

    // ── 接口 ──
    static task* basic_constructor();
    void  launch();       // 纯机械：从 priv_ctx 加载开始执行
    void  resume();       // 根据 choose 无脑加载对应上下文
    void  task_event_shift(event_type_t new_event);  // 关旧帐、开新帐，见 §三.1
    // 状态转换...
};
```

### 三.1 task_event_shift — 关旧帐、开新帐

```cpp
void task::task_event_shift(event_type_t new_event)
{
    if (new_event == this->current_event)  // 同一事件继续，不计时
        return;

    miusecond_time_stamp_t now = ktime::get_microsecond_stamp();
    uint64_t elapsed = now - this->current_event_start_stamp;

    // ① 归帐：旧事件下台，时段写入 accumulates_bank
    this->accumulates_bank[this->current_event] += elapsed;

    // ② 甘特图拓展（可选）：若当时有 dts_gantt
    //    此处可写一条 Gantt 日志：old_event → new_event，持续 elapsed

    // ③ 新事件开始计时
    this->current_event_start_stamp = now;
    this->current_event = new_event;
}
```

**设计思想 — 一个函数完成两个操作：**

1. **关旧帐** — 旧事件下台，将其存活时长计入对应 `accumulates_bank[old]`。
   若已集成 Gantt 日志，同时写一条事件切换记录到 `dts_gantt`。
2. **开新帐** — 新事件上场，记录当前时间戳作为起始点。

**关键约定：**
- `current_event` 在 `basic_constructor` 中被初始化为 `init`，`current_event_start_stamp` 全零。
  第一次 `task_event_shift` 自动把 `init→run_kthread` 的时段计入 `accumulates_bank[init]`。
- `current_event` 与 `accumulates_bank` 形成**闭包**：任何执行流切换必须先调 `task_event_shift`，
  确保不遗漏任何时间的归属。典型的调用点：
  - `block_if_equal` 切出前 → `task_event_shift(wait_io/wait_other)`
  - `kthread_sleep` 切出前 → `task_event_shift(sleep)`
  - 被唤醒调度回来时 → `task_event_shift(run_kthread/run_uthread/run_vCPU)`
  - `task_save` 中被切走前 → 同上（但 task_save 本身不调 shift，调用方负责）

### 三.2 min_wakeup_stamp — 超时边界

```
┌───────────────────────────────────────────────────────────┐
│                                                           │
│  min_wakeup_stamp 是任务阻塞/睡眠的超时边界。              │
│                                                           │
│  current_event ∈ {sleep, wait_io, wait_other, wait_mutex}  │
│  ∧ 当前时间 > min_wakeup_stamp                            │
│  → 任务应被强制唤醒（超时）                                │
│                                                           │
│  min_wakeup_stamp == 0 表示不限时（无超时）               │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

**设置点：**
- `kthread_sleep(offset_us)` → `sleep_queue` 插入，`min_wakeup_stamp = now + offset_us`
- `block_if_equal(..., timeout_us ≠ 0)` → `block_queue` 插入，`min_wakeup_stamp = now + min(timeout_us, MAX_BLOCK_TIME)`
- `block_if_equal(..., timeout_us = 0)` → `block_queue` 插入，`min_wakeup_stamp = 0`（不限时）
- 唤醒后调用方自行 `min_wakeup_stamp = 0`（清除超时边界）

超时检查不依赖统一队列扫描——两条路径分离，见 §十。

### u_ctx_t

```cpp
struct u_ctx_t {
    x64_standard_context_v2 xtd_ctx;    // GPRs + 中断帧

    // 以下字段由内核严格管控——没有"保存"只有"加载"
    AddressSpace*           as;          // 页表（load_cr3）
    uint32_t                xcr0_mask;
    uint64_t                cr4_mask;
    uint64_t                drs[8];      // DR0-DR7
    vaddr_t                 gs_base;     // wrmsr(MSR_GS_BASE)
    vaddr_t                 fs_base;     // wrmsr(MSR_FS_BASE)

    // 以下字段是真正的"用户状态"——需要惰性保存
    void*                   xsave_area;  // new 或 __wrapped_pgs_alloc
    uint64_t                xsave_size;
};
```

**v2 的问题：uctx 通过 this+4K 偏移引用，XSAVE 超 4KB 就破。v3：指针，大小不限。**

### vCPU 上下文

```cpp
// 独立分配，不在 task 块内
struct vcpu_ctx_t {
    vmcs_t*                 vmcs;            // 4KB VMCS 页

    // VMCS 自动管理的——不需要 VMM 额外 save/restore：
    //   GUEST_CS/DS/ES/FS/GS/SS/LDTR/TR (selector, base, limit, AR)
    //   GUEST_CR0, GUEST_CR3, GUEST_CR4
    //   GUEST_FS_BASE, GUEST_GS_BASE
    //   GUEST_RIP, GUEST_RSP, GUEST_RFLAGS
    //   GUEST_GDTR, GUEST_IDTR
    //   GUEST_DR7

    // 需要 VMM 手动 save/restore 的（VMCS 不自动管理）：
    void*                   guest_xsave_area;  // FPU → 惰性 XSAVEC
    uint64_t                xsave_size;
    uint32_t                guest_xcr0;        // XSETBV 必 VM-exit, 但需手动管
    uint32_t                host_xcr0;
    uint64_t                guest_kernel_gs_base; // IA32_KERNEL_GS_BASE

    // VM-entry/exit MSR load/store lists（见 VMCS）
    // ...
};
```

---

## 四、上下文保存/恢复

### task_save — 保存时判断来源，写入 choose

```
task_save(frame):
  1. self = read_gs(NOW_RUNNING_TASK)
  2. self->priv_ctx = *frame              // 内核 GPRs → 临时缓冲区

  3. 更新时间账本
     // task_save 的调用方（中断/异常入口）必须已在切出前调 task_event_shift
     // 关闭当前事件（如 run_uthread/run_vCPU）并归档到 accumulates_bank。
     // 见 §三.1。此处不重复做 shift——调用方负责。

  4. 判断交出执行流的上下文类型并写入 choose:
     if VM-exit (fred.errcode[63] or VM-exit info):
       // guest FPU 还在物理寄存器里 → 惰性
       if (gs->fpu_domain_tid == self->tid && gs->fpu_dirty)
           XSAVEC(self->vcpu_ctx->guest_xsave_area)
       // VMCS 自动保存了大部分 guest 状态
       self->choose = vCPU

     elif from user (CS.RPL==3):
       // 用户态上下文已经在 int 227/中断入口时存好了
       // 但 FPU 可能还没存（惰性）
       if (gs->fpu_domain_tid == self->tid && gs->fpu_dirty)
           XSAVEC(self->uctx->xsave_area)
       self->choose = u_ctx

     else:                                // 来自内核 CPL0 片段
       self->choose = priv                // FPU 是内核的，不保存

  5. 更新 FPU 域标记
     gs->fpu_domain_tid  = INVALID        // FPU 不再属于任何 task
     gs->fpu_domain_type = KERNEL
     gs->fpu_dirty       = false

  6. return self
```

### launch — 纯机械启动

```
launch():
  // priv_ctx 已由调用方填好（RIP、RSP），此函数只负责加载执行
  // 调用方在 launch 前必须保证 priv_stack 已分配、priv_ctx 已就绪
  switch_to_context(priv_ctx)
```

调用方范例 —— 创建一个用户线程：

```
task* t = task::basic_constructor();

t->priv_stack_base  = stack_alloc(&kurd, 3);   // 3页 = 1 guard + 2 可用
t->priv_stack_pages = 3;

t->priv_ctx.rip = (vaddr_t)user_thread_entry;  // 内核入口函数
t->priv_ctx.rsp = t->priv_stack_base - 64;     // 初始栈顶
t->priv_ctx.cs  = KERNEL_CS;                    // CPL0
// ... 其他必要字段

t->launch();  // 开始执行内核入口

// 内核入口内部：
//   new u_ctx_t → 填用户上下文 → iretq → 用户态
```

调用方范例 —— 内核一次性业务：

```
task* t = task::basic_constructor();
t->priv_stack_base  = stack_alloc(&kurd, 2);   // 2页 = 1 guard + 1 可用
t->priv_stack_pages = 2;
t->priv_ctx.rip = (vaddr_t)some_kernel_job;
t->priv_ctx.rsp = t->priv_stack_base - 64;
t->launch();
// 执行 some_kernel_job → 完成 → exit/yield
```

### resume — 只看 choose，无脑加载

```
resume():
  switch self->choose:
    case priv:
      iretq(self->priv_ctx)

    case u_ctx:
      load_regs(self->uctx)   // AS、FS/GS base、DR、FPU 等
      iretq(self->uctx->xtd_ctx)

    case vCPU:
      load_vmcs_host()
      load_guest_specific(self->vcpu_ctx)  // FPU、KERNEL_GS_BASE 等
      VMRESUME
```

resume 的 load_regs / load_vmcs_host / load_guest_specific 是底层实现细节，
不属于 resume 的语义：resume 只负责"选一个上下文，让它跑"。

---

## 五、用户态入口设计（int 227 — USER_ABI_ENTER）

SparrowOS 不提供 x86-64 `syscall`/`sysret` 指令路径。用户→内核切换
统一走软中断 `int 227`（`USER_ABI_ENTER`）。

**理由：**
- 无需设置 `IA32_STAR`/`IA32_LSTAR`/`IA32_FMASK` 等 MSR
- 无需 `swapgs`：硬件通过门描述符的 DPL 校验自动完成栈切换（IDT 下 TSS 加载 RSP，FRED 下 FRED 引擎处理）
- `int` 指令在 CPL 切换时硬件自动压入 SS:RSP、CS:RIP、RFLAGS——入口处已有一个完整的中断帧
- 一套入口，和硬件中断、异常走相同的处理基础设施

**代价：**
- 比 `syscall` 慢数十周期（微架构的 `int` vs `syscall` 延迟差）
- 但内核调度/切换的代价远大于此，不值得为这点差异多维护一套路径

```
user_abi_entry (asm):
  // 阶段 1: 保存剩余 GPRs → 完整 x64_standard_context
  //   硬件已压好 SS:RSP, CS:RIP, RFLAGS（int 227，CPL3→CPL0）
  push regs...
  // 栈上现在有完整的 x64_standard_context frame

  // 阶段 2: 全局保存到 task->uctx（无脑，但不包括 FPU）
  mov rdi, rsp
  call user_abi_entry_cpp
  // 内部: memcpy(self->uctx->xtd_ctx, stack_frame)
  //       标记 gs->fpu_domain_tid = self->tid
  //       标记 gs->fpu_domain_type = U_CTX
  //       标记 gs->fpu_dirty = false（干净，还没动）

  // 阶段 3: 切到 priv_stack
  // 初始RSP = priv_stack_base - 64B（见§三栈布局）
  mov rsp, self->priv_stack_base - 64

  // 正常处理，需要 FPU 时先检查：
  //   if (gs->fpu_domain_type == U_CTX)
  //       XSAVEC(self->uctx->xsave_area)
  //       gs->fpu_domain_type = KERNEL
  // 处理完毕 → 检查是否阻塞
  //   → 返回用户态: switch_to_user() → 铺帧 → iretq
  //   → 需要阻塞: 走调度器
```

**v2 没处理的场景 v3 解决：**

```
v2: 入口 → 切到内核线程上下文 → 用 priv_stack → 被切走
    → task_save: CS.RPL==0 → choose=priv
    → FPU 里还有用户状态！保存给谁？——无法回答

v3: 入口 → 立即搬用户帧到 uctx
    → 标记 FPU 所有权
    → 切 priv_stack
    → 被切走时 FPU 已经标记好了 → 干净的 task_save
```

---

## 六、FPU 惰性管理

### per-CPU 跟踪状态（gs_complex）

```cpp
struct {
    uint64_t owner_tid;      // FPU 当前属于哪个 task
    uint8_t  owner_type;     // U_CTX | VCPU | KERNEL | NONE
    bool     dirty;          // FPU ≠ 内存副本
} fpu_state;
```

### 完整流程

```
┌─ 用户态 / Guest 运行 ─────────────────────┐
│  FPU 物理寄存器 = 用户/guest 的有效状态     │
│  gs->fpu_state = { tid=A, type=U_CTX, dirty=false }
└───────────────────────────────────────────┘
         │
         ▼ 中断 / int 227 / VM-exit
┌─ 入口代码 ─────────────────────────────────┐
│  搬 GPRs → uctx                            │
│  ！！！此处不动 FPU！！！                   │
│  gs->fpu_state.dirty = false（不变）        │
└───────────────────────────────────────────┘
         │
         ▼ 内核处理过程中第一次碰 FPU（如 SSE memcpy）
┌─ 内核用 FPU ───────────────────────────────┐
│  if (gs->fpu_state.owner_type == U_CTX) {   │
│      XSAVEC(self->uctx->xsave_area)         │
│      gs->fpu_state.dirty = false            │
│  }                                           │
│  if (gs->fpu_state.owner_type == VCPU) {    │
│      XSAVEC(self->vcpu_ctx->xsave_area)     │
│  }                                           │
│  gs->fpu_state.owner_type = KERNEL          │
│  // 用 FPU...                                │
│  gs->fpu_state.dirty = true                 │
└───────────────────────────────────────────┘
         │
         ▼ task_save（被切走）
┌─ 保存 FPU（如果需要）───────────────────────┐
│  if (gs->fpu_state.owner_tid == self->tid   │
│      && gs->fpu_state.dirty) {               │
│      switch (gs->fpu_state.owner_type) {     │
│          U_CTX: XSAVEC(self->uctx->xsave)    │
│          VCPU:  XSAVEC(self->vcpu_ctx->xsave)│
│          KERNEL: // 不需要保存               │
│      }                                       │
│  }                                           │
│  gs->fpu_state.owner_tid = INVALID          │
│  gs->fpu_state.owner_type = NONE            │
└───────────────────────────────────────────┘
         │
         ▼ resume（调度回来）
┌─ 加载 FPU（如果需要）───────────────────────┐
│  if (gs->fpu_state.owner_tid != new->tid) { │
│      XRSTOR(new->FPU area based on choose)  │
│      gs->fpu_state.owner_tid = new->tid     │
│      gs->fpu_state.owner_type = new->choose │
│      gs->fpu_state.dirty = false            │
│  }                                           │
│  // iretq/VMRESUME                           │
└───────────────────────────────────────────┘
```

---

## 七、vCPU 上下文管理（VMCS 与 VMM 的分工）

### 硬件自动管理（VMCS 双缓冲）

| 寄存器 | VMCS guest-state | VMCS host-state | 备注 |
|---|---|---|---|
| CS/DS/ES/FS/GS/SS selector | ✅ | ✅ | |
| FS.base / GS.base | ✅ GUEST_FS_BASE/GS_BASE | ✅ HOST_FS_BASE/GS_BASE | **VM-exit 自动切换** |
| CR0 / CR3 / CR4 | ✅ GUEST_CR0/3/4 | ✅ HOST_CR0/3/4 | 物理寄存器中间态通过 read_shadow 同步 |
| GDTR / IDTR | ✅ | ✅ | |
| LDTR / TR | ✅ | ✅ (仅 TR) | |
| RFLAGS / RIP / RSP | ✅ | ✅ | |
| DR7 | ✅ | ❌ | |

### VMM 手动管理（VMCS 无对应字段或只是控制位）

| 项 | 管理方式 | 原因 |
|---|---|---|
| **FPU (XMM/YMM/ZMM/x87)** | 惰性 XSAVEC/XRSTOR | guest 自由修改，无 VM-exit |
| **XCR0** | VM-entry 前/VM-exit 后手动 XSETBV | XSETBV 必 VM-exit，但 VMCS 无字段 |
| **PKRU** | 如果 MSR bitmap 放行则需手动 | WRPKRU 不触发 VM-exit |
| **IA32_KERNEL_GS_BASE** | VM-entry/exit MSR load/store list | 不在 VMCS 中 |
| **DR0-DR3** | 如果 MOV-DR exiting=0 则需手动 | VMCS 只有 DR7 |
| **MSR bitmap 放行的 MSR** | VM-entry/exit MSR list | 通过 MSR load/store area |

### v3 新增的 vCPU 上下文生命周期

```
VM-entry 前:
  wrmsr(KERNEL_GS_BASE, vcpu->guest_kernel_gs_base)
  XSETBV(vcpu->guest_xcr0)
  // VMCS 已配置好 → VMRESUME

VM-exit 后:
  // VMCS 自动保存了大部分 guest 状态
  rdmsr(KERNEL_GS_BASE, &vcpu->guest_kernel_gs_base)  // 手动捞
  // XCR0: VMM 决定何时读（或 VMCS 有不触发 XSETBV 的读）
  // FPU: 惰性（切走时 XSAVEC）
  wrmsr(KERNEL_GS_BASE, host_kernel_gs_base)
  XSETBV(host_xcr0)
```

---

## 八、三种执行流对比

```
               创建              主体身份          被中断时 task_save
               ────             ────────          ────────────────
内核一次性业务: basic_constructor  priv_ctx（临时的）  choose=priv
               launch()          → exit/yield      只保存 GPRs，FPU 不存

用户线程:      basic_constructor  u_ctx_t            choose=u_ctx
               launch()          → iretq → 用户态   搬 GPRs + 惰性 FPU
                                → 中断回来          恢复时加载 AS/FS/GS/DR/FPU

vCPU:          basic_constructor  vcpu_ctx            choose=vCPU
               launch()          → VMRESUME→Guest    VMCS 自动+惰性 FPU
                                → VM-exit           恢复时只管 VMCS 不管的
```

---

## 十、超时唤醒机制 — 分散式 BQ 超时扫描

### 10.1 问题：谁检查 min_wakeup_stamp？

v2/v3 初期使用单一 `sleep_queue` 处理所有超时（sleep + wait_* 都在一条排序队列上）。
这要求 `block_if_equal` 超时任务也放入 sleep_queue，造成两条路径（定时睡眠 vs 条件阻塞）
共享同一扫描通道。

最新设计：两条路径完全分离。

```
kthread_sleep → sleep_queue（排序插入 + sched() 前 sleep_tasks_wake 扫描）
block_if_equal 超时 → block_queue（FIFO + sched() 前随机轮转 BQ 扫描）
```

### 10.2 MAX_BLOCK_TIME 全局兜底

```cpp
constexpr uint64_t MAX_BLOCK_TIME_US = 5_000_000;  // 5000ms
```

`block_if_equal` 的 `timeout_us` 实际生效值：

```
effective = min(timeout_us, MAX_BLOCK_TIME_US)
```

防止调用方设置无限制超时，确保系统在任何阻塞场景下都有兜底唤醒。

### 10.3 分散式 BQ 超时扫描（随机轮转临幸法）

`block_if_equal` 超时任务的 `min_wakeup_stamp` 写入后，task 只进入 BQ 的 inner_queue
（FIFO），不进入 sleep_queue。超时唤醒由各 CPU 在调用 `sched()` 前通过随机轮转扫描实现。

```
sched() 调用前：
  bq_timeout_scan_rand()       // 随机选一个 BQ 扫超时
  sleep_tasks_wake()           // 扫 sleep_queue（kthread_sleep 专用）
  sched()                      // 取下一个任务
```

**扫描函数 `bq_timeout_scan_rand()` 逻辑：**

```
1. 熵源 = 调用者已测得的微秒时间戳（不额外 TSC/MMIO 读）
   uint32_t entropy = (uint32_t)(micro_stamp ^ (micro_stamp >> 16));
   uint32_t bq_idx = entropy & (global_bq_count - 1);  // 2^n 约束

2. lock = get_lock(global_bq_table[bq_idx])
3. if lock == NULL: return
4. lock->lock()
5. count = bq_wake_timeouts(global_bq_table[bq_idx])
     // 从 inner_queue head 向后遍历
     // 对 min_wakeup_stamp != 0 && now > min_wakeup_stamp 的 task：
     //   pop_front → 出列
     //   记入本地列表
     // 返回 pop 个数
6. lock->unlock()
7. for each popped task t:
     task_lock(t)
     t->min_wakeup_stamp = 0
     task_event_shift(t, run_kthread)
     t->set_ready()
     task_unlock(t)
     insert_ready_task(t)
```

**FIFO O(n) 扫描说明：**

block_queue 按 FIFO 维护，不按 `min_wakeup_stamp` 排序。`bq_wake_timeouts` 必须从 head
扫到尾。O(n) 复杂度在以下场景可接受：
- 大多数 BQ 只有 0-1 waiter（per-CID NVMe）
- 共享 BQ（如 i8042）通常只有 1 waiter
- 5000ms MAX_BLOCK_TIME 提供充裕宽容度

### 10.4 随机轮转的覆盖率

单个 CPU 每次 `sched()` 只扫描一个 BQ。多个 CPU 并发各自随机扫描，长期来看所有 BQ
被公平覆盖。不要求每次 `sched()` 都命中目标 BQ——超时本身有 5000ms 的缓冲，
概率上即使单 CPU 系统也能在几个调度周期内覆盖所有活跃 BQ。

### 10.5 精确性说明

由于随机轮转的分散特性，一个 BQ 的实际超时扫描间隔是不确定的（依赖该 CPU 或其他 CPU
的 sched 频率）。这不影响正确性——超时是容错的兜底边界，不是精确的时间承诺。
需要精确时序的场景应使用 `kthread_sleep`（走 sleep_queue + timer 中断扫描，
排序队列可精确到最早到期任务）。

### 10.6 任务消费的互斥性

`kthread_sleep` → sleep_queue
`block_if_equal` → block_queue

两条队列互不交叉。同一个 task 不可能同时在两条队列上。`bq_wake_timeouts` 和
`sleep_tasks_wake` 扫描的无序性是安全的——它们扫描的是不同的任务集。

---

## 九、未定事项

1. **per-task vs per-CPU 内核栈** — 目前入口用 per-CPU，后续可切到 per-task
2. **FPU 惰性第一次碰触检测** — 用 #NM？cr0.TS？还是直接检查
3. **vCPU ctx 中 PKRU 的惰性方案**
4. **AMX（XFD）按需分配**
5. **任务迁移（跨 CPU）**
6. **drs 加载完整实现（当前预留）**
7. **vCPU 的 Gantt 日志集成**
