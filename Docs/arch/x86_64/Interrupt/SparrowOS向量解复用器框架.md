# SparrowOS 向量解复用器框架 (Vector Demux)

## 概述

x86 向量号 256 个，IDT 把异常、软中断、硬件中断、IPI 四类异质机制押在同一个线性空间。
FRED 虽然用 SS.type 分离了软中断(type=4)和硬件中断(type=0/2)，但硬件中断内部仍杂糅 IPI。
本框架的核心是**向量解复用**——给定一个向量号，按优先级查三张分发表，路由到正确的 handler。

---

## 1. 相对于旧版 (232e3a3c) 的架构演进

| 维度 | 旧版 (commit 232e3a3c) | 当前 |
|------|----------------------|------|
| 类名 | `idt_vec_dispatch_mgr` | `vec_demux` |
| 分发表 | 两路: soft → tokens | **三路**: soft → ipi_descrioptors → tokens |
| IPI 机制 | `soft_int[240]` → `global_ipi_handler` (单函数指针) | 独立 `ipi_descrioptors[256]` 全局表, 固定向量号, 带 `is_no_return` 标志 |
| 汇编入口 | 224 个独立 `VEC_DELIVERY_ENTRY` (~85B/个, 需符号表扫描) | **16B trampoline 表** + 公共主体, Init 纯算术寻址 |
| Init 阶段 | `exceptions_init` (boot) + `idt_vec_dispatch_mgr::Init` (MM_READY) | `vec_demux::early_init` (boot, 全 0–255 一次写入) + `vec_demux::late_init` (MM_READY, 表初始化) |
| 异常入口位置 | `fixed_interrupt_resources.cpp` | 并入 `vec_demux::early_init` |
| 命名空间 | `ivec` 单命名空间混合异常/软中断/IPI | `x86_exceptions` / `x86_softinterrupt_abi` / `runaway_ipi_vec` / `return_ipi_vec` 四语义 |
| x64_standard_context | 直接传 frame | **x64_vec_demux_frame** (GPR + vec + IRETQ, 因 trampoline 在 GPR 与 iretq 间插入了 vec) |
| FRED | 空壳 `fred_user_cpp_enter` | `fred_common_enter` + `fred_exceptions_router` 完整 skeleton |
| alloc_vec 跳过 | 仅跳过 soft_interrupt_functions | 跳过 soft + IPI 固定槽 (250–254) |
| KURD event namespace | `idt_mgr_events` | `vec_demux_events` |

---

## 2. 四类中断 — 分门别类

| 类别 | 触发方式 | 数据结构 | 作用域 | 可否跑飞 | 向量号分配 |
|------|---------|---------|--------|---------|-----------|
| CPU 异常 | 硬件条件 | 专用 C++ 入口 | 架构固定 | 否 | 0–31 固定 |
| 软中断 | `int N` (同步指令) | `soft_interrupt_functions[256]` | 全局 | 是 (UD2 sentinel) | 固定 (225–227, 255) |
| 系统 IPI | LAPIC ICR (异步) | `ipi_descrioptors[256]` | 全局 | 看 `is_no_return` | 固定 (250–254) |
| 硬件中断 | 外部/MSI (异步) | `gs_complex_t::tokens[256]` per-CPU | per-CPU | 否 (必须返回) | alloc_vec 动态 |

---

## 3. 向量号布局

```
 0–31    x86_exceptions               CPU 异常（架构固定，专用入口）
32–224   [free pool]                   alloc_vec 自由分配区
225      x86_softinterrupt_abi::ASM_PANIC      软中断 — 内核恐慌
226      x86_softinterrupt_abi::KTHREAD_CALL   软中断 — 跨核函数调用
227      x86_softinterrupt_abi::USER_ABI_ENTER 软中断 — 用户态系统调用
228–249  [free pool]                   中间空档
250      return_ipi_vec::LOCAL_TLB      系统 IPI — 本地 TLB flush（返回型）
251      return_ipi_vec::GLOBAL_TLB     系统 IPI — 全局 TLB flush（返回型）
252      runaway_ipi_vec::IPI_HALT      系统 IPI — AP 停机（不返回）
253      runaway_ipi_vec::RESCHEDDUE    系统 IPI — 远程重调度（不返回）
254      runaway_ipi_vec::START_SCHED   系统 IPI — AP 上线/启动调度（不返回）
255      x86_softinterrupt_abi::SUPRIOUS 软中断 — 虚假中断检测
```

约束：**三组向量集两两不可相交**（alloc_vec 跳过 soft + IPI 固定槽）。

---

## 4. 核心数据结构

`src/include/arch/x86_64/Interrupt_system/Interrupt.h`

```cpp
typedef void (*soft_interrupt_func_t)(x64_standard_context* context);

struct ipi_descrioptor_t {
    soft_interrupt_func_t func;       // null = 未注册
    bool is_no_return;                // true → UD2 sentinel
};

struct interrupt_token_t {
    uint64_t flags;                   // bit 0: TOKEN_FLAG_MASK_TOKEN_SCHEDULE
    uint64_t token_private;           // O(1) 触发源编码
    uint64_t (*func)(interrupt_token_t* token);  // 必须返回
};
```

三张分发表：
```cpp
soft_interrupt_func_t soft_interrupt_functions[256];  // 全局，软中断
ipi_descrioptor_t     ipi_descrioptors[256];           // 全局，系统 IPI
interrupt_token_t     tokens[256];                     // per-CPU, 在 gs_complex_t 中
```

---

## 5. IDT 路径

### 5.1 汇编 Trampoline

**文件**: `Sysdef_exception_entries.asm`

旧方案 (已废弃): 224 个完整入口, 每个 ~85B, 需符号表扫描 Init。
当前方案: **16B trampoline + 公共主体**, 算术寻址 Init。

```
vec_demux_table (224 × 16B = 3.5KB):

  [vec 32]  push dword 32      ; 5B — 向量号入栈（不破坏 GPR 上下文）
            jmp short common    ; 2B
            times 9 nop         ; pad to 16
  [vec 33]  push dword 33
            jmp short common
            times 9 nop
  ...
  [vec 255] push dword 255
            jmp short common
            times 9 nop

vec_demux_common:
            push rbp..rax       ; 保存 15 GPR (120B)
            ; 栈: [GPR] [vec] [iretq]
            call vec_demux_entry
            pop rbp..rax
            add rsp, 8          ; 弹出 vec
            iretq
```

**关键决策**: `push dword N` 而非 `mov esi, N`, 保证被中断上下文完整保存。

### 5.2 vec_demux_frame

Trampoline 在 GPR 与 iretq 之间插入了 vec, 导致栈布局与 `x64_standard_context` 不一致。
使用专用结构体接收:

```cpp
struct x64_vec_demux_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rbp;
    uint64_t vec;
    iret_complex_context iret;
};
```

`vec_demux_entry` 内部用 `frame_to_standard` → `standard_to_frame` 进行转换:
```cpp
extern "C" void vec_demux_entry(x64_vec_demux_frame* raw_frame) {
    x64_standard_context ctx;
    frame_to_standard(&ctx, raw_frame);   // 修正 vec 偏移
    // ... 调 handler ...
    standard_to_frame(raw_frame, &ctx);   // 写回
}
```

### 5.3 IDT 分发逻辑

```
vec_demux_entry(frame):
  vec = frame->vec

  → 1. soft_interrupt_functions[vec] → 跑 + UD2
  → 2. ipi_descrioptors[vec] → 跑 + EOI, is_no_return? UD2 : return
  → 3. tokens[vec]          → 跑 + EOI, res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE? resched
  → 4. 未匹配 → 虚假中断日志
```

---

## 6. FRED 路径

### 6.1 FRED 初始化 (规划中, 未来 CR4.FRED=1 时启用)

参照 SDM Vol 3 §8.2.3–8.3:
1. 配置 `IA32_FRED_CONFIG` (handler 页基址, 栈级别, maskable int 栈)
2. 配置 `IA32_FRED_RSP0~3` ← 复用 `per_processor_hardware_stack_t` (见 §8)
3. 配置 `IA32_FRED_STKLVLS` (向量 0–31 per-vector 最低栈级别)
4. 建立 handler 页: 偏移 0x000 (CPL→3 入口) + 0x100 (CPL→0 入口)
5. `CR4.FRED = 1`

### 6.2 FRED 分发骨架

```
fred_common_enter(fred_ctx):
  switch (fred_ctx->type):
    0 (extint) → filt_frame → vec_demux_hw_dispatch(frame, vec)
                 先查 ipi_descrioptors (低频), 再查 tokens (高频设备)
    2 (NMI)    → filt_frame → nmi_cpp_enter
    3 (exception) → fred_exceptions_router(fred_ctx)
                    → 内部 filt_frame + 按 vec 分发到各 handler
    4 (soft)   → filt_frame → vec_demux_soft_dispatch(frame, vec)
                 直接查 soft_interrupt_functions
    7 (syscall) → TODO: 用户态入口

fred_exceptions_router(fred_ctx):
  filt_frame(&frame, fred_ctx)
  vec = fred_ctx->fred.vec
  event_data = fred_ctx->fred.event_data   // SDM §8.3.2:
    #PF  → event_data = faulting linear address
    #DB  → event_data = debug nature
    #NM  → event_data = IA32_XFD_ERR
    其他 → event_data = 0 (FRED 不推单独错误码)
  // 对于 #PF, 错误码从 fred_ctx->fred.errcode 获取
  switch(vec): ...对应 handler
```

### 6.3 FRED 下的 vec_demux 表共用

FRED 和 IDT 共用同一套三张分发表:
- `soft_interrupt_functions` — 路由不同, 表相同
- `ipi_descrioptors` — 路由不同, 表相同
- `tokens` — 路由不同, 表相同

区别在入口路径:
- IDT: `trampoline → vec_demux_entry → 三表`
- FRED: `handler page → fred_common_enter → 按 type 选三表`

---

## 7. 初始化时序 (两阶段)

### 阶段 1: early_init (boot asm, 无 heap)

```
_kernel_Init:
  → vec_demux_early_init() [extern "C" wrapper]
      → vec_demux::early_init()
          1. 填 template_idt[0..31] — 异常入口 (原 exceptions_idt_init)
          2. 算术计算 template_idt[32..255]:
             template_idt[v].handler = &vec_demux_table + (v-32)*16
          3. USER_ABI_ENTER.dpl = 3
          4. template_idt_apply_region(0, 255)  → 一次写入 global_idt
  → lidt [global_idtr]     ← IDT 256 项全部就绪
```

### 阶段 2: late_init (MM_READY 后)

```
kernel_start():
  → mem_init()
  → vec_demux::late_init()
      1. 清零 per-CPU tokens 表 (遍历 conjucnt_GSs)
      2. 初始化 soft_interrupt_functions:
         [225] = asm_panic_cpp_enter
         [226] = kthread_call_cpp_enter
         [227] = user_abi_cpp_enter
         [255] = suprious_interrupt_cpp_enter
      3. 初始化 ipi_descrioptors (占位, func=nullptr):
         [254] START_SCHED  is_no_return=true
         [253] RESCHEDDUE   is_no_return=true
         [252] IPI_HALT     is_no_return=true
         [251] GLOBAL_TLB   is_no_return=false
         [250] LOCAL_TLB    is_no_return=false
```

---

## 8. 与 per-CPU 栈基础设施的衔接 (FRED 相关)

`gs_complex_t` + `per_processor_hardware_stack_t` 为 FRED 栈级别提供现成支持:

```
per_processor_hardware_stack_t  →  FRED MSR 映射
──────────────────────────────────────────────────
stack_rsp0  (32 KB)  →  IA32_FRED_RSP0  (sl0, 通用 CPL=0 栈)
stack_ist1  (8 KB)   →  IA32_FRED_RSP1  (sl1, 原 DF 栈)
stack_ist2  (12 KB)  →  IA32_FRED_RSP2  (sl2, 原 MC 栈)
stack_ist3  (12 KB)  →  IA32_FRED_RSP3  (sl3, 原 NMI 栈)
stack_ist4  (12 KB)  →  maskable int 栈 (IA32_FRED_CONFIG[10:9])
```

关键: `gs_complex_t` 已经 per-CPU 分配, `stacks_ptr` 指向物理连续的栈区。
FRED MSR 也是 per-CPU (thread-scoped), 各 CPU 填入自己的栈顶地址即可。

---

## 9. 管理接口

```cpp
// 统一外部接口 (IDT/FRED 共用, 实现在 vec_demux 方法之上)
uint8_t out_interrupt_vec_alloc(interrupt_token_t*, uint32_t pid, KURD_t*);
uint8_t out_interrupt_vec_alloc_by_apicid(interrupt_token_t*, uint32_t apicid, KURD_t*);
KURD_t  out_interrupt_vec_free(uint8_t vec, uint32_t pid);
interrupt_token_t* out_interrupt_vec_get(uint8_t vec, uint32_t pid, KURD_t*);
```

alloc_vec 扫描跳过条件:
```cpp
if (soft_interrupt_functions[vec])          continue;  // 软中断固定槽
if (vec >= LOCAL_TLB && vec <= START_SCHED) continue;  // IPI 固定槽 250–254
if (slice[vec].func == nullptr)             break;     // 空槽分配
```

---

## 10. 文件清单

| 文件 | 相对于旧版 (232e3a3c) 的变化 |
|------|---------------------------|
| `fixed_interrupt_vectors.h` | `ivec` → 四命名空间; 移除 `IPI=240`, `LAPIC_ERR=241`; 新增 `USER_ABI_ENTER=227`, `runaway_ipi_vec`, `return_ipi_vec` |
| `Interrupt.h` | 新增 `ipi_descrioptor_t` 定义 |
| `x86_vecs_deliver_mgr.h` | 类 `idt_vec_dispatch_mgr` → `vec_demux`; `Init` → `early_init`+`late_init`; KURD `idt_mgr_events` → `vec_demux_events`; 新增 `x64_vec_demux_frame` |
| `x86_vecs_deliver_mgr.cpp` | `all_vec_delivery` → `vec_demux_entry`; 符号表扫描 → 算术寻址; `exceptions_idt_init` 并入 `early_init`; 新增 `vec_demux_hw/soft_dispatch`; 新增 IPI 向量跳过 |
| `fixed_interrupt_resources.cpp` | 移除 `exceptions_init`/`exceptions_idt_init` |
| `Sysdef_exception_entries.asm` | 移除 224 `VEC_DELIVERY_ENTRY` + FRED 入口; 新增 trampoline 表 + vec_demux_common |
| `exceptions_handler.cpp` | FRED 骨架: `fred_common_enter` + `fred_exceptions_router`; `fred_user/supervisor_cpp_enter` 现调 `fred_common_enter` |
| `kernel_entry.asm` | `exceptions_init` → `vec_demux_early_init` |
| `kinit.cpp` | `idt_vec_dispatch_mgr::Init()` → `vec_demux::late_init()` |
| `GS_complex.h` | 无变化 (但 `per_processor_hardware_stack_t` 是 FRED 栈支持的关键) |
