# task 设计文档

> 2026-06-19 — 调度器/线程接口改造第二阶段

---

## 一、核心模型：所有任务都是内核线程

```
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  每个 task 就是一个内核线程。                               │
│  ─ 没有 kthreadm / userthread / vCPU 之分。                │
│  ─ userthread 和 vCPU 是内核线程主动调用函数后的结果。     │
│  ─ 可以被抢占、阻塞、切走、唤醒的始终是内核线程本身。     │
│                                                             │
│  用户态或 Guest 态不是"线程类型"，而是内核线程的"度假目标"。│
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 生命周期

```
basic_constructor() → task 诞生，只有 priv_ctx，没有 uctx，没有 vCPU

task->launch():
  ──→ 加载 priv_ctx → 执行内核入口函数
      │
      ├─ 入口函数自我初始化：
      │   • 分配 uctx 或 vCPU ctx 所需资源
      │   • 填充 xtd_ctx（用户态的 RIP/CS/RFLAGS/RSP/SS）
      │   • 或填充 VMCS（Guest 状态）
      │
      ├─ 然后任务可以决定：
      │   • 继续做内核业务（普通内核线程）
      │   • iretq → 用户态跑用户代码，等中断/异常回来（用户线程）
      │   • VMRESUME → Guest 跑虚拟机代码，等 VM-exit 回来（vCPU）
      │
      └─ 无论如何，调度器回来时继续内核入口函数的剩余逻辑
          （处理完中断原因后，可能再次 iretq/VMRESUME，也可能阻塞）
```

**用户线程本质：内核线程调用了 `switch_to_user()`，内含 iretq。**

**vCPU 本质：内核线程调用了 `switch_to_guest()`，内含 VMRESUME。**

两种都是同步函数调用，内核线程在调用点"暂停"，等事件回来继续。

---

## 二、数据流总览

```
                          ┌──────────────────┐
                          │  task::launch()   │
                          │  加载 priv_ctx    │
                          └────────┬─────────┘
                                   │
                          ┌────────▼─────────┐
                          │  内核入口函数      │
                          │  (priv_ctx RIP)   │
                          └──┬────┬────┬─────┘
                             │    │    │
               ┌─────────────┘    │    └──────────────┐
               ▼                  ▼                   ▼
         普通内核线程        用户线程              vCPU
         (不做额外初始化)    (初始化 uctx)       (分配 VMCS)
               │                  │                   │
               │            iretq (u)           VMRESUME
               │                  │                   │
               │            用户态代码            Guest 代码
               │                  │                   │
               │            中断/异常             VM-exit
               │                  │                   │
               ▼                  ▼                   ▼
          ┌─────────────────────────────────────────────┐
          │        task_save() → 调度器切走             │
          │        自动判断来源写入 choose              │
          │        priv_ctx 保存内核上下文              │
          │        + 按需保存 uctx / vCPU ctx          │
          └─────────────────────────────────────────────┘
                                   │
                          ┌────────▼─────────┐
                          │  task::resume()   │
                          │  只读 choose      │
                          │  恢复 priv_ctx    │
                          └──┬────┬────┬─────┘
                             │    │    │
                ┌────────────┘    │    └──────────────┐
                ▼                 ▼                   ▼
         priv_ctx iretq     uctx→xtd_ctx iretq    VMRESUME
         (继续内核逻辑)      (回用户态)            (回 Guest)
```

---

## 三、32KB 统一内存块布局

```
8 pages = 32KB, 一次 __wrapped_pgs_valloc(8) 分配

┌──── Page 0 ────┐ 0
│ task header     │ ~200B (state/pid/tid/lock/timers/waiters)
│                 │
│ priv_ctx        │ sizeof(x64_standard_context_v2) ≈ 128B
│                 │  (GPRs + core_ctx union)
│ priv_stack_bot  │ 8B
│ priv_stack_top  │ 8B
│ uctx*          │ 8B   ← 可选：非内核线程设这个指针
│ ctx_choose      │ 1B (+padding)
│                 │
│ (剩余空闲)      │ ← 留给栈扩展的缓冲
├──── Page 1 ────┤ 4K
│ u_ctx_t         │     ← 内核入口函数可选分配的内容
│   xtd_ctx      │ x64_standard_context_v2
│   AddressSpace* │
│   xcr0_mask    │ 4B
│   cr4_mask     │ 8B
│   drs[8]       │ 64B (调试寄存器)
│   gs_base      │ 8B
│   fs_base      │ 8B
│ ──align(64)─── │
│   xsave_area   │ ～2.5KB (XSAVEC 压缩格式)
│   xsave_size   │ 8B
├──── Pages 2-7 ─┤ 8K
│ privstack      │ 24KB (向下增长)
└────────────────┘ 32K

vCPU 上下文（独立分配，不在 32KB 块内）:
  ┌──────────────┐
  │ VMCS (4KB)   │ ← VM-entry 前写入，VM-exit 后读取
  │ Guest XSAVE  │
  │ ...          │
  └──────────────┘
```

### 偏移常量

```cpp
#define TASK_BLOCK_SIZE        32768
#define TASK_UCTX_OFFSET       4096     // uctx = this + 4KB
#define TASK_STACK_BASE        8192     // privstack 起点
#define TASK_STACK_SIZE        24576
```

---

## 四、核心数据结构

### task

```cpp
class task {
    // ── 调度核心字段 ──
    task_state_t            task_state;
    uint32_t                belonged_processor_id;
    uint64_t                tid;
    reentrant_spinlock_cpp_t task_lock;
    task_blocked_reason_t   blocked_reason;

    // ── 时间账本 ──
    enum event_type_t { run, sleep, wait_io, wait_other };
    struct event_record_t {
        miusecond_time_stamp_t base;
        uint32_t               span_length;
        event_type_t           type;
    };
    event_record_t          latest_record;
    miusecond_time_stamp_t  min_wakeup_stamp;
    miusecond_time_stamp_t  accumulated_running;
    miusecond_time_stamp_t  accumulated_sleeping;
    miusecond_time_stamp_t  accumulated_io_blocking;
    miusecond_time_stamp_t  accumulated_waiting_other;

    // ── 内核上下文（内联，永远存在） ──
    x64_standard_context_v2 priv_ctx;
    vaddr_t                 priv_stack_bottom;
    vaddr_t                 priv_stack_top;

    // ── 上下文选择（由 task_save 写入，resume 只读） ──
    enum ctx_choose { priv, u_ctx, vCPU };
    ctx_choose              choose;
    u_ctx_t*                uctx;      // → this + 4KB（可选分配）
    // vCPU ctx: 独立分配，通过额外字段或全局映射引用

    // ── waiters ──
    Ktemplats::list_doubly<task*> waiters;

    // ── 接口 ──
    static task* basic_constructor();
    void  launch();     // 加载 priv_ctx → 内核入口（首次启动）
    void  resume();     // 根据 choose 恢复上下文
    bool  set_ready();
    bool  set_blocked();
    bool  set_running();
    bool  set_zombie();
    bool  set_dead();
    void  assign_valid_tid(uint64_t tid);
};
```

### u_ctx_t（用户态上下文快照）

```cpp
struct u_ctx_t {
    x64_standard_context_v2 xtd_ctx;    // 用户 iret 帧 + GPRs
    AddressSpace*           as;
    uint32_t                xcr0_mask;
    uint64_t                cr4_mask;
    uint64_t                drs[8];     // DR0-DR7
    vaddr_t                 gs_base;
    vaddr_t                 fs_base;
    void*                   xsave_area;
    uint64_t                xsave_size;
};
```

---

## 五、上下文保存/恢复

### task_save — 保存时判断来源，写入 choose

```
task_save(frame):
  1. self = read_gs(NOW_RUNNING_TASK)
  2. self->priv_ctx = *frame                    // 内核上下文永远保存
  3. 更新时间账本

  4. 判断中断来源并写入 choose:
     if fred.errcode[63] == 1:                  // 来自 Guest
       save VMCS guest state → vCPU ctx
       save guest XSAVE → vCPU xsave_area
       self->choose = vCPU

     elif frame->core_ctx.idtctx.iret.cs.RPL==3: // 来自用户态
       memcpy(&self->uctx->xtd_ctx, frame, sizeof(*frame))
       XSAVEC(self->uctx->xsave_area)
       self->choose = u_ctx

     else:                                       // 来自内核态
       self->choose = priv

  5. return self
```

### resume — 恢复时只看 choose

```
resume():
  switch choose:
    priv:   iretq (弹出 priv_ctx.core_ctx)
    u_ctx:  XRSTOR(uctx->xsave_area)
            搬 uctx->xtd_ctx iret 帧到栈顶 → iretq
    vCPU:   XRSTOR(vcpu_xsave_area)
            恢复 VMCS → VMRESUME
```

### launch — 首次启动

```
launch():
  1. 设 choose = priv（内核线程首次跑）
  2. 恢复 priv_ctx 中的 GPRs
  3. iretq → 内核入口函数

内核入口函数:
  // 执行 body
  //    ↓ 如果目标是:
  //    内核线程: 直接做内核工作，yield/exit
  //    用户线程: alloc uctx → 填充 xtd_ctx → iretq
  //    vCPU:     alloc VMCS → 填充 Guest 状态 → VMRESUME
```

---

## 六、三种线程形态对比

```
               创建              执行体             被中断时保存
               ────             ──────            ──────────
普通内核线程:  basic_constructor  priv_ctx 内核代码  只 save priv_ctx
               launch()          → yield/exit       choose=priv

用户线程:     basic_constructor  priv_ctx 入口函数   save priv_ctx
               launch()          → 初始化 uctx      + uctx + FPU
                                 → iretq → 用户态   choose=u_ctx
                                 → 中断回来 → 处理
                                 → 再次 iretq ...

vCPU:         basic_constructor  priv_ctx 入口函数   save priv_ctx
               launch()          → 分配 VMCS        + Guest 上下文
                                 → VMRESUME→Guest   choose=vCPU
                                 → VM-exit→处理
                                 → 再次 VMRESUME...
```

**三者在调度器眼里毫无区别。** 调度器只管 `priv_ctx` 的保存/恢复，`uctx` 和 `choose` 对它是透明的。

---

## 七、FPU 管理

```
基本策略:
  基础 FPU (x87+SSE+AVX) inline 在 u_ctx_t 页内，压缩格式 XSAVEC
  超预算（AMX）走独立分配，xtx->xsave_area 挂指针

切换点:
  用户态→内核 (CS.RPL==3 detected in task_save):
    XSAVEC(uctx->xsave_area)     ← 存用户 FPU
  内核→用户态 (resume choose=u_ctx):
    XRSTOR(uctx->xsave_area)     ← 恢复用户 FPU
  Guest→VMM (errcode[63] detected):
    XSAVEC(vcpu_xsave)           ← 存 Guest FPU
  VMM→Guest (resume choose=vCPU):
    XRSTOR(vcpu_xsave)           ← 恢复 Guest FPU
  kernel↔kernel:
    不需要 FPU 切换
```

初始化：

```asm
CPUID.0DH.0 → 枚举支持组件
CPUID.0DH.1 → 压缩格式大小
CR4.OSXSAVE = 1
XSETBV(XCR0, 0x07)

; 新 task 的 uctx 创建时
memset(uctx->xsave_area, 0, fpu_area_size)
```

---

## 八、FRED 兼容

`x64_standard_context_v2::core_ctx` union：

```cpp
union {
    fred_complex  fred;     // 64B — FRED 事件递送帧
    idt_core_ctx  idtctx;   // 64B — IDT 事件递送帧
} core_ctx;
```

VM-exit 标记：`fred_complex::errcode[63]`

```
// fred_complex 第一个 u64:
//   bit[15:0]  → errcode (硬件填写)
//   bit[62:16] → reserved (硬件填 0)
//   bit[63]    → VM-exit 标记 (自用)
// ERETU/ERETS/IRETQ 都跳过此 u64 (RSP+=8)
```

`fred_complex + 8` 的 40B 与 `iret_complex_context` 布局一致，IDT 模式下可直接 iretq。

---

## 九、时间账本

```
update_time_accounting(task* self):
  now = now()
  delta = now - self->latest_record.base

  switch self->latest_record.type:
    run:        accumulated_running      += delta
    sleep:      accumulated_sleeping     += delta
    wait_io:    accumulated_io_blocking  += delta
    wait_other: accumulated_waiting_other+= delta

  self->latest_record.base = now
  self->latest_record.span_length = delta
```

---

## 十、未定事项

1. **vCPU 上下文数据结构** — VMCS/VMCB + xsave_area 布局
2. **sched_log 环形缓冲区** — per-task 追踪，可选消费者强制
3. **AMX 惰性分配** — 通过 #NM + XFD 做按需预算
4. **drs[8] 保存/恢复** — 当前预留字段，存取逻辑待实现
5. **priv_stack 动态调整** — uctx 初始化时可能需要缩栈
6. **vCPU ctx 指针的位置** — 作为 task 的一个字段还是通过全局映射
