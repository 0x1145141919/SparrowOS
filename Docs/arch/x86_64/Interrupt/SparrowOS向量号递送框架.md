# SparrowOS 向量号递送框架

x86 向量号上限 256，IDT 把异常、软中断、硬件中断、IPI 四类异质机制押在同一个线性空间。
FRED 虽然用 SS.type 分离了软中断(type=1)和硬件中断(type=2)，但硬件中断内部仍然杂糅 IPI 与设备中断。
本文档描述实际代码落地。

---

## 1. 四类中断 — 各走各路

| 类别 | 触发方式 | 数据结构 | 作用域 | 可否跑飞 | 向量号分配 |
|------|---------|---------|--------|---------|-----------|
| CPU 异常 | 硬件条件 | 专用 C++ 入口 | 架构固定 | 否 | 0–31 固定 |
| 软中断 | `int N` (同步指令) | `soft_interrupt_functions[256]` | 全局 | 是 (UD2 sentinel) | 固定 (225–227, 255) |
| 系统 IPI | LAPIC ICR (异步) | `ipi_descrioptors[256]` | 全局 | 看 `is_no_return` | 固定 (250–254) |
| 硬件中断 | 外部/MSI (异步) | `gs_complex_t::tokens[256]` per-CPU | per-CPU | 否 (必须返回) | alloc_vec 动态 |

---

## 2. 向量号布局

```
 0–31    x86_exceptions          CPU 异常（架构固定，专用入口）
32–224   [free pool]              alloc_vec 自由分配区（硬件中断/远端函数调用）
225      x86_softinterrupt_abi::ASM_PANIC      软中断 — 内核恐慌
226      x86_softinterrupt_abi::KTHREAD_CALL   软中断 — 跨核函数调用
227      x86_softinterrupt_abi::USER_ABI_ENTER 软中断 — 用户态系统调用
228–249  [free pool]              中间空档
250      return_ipi_vec::LOCAL_TLB      系统 IPI — TLB shootdown（返回型）
251      return_ipi_vec::GLOBAL_TLB     系统 IPI — 全局 TLB（返回型）
252      runaway_ipi_vec::IPI_HALT      系统 IPI — AP 停机（不返回）
253      runaway_ipi_vec::RESCHEDDUE    系统 IPI — 远程重调度（不返回）
254      runaway_ipi_vec::START_SCHED   系统 IPI — AP 上线/启动调度（不返回）
255      [SUPRIOUS_INTERRUPT]           软中断 — 虚假中断检测
```

约束： **软中断向量集、系统 IPI 向量集、硬件中断向量集两两不可相交。**

---

## 3. 核心数据结构

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

三张全局表：
```cpp
soft_interrupt_func_t soft_interrupt_functions[256];  // 全局，软中断
ipi_descrioptor_t     ipi_descrioptors[256];           // 全局，系统 IPI
// tokens[256] 在 per-CPU gs_complex_t 中
```

---

## 4. IDT 分发逻辑 — all_vec_delivery

`src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp`

IDT 入口只有一个信息：向量号。靠死顺序决定归属：

```
all_vec_delivery(frame, vec):

  if vec < 32 → panic("BAD_VEC_RECIEVED")  // 异常有专用入口

  ── 1. 软中断表 (全局, 同步, int N) ──
  if soft_interrupt_functions[vec] {
      soft_interrupt_functions[vec](frame)
      asm("ud2")   // sentinel — 软中断必须跑飞或 panic，返回即 bug
  }

  ── 2. 系统 IPI 表 (全局, LAPIC ICR) ──
  if ipi_descrioptors[vec].func {
      ipi_descrioptors[vec].func(frame)
      write_eoi()
      if ipi_descrioptors[vec].is_no_return {
          asm("ud2")  // 不返回型 IPI 返回了 → 崩
      }
      return
  }

  ── 3. 硬件中断表 (per-CPU, 外部/MSI) ──
  if self->tokens[vec].func {
      uint64_t res = self->tokens[vec].func(&self->tokens[vec])
      write_eoi()
      if res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE {
          resched(frame)  // 触发重调度
      }
      return
  }

  ── 4. 未匹配 → 虚假中断 ──
  bsp_kout warning
```

优先级：软中断 > IPI > 硬件中断。软中断和 IPI 的优先级高于硬件中断，因为它们是系统语义级别的同步/全局事务。

---

## 5. FRED 分发（预留）

FRED 下 SS.type 直接分离：
- `type = 0` → 异常（独立入口，不经过 all_vec_delivery）
- `type = 1` → 软中断（自由分配，不冲突）
- `type = 2` → 硬件中断（杂糅 IPI）→ 顺序：tokens → ipi_descrioptors（与 IDT 反序）

硬件中断中先查 tokens（高频设备中断），再查 ipi_descrioptors（低频系统 IPI）。

软中断向量集在 FRED 下可自由分配（type 分离），但系统 IPI 表必须在 Init 中初始化且不得自由分配。

---

## 6. 初始化流程 — idt_vec_dispatch_mgr::Init

1. **清零 tokens 表**：遍历所有 CPU 的 `gs_complex_t`

2. **符号表扫描**：查找 `vec_delivery_32_bare_enter` → 递增尝试直到断裂 → 全表补扫

3. **填入 template_idt**：`handler=symbol.address, type=0xE, ist=0, dpl=0`

4. **设置 USER_ABI_ENTER DPL=3**：用户态 `int 227` 可触发

5. **写入 global_idt**：`template_idt_apply_region(32, 255)`

6. **初始化软中断函数表**：
   ```
   soft_interrupt_functions[225] = asm_panic_cpp_enter
   soft_interrupt_functions[226] = kthread_call_cpp_enter
   soft_interrupt_functions[227] = user_abi_cpp_enter
   soft_interrupt_functions[255] = suprious_interrupt_cpp_enter
   ```

7. **初始化 IPI 描述符表**（占位，func=nullptr，由 IPI 子系统后续填充）：
   ```
   ipi_descrioptors[254] — START_SCHED,  is_no_return=true
   ipi_descrioptors[253] — RESCHEDDUE,   is_no_return=true
   ipi_descrioptors[252] — IPI_HALT,     is_no_return=true
   ipi_descrioptors[251] — GLOBAL_TLB,   is_no_return=false
   ipi_descrioptors[250] — LOCAL_TLB,    is_no_return=false
   ```

8. 未找到全部 224 个符号 → Panic(NOT_ALL_IDT_FOUND)

---

## 7. alloc_vec 扫描约束

```cpp
for (vec = 32; vec <= 255; vec++) {
    if (soft_interrupt_functions[vec])  continue;  // 跳过软中断固定槽
    if (ipi_descrioptors[vec].func)     continue;  // 跳过系统 IPI 固定槽  ← 当前代码遗漏此项
    if (slice[vec].func == nullptr)     break;     // 空槽则分配
}
```

> ⚠️ **当前代码状态**：`alloc_vec` 和 `alloc_vec_by_apicid` 仅跳过 `soft_interrupt_functions`，未跳过 `ipi_descrioptors`。向量 250–254 可能被硬件中断分配走，与 IPI 子系统冲突。需要补充 `ipi_descrioptors` 跳过逻辑。

验证链（6 步）：
1. `token->func != nullptr` → BAD_FUNC_PTR
2. `is_addr_kernel_address` → BAD_FUNC_PTR
3. ksymmanager 精确匹配 → SYM_NOT_FOUND
4. processor_id/apicid 合法性 → INVALID_PROCESSOR_ID
5. 扫描 32–255 跳过软中断 + IPI 占位 → NO_FREE_VEC
6. `dispatch_lock` (spinlock_interrupt_about_guard) 保护写入

---

## 8. 管理接口

```cpp
// 统一外部接口（IDT/FRED 切换点）
uint8_t out_interrupt_vec_alloc(interrupt_token_t*, uint32_t pid, KURD_t*);
uint8_t out_interrupt_vec_alloc_by_apicid(interrupt_token_t*, uint32_t apicid, KURD_t*);
KURD_t  out_interrupt_vec_free(uint8_t vec, uint32_t pid);
interrupt_token_t* out_interrupt_vec_get(uint8_t vec, uint32_t pid, KURD_t*);
```

---

## 9. 已注册的中断使用场景

| 场景 | 路径 | 向量来源 |
|------|------|---------|
| LAPIC Timer | tokens (per-CPU) | alloc_vec 动态 |
| LAPIC Error | tokens (per-CPU) | alloc_vec 动态 |
| i8042 键盘 | tokens (per-CPU) | alloc_vec 动态 |
| NVMe CQ | tokens (per-CPU) | alloc_vec 动态 |
| DMAR fault | tokens (per-CPU) | alloc_vec 动态 |
| ASM_PANIC | soft_interrupt_functions[225] | 固定 |
| KTHREAD_CALL | soft_interrupt_functions[226] | 固定 |
| USER_ABI_ENTER | soft_interrupt_functions[227] | 固定 |
| SUPRIOUS | soft_interrupt_functions[255] | 固定 |
| START_SCHED | ipi_descrioptors[254] | 固定 (func=nullptr 待填) |
| RESCHEDDUE | ipi_descrioptors[253] | 固定 (func=nullptr 待填) |
| IPI_HALT | ipi_descrioptors[252] | 固定 (func=nullptr 待填) |
| GLOBAL_TLB | ipi_descrioptors[251] | 固定 (func=nullptr 待填) |
| LOCAL_TLB | ipi_descrioptors[250] | 固定 (func=nullptr 待填) |

---

## 10. 文件清单

| 文件 | 内容 |
|------|------|
| `fixed_interrupt_vectors.h` | 向量编号常量（x86_exceptions/x86_softinterrupt_abi/runaway_ipi_vec/return_ipi_vec） |
| `Interrupt.h` | interrupt_token_t / soft_interrupt_func_t / ipi_descrioptor_t 类型定义 |
| `x86_vecs_deliver_mgr.h` | idt_vec_dispatch_mgr 类声明 + KURD 事件代码 + extern "C" 接口 |
| `x86_vecs_deliver_mgr.cpp` | Init / alloc_vec / free_vec / get_vec / all_vec_delivery |
| `fixed_interrupt_resources.cpp` | logical_idt/global_idt + template_idt_apply_region |
| `loacl_processor.h` | TSS / IDTEntry / logical_idt / GS slot |
| `GS_complex.h` | gs_complex_t (含 tokens[256]) |
| `Sysdef_exception_entries.asm` | VEC_DELIVERY_ENTRY 宏 + 224 个入口 |
| `exceptions_handler.cpp` | 各软中断/IPI handler 实现 |
| `lapic.cpp` | LAPIC timer + error handler 注册 |
