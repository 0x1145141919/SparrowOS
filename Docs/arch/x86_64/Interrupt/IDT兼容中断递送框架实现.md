# IDT 兼容中断递送框架实现文档

## 概述

本框架受 FRED 中断机制启发，基于传统 IDT 实现了向量号递送能力：256 个 IDT 入口各自嵌入不同的向量号，高级语言入口统一接收 `(context, vec)`，无需额外查表确定中断来源。

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
| 32～255| 自由分配 (硬件中)  但是全局软中断占位: , 225=ASM_PANIC, 226=KTHREAD_CALL, 240=IPI, 255=SUPRIOUS |

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

所有 224 个递送入口统一使用 **IST=5**（TSS.IST[5]）。当 CPU 从任意 CPL 递送中断时，自动切换到 IST5 专用栈，不污染当前内核栈。

---

## 3. 高级语言入口

### 文件位置
`src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp`

### 分发逻辑：`all_vec_delivery`

```cpp
extern "C" void all_vec_delivery(x64_standard_context *frame, uint8_t vec) {
    if (vec < 32)
        panic("BAD_VEC_RECIEVED");     // 不应到达此处

    uint32_t pid = fast_get_processor_id();

    // 1. 软中断表 (全局、同步)
    if (soft_interrupt_functions[vec]) {
        soft_interrupt_functions[vec](frame, vec);
        return;
    }

    // 2. 硬件中断表 (per-CPU)
    hard_interrupt_func_t *table =
        (hard_interrupt_func_t *)read_gs_u64(PROCESSOR_INTERRUPT_FUNCS_TABLE_GS_INDEX);
    if (table && table[vec]) {
        table[vec](frame, vec, pid);
        return;
    }

    // 3. 谁都没匹配 → 虚假中断警告
    bsp_kout << "[WARNING] no handler for vec " << vec
             << " on processor " << pid << kendl;
}
```

优先级：软中断表 > 硬件中断表。软中断是同步指令流跳转，不关心 per-CPU 信息；硬件中断表是 per-CPU 的，可精确描述中断事件。

---

## 4. 数据结构

### 函数指针类型

**`src/include/arch/x86_64/Interrupt_system/Interrupt.h`**

```cpp
typedef void (*soft_interrupt_func_t)(x64_standard_context* context, uint8_t vec);
typedef void (*hard_interrupt_func_t)(x64_standard_context* context, uint8_t vec, uint32_t processor_id);
```

### 软中断表

```cpp
// fixed_interrupt_resources.cpp — 编译时初始化（const）
soft_interrupt_func_t soft_interrupt_functions[256];

// 已注册的内置软中断:
//   [ivec::ASM_PANIC]      = asm_panic_cpp_enter
//   [ivec::IPI]            = ipi_cpp_enter
//   [ivec::KTHREAD_CALL]   = kthread_call_cpp_enter
//   [ivec::SUPRIOUS_INTERRUPT] = suprious_interrupt_cpp_enter
```

### 硬件中断表

```cpp
// 运行时分配的 flat 数组: [logical_processor_count × 256]
// 每个处理器 GS slot[4] 指向其 256 项切片
hard_interrupt_func_t *all_processors_interrupt_functions;

// local_processor_manage.cpp — 每个处理器初始化时:
gs_slot[PROCESSOR_INTERRUPT_FUNCS_TABLE_GS_INDEX] =
    (uint64_t)&all_processors_interrupt_functions[256 * processor_id];
```

### logical_idt → IDTEntry 翻译

```cpp
// fixed_interrupt_resources.cpp
void template_idt_apply_region(uint8_t from_vec, uint8_t to_vec) {
    for each v in [from_vec, to_vec]:
        global_idt[v] = translate(template_idt[v]);
        // 段选择子 = 0x08, type = 0xE (64-bit 中断门)
        // present = 1, ist_index / dpl 透传
}
```

---

## 5. 初始化流程

### 异常入口 (`exceptions_idt_init`)
在 `fixed_interrupt_resources.cpp` 中，BSP 调用 `exceptions_init()` → `x86_smp_processors_container::exceptions_idt_init()`。填充向量 0～31 的 `template_idt`，调用 `template_idt_apply_region(0, 31)` 写入 `global_idt`。

### 向量递送入口 (`idt_vec_dispatch_mgr::Init`)

在 MM_READY 段由 BSP 调用：

1. **分配硬件中断表**：
   `__wrapped_pgs_valloc(bytes)` → `ksetmem_8` 清零

2. **符号表扫描**：
   `ksymmanager` 中查找 `vec_delivery_32_bare_enter` → 递增尝试 33, 34… 直到断裂 → 回退全表扫描补全

3. **填入 template_idt**：
   ```
   handler   = symbol.address
   type      = 0xE
   ist_index = 5
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
| `Init(logical_processor_count)` | 符号表扫描 + template_idt 填充 + global_idt 写入 |
| `alloc_vec(func, pid, &kurd)` | 在指定处理器的硬件表中分配空闲向量 (32～255)，跳过被软中断占用的向量 |
| `free_vec(vec, pid)` | 释放指定处理器上的向量槽 |
| `get_vec(vec, pid, &kurd)` | 查询指定处理器上向量的处理函数 |

**alloc_vec 验证链**:
1. `is_addr_kernel_address(func)` → `BAD_FUNC_PTR`
2. `processor_id >= logical_processor_count` → `INVALID_PROCESSOR_ID`
3. 跳过 `soft_interrupt_functions[vec]` 非空的槽位
4. 扫 32～255 找空槽 → 无空位 `NO_FREE_VEC`
5. 加 `dispatch_lock` 原子写入

### extern "C" 统一接口

提供给其他模块（如 LAPIC 驱动）的统一入口，IDT/FRED 切换时只改这里：

```cpp
extern "C" uint8_t out_interrupt_vec_alloc(func, pid, kurd_ptr);
extern "C" KURD_t   out_interrupt_vec_free(vec, pid);
extern "C" auto     out_interrupt_vec_get(vec, pid, kurd_ptr);
```

当前无条件走 IDT 路径 → `idt_vec_dispatch_mgr::*`。

---

## 7. KURD 纪律

```cpp
// 类级模板
static KURD_t idt_mgr_default_kurd()    // module=INTERRUPT, modloc=modloc_idt_mgr, domain=CORE
static KURD_t idt_mgr_default_success() // result=SUCCESS, level=INFO
static KURD_t idt_mgr_default_error()   // result=FAIL, level=ERROR
static KURD_t idt_mgr_default_fatal()   // result=FATAL, level=FATAL

// per 方法模式:
KURD_t success_k = idt_mgr_default_success();
KURD_t fail_k    = idt_mgr_default_error();
success_k.event_code = <事件>;
fail_k.event_code    = <事件>;

using namespace <结果代码命名空间>;

kurd = fail_k;
kurd.reason = <原因码>;
```

---

## 8. 已注册的中断使用场景

### LAPIC Timer
```cpp
// lapic.cpp
x2apic::lapic_timer_one_shot::processor_regist()
x2apic::lapic_timer_tsc_ddline::processor_regist()
```
使用 `out_interrupt_vec_alloc` 在本地处理器上分配向量，配置 LVT 计时器。

### LAPIC Error
```cpp
// lapic.cpp
x2apic::lapic_error_handler::handler()      // 读 ESR → bsp_kout 打印错误标志
x2apic::lapic_error_handler::processor_regist() // 分配 vec + 配置 LVT Error
```

### 内置软中断
| 向量 | 用途 |
|------|------|
| 225 (ASM_PANIC) | `int ASM_PANIC` 触发内核恐慌 |
| 226 (KTHREAD_CALL) | 跨核函数调用 |
| 240 (IPI) | 核间中断 |
| 255 (SUPRIOUS) | 虚假中断检测记录 |

---

## 9. 文件清单

| 文件 | 内容 |
|------|------|
| `Sysdef_exception_entries.asm` | VEC_DELIVERY_ENTRY 宏 + 224 个入口 + swapgs |
| `x86_vecs_deliver_mgr.h` | 类声明 + KURD 事件代码 + extern "C" 统一接口 |
| `x86_vecs_deliver_mgr.cpp` | Init / alloc_vec / free_vec / get_vec + all_vec_delivery |
| `fixed_interrupt_resources.cpp` | logical_idt/global_idt/soft_interrupt_functions + template_idt_apply_region |
| `loacl_processor.h` | TSS / IDTEntry / logical_idt / GS slot 常量 |
| `Interrupt.h` | hard_interrupt_func_t / soft_interrupt_func_t 类型定义 |
| `fixed_interrupt_vectors.h` | ivec 命名空间 (向量编号常量) |
| `lapic.cpp` | LAPIC timer + error handler 注册示例 |
| `local_processor_manage.cpp` | per-CPU GS slot 初始化 (函数表指针) |
