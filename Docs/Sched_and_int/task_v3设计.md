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
  priv_ctx = 空
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
    event_record_t          latest_record;
    miusecond_time_stamp_t  min_wakeup_stamp;
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

    // ── 上下文选择（由 task_save 写入，resume 只读） ──
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
    // 状态转换...
};
```

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

  4. 判断中断来源并写入 choose:
     if VM-exit (fred.errcode[63] or VM-exit info):
       // guest FPU 还在物理寄存器里 → 惰性
       if (gs->fpu_domain_tid == self->tid && gs->fpu_dirty)
           XSAVEC(self->vcpu_ctx->guest_xsave_area)
       // VMCS 自动保存了大部分 guest 状态
       self->choose = vCPU

     elif from user (CS.RPL==3):
       // 用户态上下文已经在 syscall/中断入口时存好了
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

## 五、syscall 入口设计

```
syscall_entry (asm):
  swapgs
  // per-CPU 栈当前 top 就在 RSP（由 TSS/FRED 自动加载）
  // 阶段 1: 快速保存用户上下文
  push rcx            // 用户 RIP (syscall 存 RCX)
  push r11            // 用户 RFLAGS
  push regs...
  // 栈上现在有 x64_standard_context frame

  // 阶段 2: 全局保存到 task->uctx（无脑，但不包括 FPU）
  mov rdi, rsp
  call kthread_syscall_entry
  // 内部: memcpy(self->uctx->xtd_ctx, stack_frame)
  //       标记 gs->fpu_domain_tid = self->tid
  //       标记 gs->fpu_domain_type = U_CTX
  //       标记 gs->fpu_dirty = false（干净，还没动）

  // 阶段 3: 切到 priv_stack
  // 初始RSP = priv_stack_base - 64B（见§三栈布局）
  mov rsp, self->priv_stack_base - 64

  // 正常处理 syscall，需要 FPU 时先检查：
  //   if (gs->fpu_domain_type == U_CTX)
  //       XSAVEC(self->uctx->xsave_area)
  //       gs->fpu_domain_type = KERNEL
  // 处理完毕 → 检查是否阻塞
  //   → 返回用户态: switch_to_user() → 铺帧 → iretq
  //   → 需要阻塞: 走调度器
```

**v2 没处理的场景 v3 解决：**

```
v2: syscall → 切到内核线程上下文 → 用 priv_stack → 被切走
    → task_save: CS.RPL==0 → choose=priv
    → FPU 里还有用户状态！保存给谁？——无法回答

v3: syscall → 立即搬用户帧到 uctx
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
         ▼ 中断 / syscall / VM-exit
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

## 九、未定事项

1. **per-task vs per-CPU 内核栈** — 目前入口用 per-CPU，后续可切到 per-task
2. **FPU 惰性第一次碰触检测** — 用 #NM？cr0.TS？还是直接检查
3. **vCPU ctx 中 PKRU 的惰性方案**
4. **AMX（XFD）按需分配**
5. **任务迁移（跨 CPU）**
6. **drs 加载完整实现（当前预留）**
7. **vCPU 的 Gantt 日志集成**
