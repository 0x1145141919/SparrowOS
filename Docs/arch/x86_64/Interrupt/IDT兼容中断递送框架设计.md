# IDT 兼容中断递送框架设计

## 动机

FRED 机制提供了向量号递送能力——每个中断入口自动携带向量参数，高级语言入口
统一接收 `(context, vec)` 无需查表。但在 FRED 硬件普及前，基于传统 IDT 实现
相同的设计理念。

核心思路：256 个 IDT 入口各自嵌入不同的向量号，高级语言入口统一接收 `(context, vec)`。

## 纳入范围

| 纳入 | 排除 |
|------|------|
| 向量 32～255 的软中断 (int n) | 向量 0～31 的 CPU 异常 (各带专用入口) |
| 向量 32～255 的硬件中断 (外部/消息) | FRED 模式 (下行接口预留) |

## 架构分层

```
  IDT (256 entry)
    │
    ├─ [0..31] CPU 异常 → 专用 bare_enter → cpp_enter
    │
    └─ [32..255] VEC_DELIVERY_ENTRY (224 个，宏批量生成)
         │
         └─ all_vec_delivery(frame, vec)
              │
              ├─ soft_interrupt_functions[vec] → 带 frame，可跑飞
              │    ├─ KTHREAD_CALL (226): yield/exit/wait/sleep/block
              │    ├─ IPI          (240): AP bringup up/down
              │    ├─ ASM_PANIC    (225)
              │    └─ SUPRIOUS     (255)
              │
              └─ self->tokens[vec].func(&tok) → 无 frame，必须返回
                   ├─ LAPIC Timer
                   ├─ i8042 / NVMe CQ / DMAR
                   └─ TLB flush IPI (非跑飞型)
```

## 两路分流设计

**软中断（soft_interrupt_functions）**：
- 全局、同步、由 `int` 指令触发
- handler 接收 `x64_standard_context* frame`，可访问/修改 CPU 寄存器状态
- 允许执行流跑飞（即不返回），例如 kthread_call 内部的 sched()
- 256 项函数指针表，空指针视为未注册

**硬件中断（tokens[256]）**：
- per-CPU、异步、由 LAPIC/IOAPIC/MSI 触发
- handler 接收 `interrupt_token_t* token`，**无 frame**
- handler 必须返回，禁止跑飞
- 框架统一做 EOI，根据 handler 返回值 + token->flags 决策是否调度
- gs_complex_t::tokens[256] 数组（每 CPU 一份）

## 核心数据结构

```cpp
// Interrupt.h
struct interrupt_token_t {
    uint64_t flags;           // bit 0: NEED_RESCHED
    uint64_t token_private;   // O(1) trigger source encoding
    void   (*func)(interrupt_token_t* token);
};

// GS_complex.h — per-CPU 存储
struct gs_complex_t {
    uint64_t slots[256];             // [0x0000, 0x0800)
    interrupt_token_t tokens[256];   // [0x0800, 0x1800) ← 6144 bytes
    // ... GDT/TSS/FPU/stacks ...
};
```

## 管理接口

```cpp
class idt_vec_dispatch_mgr {
    static KURD_t Init();
    static uint8_t alloc_vec(interrupt_token_t* token, uint32_t pid, KURD_t&);
    static uint8_t alloc_vec_by_apicid(interrupt_token_t*, uint32_t apicid, KURD_t&);
    static KURD_t free_vec(uint8_t vec, uint32_t pid);
    static interrupt_token_t* get_vec(uint8_t vec, uint32_t pid, KURD_t&);
};

// extern "C" 统一入口，IDT/FRED 切换时只改这里
uint8_t out_interrupt_vec_alloc(interrupt_token_t*, uint32_t pid, KURD_t*);
uint8_t out_interrupt_vec_alloc_by_apicid(interrupt_token_t*, uint32_t apicid, KURD_t*);
KURD_t out_interrupt_vec_free(uint8_t vec, uint32_t pid);
interrupt_token_t* out_interrupt_vec_get(uint8_t vec, uint32_t pid, KURD_t*);
```

## alloc_vec 验证链

1. token->func != nullptr → BAD_FUNC_PTR
2. is_addr_kernel_address(token->func) → BAD_FUNC_PTR
3. ksymmanager 精确地址匹配 → SYM_NOT_FOUND
4. processor_id / apicid 合法性 → INVALID_PROCESSOR_ID
5. 扫描 32..255 跳过 soft_interrupt_functions 占位 → NO_FREE_VEC
6. dispatch_lock 保护下写入

## 中断向量布局

| 范围 | 用途 |
|------|------|
| 0～31 | CPU 异常（专用入口）|
| 32～224 | 自由分配（硬件中断 token 表）|
| 225 | ASM_PANIC（软中断）|
| 226 | KTHREAD_CALL（软中断）|
| 240 | IPI（跑飞型，软中断）|
| 241～254 | 自由分配 |
| 255 | SUPRIOUS_INTERRUPT（软中断）|
