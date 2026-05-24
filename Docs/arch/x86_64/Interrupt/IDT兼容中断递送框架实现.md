# IDT 兼容中断递送框架实现文档

## 概述

本框架受 FRED 中断机制启发，基于传统 IDT 实现了向量号递送能力：256 个 IDT 入口
各自嵌入不同的向量号，高级语言入口统一接收 `(context, vec)`，无需额外查表确定中断来源。

> 设计文档见 [IDT兼容中断递送框架设计.md](./IDT兼容中断递送框架设计.md)。
> 本文档描述代码仓库中的实际落地实现。

---

## 1. 纳入范围

| 范围 | 排除 |
|------|------|
| 向量 32～255 的软中断 (int n) | 向量 0～31 的 CPU 异常 (各自有专用 C++ 入口) |
| 向量 32～255 的硬件中断 (外部/消息) | FRED 模式 (下行接口已预留，暂未启用) |

向量分配布局：

| 范围 | 用途 |
|------|------|
| 0～31 | CPU 异常 (专用 bare_enter → cpp_enter) |
| 32～224 | 自由分配 (硬件中断 token 表) |
| 225 (ASM_PANIC) | 软中断 — 内核恐慌 |
| 226 (KTHREAD_CALL) | 软中断 — 跨核函数调用 |
| 240 (IPI) | 软中断 — 跑飞型核间中断 (AP bringup up/down) |
| 241～254 | 自由分配 |
| 255 (SUPRIOUS) | 软中断 — 虚假中断检测 |

---

## 2. 汇编入口层

### 宏定义位置
`src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm`

### VEC_DELIVERY_ENTRY

```nasm
%macro VEC_DELIVERY_ENTRY 1
    ; %1 = 向量号

    push rbp                   ; 保存 15 个 GPR
    push r15 ... push rax

    ; swapgs if from user mode
    mov rax, [rsp + GP_GPR_BYTES]   ; CS at [RSP+128] (15 pushes × 8 + RIP8)
    test al, 3
    jz %%skip_gs
    swapgs
%%skip_gs:

    mov rdi, rsp               ; rdi = x64_standard_context*
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    push rax                   ; 保存对齐前 RSP
    mov rsi, %1                ; rsi = 向量号
    mov rax, all_vec_delivery
    call rax

    pop rsp                    ; 恢复对齐前 RSP

    ; reverse swapgs if needed (rax = C 返回值，非保存值)
    mov rax, [rsp + GP_GPR_BYTES]   ; CS
    test al, 3
    jz %%skip_gs_back
    swapgs
%%skip_gs_back:

    pop rax ... pop rbp        ; 恢复 15 个 GPR
    iretq
%endmacro
```

### 批量生成

```nasm
%assign __vec 32
%rep 224
global vec_delivery_%+__vec%+_bare_enter
vec_delivery_%+__vec%+_bare_enter:
    VEC_DELIVERY_ENTRY __vec
%assign __vec __vec + 1
%endrep
```

每个入口对应唯一全局符号 `vec_delivery_N_bare_enter`，供 IDT 引用。

### 栈切换

当前使用 IST=0（不切换栈）。所有递送入口直接在当前栈上执行。

---

## 3. 高级语言入口

### 文件位置
`src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp`

### 两路分发逻辑：all_vec_delivery

```cpp
extern "C" void all_vec_delivery(x64_standard_context *frame, uint8_t vec)
{
    if (vec < 32) {
        panic("BAD_VEC_RECIEVED");
        return;
    }

    uint32_t pid = fast_get_processor_id();

    /* ── 1. 软中断表 (全局, 同步, 带 frame) ── */
    if (soft_interrupt_functions[vec]) {
        soft_interrupt_functions[vec](frame);
        return;   // 软中断允许跑飞, 此处 return 仅在 handler 返回时可达
    }

    /* ── 2. 硬件中断表 (per-CPU, token 驱动) ── */
    gs_complex_t* self = (gs_complex_t*)rdmsr(IA32_GS_BASE);
    if (self->tokens[vec].func) {
        self->tokens[vec].func(&self->tokens[vec]);
        x2apic::x2apic_driver::write_eoi();
        if (self->tokens[vec].flags & TOKEN_FLAG_NEED_RESCHED) {
            // TODO: schedule logic
        }
        return;
    }

    /* ── 3. 未匹配 → 虚假中断 ── */
    bsp_kout << "[WARNING] no handler for vec " << (uint32_t)vec
             << " on processor " << pid << kendl;
}
```

优先级：软中断表 > 硬件中断表。软中断是同步指令流跳转，带 frame 可跑飞；
硬件中断是异步的，handler 无 frame、禁止跑飞，框架统一做 EOI + 调度决策。

---

## 4. 核心数据结构

### interrupt_token_t

**`src/include/arch/x86_64/Interrupt_system/Interrupt.h`**

```cpp
typedef void (*soft_interrupt_func_t)(x64_standard_context* context);

struct interrupt_token_t {
    uint64_t flags;           // bit 0: TOKEN_FLAG_NEED_RESCHED
    uint64_t token_private;   // O(1) 触发源定位，由注册者编码
    void   (*func)(interrupt_token_t* token);
};
```

handler 只接收 token 指针，不接收 frame，以此阻断在硬件中断上下文跑飞的路径。
`token_private` 由注册者填写，例如 NVMe 可编码 `(ctrl_ptr << 16) | queue_idx`，
handler 内 O(1) 定位触发源。

### 软中断表

```cpp
soft_interrupt_func_t soft_interrupt_functions[256];

// 已注册的内置软中断:
//   [ivec::ASM_PANIC]      = asm_panic_cpp_enter
//   [ivec::KTHREAD_CALL]   = kthread_call_cpp_enter
//   [ivec::IPI]            = ipi_cpp_enter
//   [ivec::SUPRIOUS_INTERRUPT] = suprious_interrupt_cpp_enter
```

### 硬件中断 Token 表

per-CPU 存储于 `gs_complex_t::tokens[256]`，`rdmsr(IA32_GS_BASE)` 获得基址后
直接索引。

```cpp
struct gs_complex_t {
    uint64_t slots[256];             // [0x0000, 0x0800)
    interrupt_token_t tokens[256];   // [0x0800, 0x1800)
    // ... GDT/TSS/FPU/stacks ...
};
```

### logical_idt → IDTEntry 翻译

```cpp
void template_idt_apply_region(uint8_t from_vec, uint8_t to_vec);
// type = 0xE (64-bit 中断门), ist_index=0, dpl 透传
```

---

## 5. 初始化流程

### 异常入口 (`exceptions_idt_init`)
BSP 调用 `exceptions_init()` → `template_idt_apply_region(0, 31)`，
填充向量 0～31 异常入口。

### 向量递送入口 (`idt_vec_dispatch_mgr::Init`)

在 MM_READY 段由 BSP 调用：

1. **清零 tokens 表**：
   遍历所有 CPU 的 gs_complex_t，`ksetmem_8(cx->tokens, 0, sizeof(cx->tokens))`

2. **符号表扫描**：
   查找 `vec_delivery_32_bare_enter` → 递增尝试 33, 34… 直到断裂 →
   回退全表扫描补全

3. **填入 template_idt**：
   ```
   handler   = symbol.address
   type      = 0xE (interrupt gate)
   ist_index = 0
   dpl       = 0
   ```

4. **写入 global_idt**：
   `template_idt_apply_region(32, 255)`

5. 未找到全部 224 个符号 → `Panic::panic()`（`NOT_ALL_IDT_FOUND`）

---

## 6. 管理接口

### `idt_vec_dispatch_mgr`

| 函数 | 行为 |
|------|------|
| `Init()` | 符号表扫描 + template_idt 填充 + global_idt 写入 |
| `alloc_vec(token*, pid, &kurd)` | 在指定处理器的 tokens 表中分配空闲向量 |
| `alloc_vec_by_apicid(token*, apicid, &kurd)` | 同 alloc_vec 但通过 x2APIC ID → g_gs_by_apicid 表 O(1) 查 gs_complex_t |
| `free_vec(vec, pid)` | 释放指定处理器上的向量槽 |
| `get_vec(vec, pid, &kurd)` | 查询指定处理器上向量的 token 指针 |

**alloc_vec 验证链**:
1. token->func 非空 → `BAD_FUNC_PTR`
2. `is_addr_kernel_address(token->func)` → `BAD_FUNC_PTR`
3. ksymmanager 精确地址匹配 → `SYM_NOT_FOUND`
4. processor_id / apicid 合法性 → `INVALID_PROCESSOR_ID`
5. 扫描 32～255 跳过 soft_interrupt_functions 占位 → `NO_FREE_VEC`
6. `dispatch_lock` (spinlock_interrupt_about_guard) 保护写入

### extern "C" 统一接口

提供给其他模块的统一入口，IDT/FRED 切换时只改这里：

```cpp
uint8_t out_interrupt_vec_alloc(interrupt_token_t* token, uint32_t pid, KURD_t*);
uint8_t out_interrupt_vec_alloc_by_apicid(interrupt_token_t*, uint32_t apicid, KURD_t*);
KURD_t  out_interrupt_vec_free(uint8_t vec, uint32_t pid);
interrupt_token_t* out_interrupt_vec_get(uint8_t vec, uint32_t pid, KURD_t*);
```

---

## 7. KURD 纪律

```cpp
// 类级模板
static KURD_t idt_mgr_default_kurd()    // module=INTERRUPT, modloc=modloc_idt_mgr, domain=CORE
static KURD_t idt_mgr_default_success()
static KURD_t idt_mgr_default_error()
static KURD_t idt_mgr_default_fatal()

// per 方法模式:
KURD_t success_k = idt_mgr_default_success();
KURD_t fail_k    = idt_mgr_default_error();
success_k.event_code = <事件>;
fail_k.event_code    = <事件>;
kurd = fail_k;
kurd.reason = <原因码>;
```

---

## 8. 已注册的中断使用场景

### LAPIC Timer
```cpp
interrupt_token_t token = { TOKEN_FLAG_NEED_RESCHED, 0, timer_cpp_enter };
vec = out_interrupt_vec_alloc(&token, fast_get_processor_id(), &kurd);
```
flags 带 NEED_RESCHED，框架在 EOI 后触发调度。

### LAPIC Error
```cpp
interrupt_token_t token = { 0, 0, lapic_error_handler::handler };
vec = out_interrupt_vec_alloc(&token, fast_get_processor_id(), &kurd);
```
flags=0，仅读 ESR 打印日志，不触发调度。

### i8042 键盘
```cpp
interrupt_token_t token = { 0, 0, i8042_cpp_enter };
vec = out_interrupt_vec_alloc(&token, fast_get_processor_id(), &kurd);
```

### NVMe CQ
```cpp
interrupt_token_t token = { 0, token_private, ADMIN_CQ_handler };
vec = out_interrupt_vec_alloc(&token, processor_id, &kurd);
```
token_private 编码 controller + queue 信息。

### 内置软中断
| 向量 | 用途 |
|------|------|
| 225 (ASM_PANIC) | `int ASM_PANIC` 触发内核恐慌 |
| 226 (KTHREAD_CALL) | 跨核函数调用 (yield/exit/wait/sleep/block) |
| 240 (IPI) | 跑飞型核间中断 (AP bringup up/down) |
| 255 (SUPRIOUS) | 虚假中断检测记录 |

---

## 9. 文件清单

| 文件 | 内容 |
|------|------|
| `Sysdef_exception_entries.asm` | VEC_DELIVERY_ENTRY 宏 + 224 个入口 + swapgs |
| `x86_vecs_deliver_mgr.h` | 类声明 + KURD 事件代码 + extern "C" 统一接口 |
| `x86_vecs_deliver_mgr.cpp` | Init / alloc_vec / free_vec / get_vec / alloc_vec_by_apicid + all_vec_delivery |
| `fixed_interrupt_resources.cpp` | logical_idt/global_idt/soft_interrupt_functions + template_idt_apply_region |
| `loacl_processor.h` | TSS / IDTEntry / logical_idt / GS slot 常量 |
| `Interrupt.h` | interrupt_token_t / soft_interrupt_func_t 类型定义 |
| `GS_complex.h` | gs_complex_t (含 tokens[256]) + per_processor_hardware_stack_t |
| `fixed_interrupt_vectors.h` | ivec 命名空间 (向量编号常量) |
| `lapic.cpp` | LAPIC timer + error handler 注册示例 |
| `local_processor_manage.cpp` | per-CPU GS slot 初始化 (tokens 表指针) |
