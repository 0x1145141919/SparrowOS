# IPI 设计演进：从函数指针到消息槽

日期: 2026-06-04
最后修订: 2026-06-06 (v3.2)
涉及文件: x86_vecs_deliver_mgr.cpp / GS_complex.h / cmpxchg16b.asm / fast_get_ids.asm
          tsc.cpp / exec_env_detect.cpp / lock.h

## 演进脉络

```
global_ipi_handler (v1)     →    独立向量表 (v2)     →    local_ipi_complex (v3)
   2026-05-25                     2026-05-26                 2026-06-04

  一个全局函数指针          per-vector 固定路由         16B cmpxchg16b 消息槽
  一个向量号 240           三个向量层次                 per-core 原子 {fn, arg}
  一锤子买卖               soft/token/alloc 分层        3 个向量 + 关中断临界区
```

## 第一章：v1 — global_ipi_handler（略，见历史版本）

## 第二章：v2 — 独立向量表（略，见历史版本）

## 第三章：v3 — local_ipi_complex（当前实现）

### 核心观察

v2 未解决：
1. **投递竞争**：两个发送者同时向同一目标发不同 IPI → 表项被覆盖
2. **通信原语过重**：每个 IPI 类型对应一个表项，无参数携带能力
3. **ACK 机制外挂**：`completed_processors_count` 独立于 IPI 传递机制

### 数据结构

```cpp
// GS_complex.h
struct __attribute__((packed)) alignas(4096) gs_complex_t {
    uint64_t slots[256];                    // @0x0000
    interrupt_token_t tokens[256];          // @0x0800
    x64_gdtentry        gdt[6];             // @0x2000
    TSSDescriptorEntry  tss_descriptor;     // @0x2030
    TSSentry            tss;                // @0x2040
    alignas(64) uint8_t fpu_area[8192];     // @0x20C0
    alignas(64) __uint128_t local_ipi_complex; // @0x40C0 — 16B 原子槽
    uint64_t padding[6];                    // 填满 64B 缓存线
    per_processor_hardware_stack_t* stacks_ptr; // @0x4100
};
```

**local_ipi_complex 编码** (16B per-core 原子消息槽)：
- `0x0` — 空闲（可抢占）
- `{func_lo, arg_hi}` — 有消息待处理（sender 写入）
- `{1, arg_hi}` — RUNAWAY 已消费（target 将 lo64 置 1，非完全释放）
- `{1, ret_hi}` — RETURNABLE 结果就绪（target 写回返回值）

**关键差异（对比原始 v3 文档）**：
- `alignas(16)` → `alignas(64)` 缓存线对齐，适配 UMONITOR
- 释放到 `{0, 1}` 而非 `0` —— sender 靠 `(uint64_t)slot != func` 判断而非 `== 0`
- 返回值编码：`hi64=ret, lo64=1`（`lo64` 永远非 0，避免 `ret=0` 时歧义）

### 向量分配

| 向量 | 名称 | 行为 | Slot 交互 |
|------|------|------|-----------|
| 252 | `IPI_HALT` | 硬编码，跳过 slot，`cli; hlt` | 不碰 slot |
| 253 | `IPI_RETURNABLE` | 通过 slot 收发，返回结果 | 读 fn→执行→写回 `{1, ret}` |
| 254 | `IPI_RUNAWAY` | 通过 slot 收发，不返回 | 读 fn→写 `{1, 0}`→EOI→执行 |
| 255 | `SUPRIOUS_INTERRUPT` | 虚假中断检测 | 不碰 slot |

### cmpxchg16b 原子原语

```nasm
; cmpxchg16b.asm — 纯 NASM，16 条指令
; bool cmpxchg16b(void* ptr, void* expected, const void* desired);
;   成功 → *ptr = *desired, return true
;   失败 → *expected = *ptr,  return false
```

### 收端时序（IDT 路径）

**IPI_HALT**:
```
收端: EOI → cli; hlt   🠔 不碰 slot，永不返回
```

**IPI_RETURNABLE**:
```
收端: lo64=fnbox_copy=*slot (保存原始 msg)
     fnbox = (local_ipi_complex_fnbox_t*)slot
     result = fnbox->func(fnbox->arg)
     result_box = (__uint128_t)result << 64 | 1   🠔 lo64=1
     cmpxchg16b(slot, &fnbox_copy, &result_box)    🠔 原子替换
     EOI
```
slot 状态：`{fn, arg}` → `{1, result}`（始终非零，阻挡新消息）

**IPI_RUNAWAY**:
```
收端: fnbox_copy=*slot
     get_func_mail = 1
     cmpxchg16b(slot, &fnbox_copy, &get_func_mail) 🠔 slot={1, 0}
     EOI
     fnbox.func(fnbox.arg)                         🠔 不返回
     ud2
```
slot 状态：`{fn, arg}` → `{1, 0}`（lo64=1 防止误认空闲）

### FRED 路径

FRED 自动 EOI，handler 中不调用 `write_eoi()`。逻辑与 IDT 路径相同，
但 `msg = *slot` 在 switch 前统一捕获。

### 发端接口

```cpp
// ipi_package_t 统一参数包
struct ipi_package_t {
    void*    arg;           // 函数参数
    uint64_t func;          // 函数指针（整数化，避类型系统干预）
    uint32_t id;            // x2APIC ID 或 logical processor ID
    bool     is_apicid;     // true=id 是 APIC ID, false=processor_id
    bool     is_returnable; // true=RETURNABLE, false=RUNAWAY
};

// 返回型：hi64=fn(arg), lo64=结果码
//   1=成功, 2=抢占失败(BUSY), 3=超时, 4=目标不存在
__uint128_t ret_ipi_send(ipi_package_t* package);

// 跑飞型：lo64=结果码
uint64_t fly_ipi_send(ipi_package_t* package);

// 停机
void broadcast_halt();      // 广播 ALL_EXCLUDING_SELF
void halt_on(uint32_t id, bool is_apicid);  // 定向
```

### ret_ipi_send 发端时序

```
ret_ipi_send(package):
  interrupt_guard ON       🠔 cli 保护临界区
  while(!cmpxchg16b(slot, 0→{func, arg})) pause()  🠔 抢占槽
  raw_send_ipi(ICR, IPI_RETURNABLE, dest_apicid)
  deadline = now + 10ms

  loop:
    val = *slot
    if (uint64_t)val != func:     🠔 lo64 已变 → target 消费
      result = val >> 64
      *slot = 0                   🠔 释放
      return {result, SUCCESS}
    if now >= deadline:
      *slot = 0                   🠔 超时强占
      return {0, TIMEOUT}
    cacheline_wait(slot)          🠔 UMONITOR + UMWAIT
    goto loop
  interrupt_guard OFF
```

### fly_ipi_send 发端时序

```
fly_ipi_send(package):
  interrupt_guard ON
  cmpxchg16b(slot, 0→{func, arg})
  raw_send_ipi(ICR, IPI_RUNAWAY, dest_apicid)
  loop:
    if (uint64_t)*slot == 1:     🠔 target 已置 lo64=1
      *slot = 0
      return SUCCESS
    if timeout: *slot=0; return TIMEOUT
    cacheline_wait(slot)
    goto loop
  interrupt_guard OFF
```

### 探测 WAITPKG 与停止策略

- KVM / BARE_METAL: **WAITPKG 必须支持**，否则 `tsc_panic_hlt()`
- TCG: 无条件 fallback 到 `pause()` spin（TCG 无 UMONITOR/UMWAIT）
- WAITPKG 检测点：`tsc_regist()` 末尾，一次 cpuid 决定系统能否启动

### cacheline_wait — 硬件休眠等待

```cpp
// 监听 64B 对齐缓存行，等待 store 命中或被 OS deadline 唤醒
// EDX:EAX = UINT64_MAX → 忽略指令 deadline，IA32_UMWAIT_CONTROL 绑定
static inline void cacheline_wait(void* addr) {
    umonitor(addr);
    // 调用者随后应双检条件（可能因 OS 超时或虚假唤醒返回）
    umwait(C0.1, EDX:EAX=~0ULL);
}
```

### IA32_UMWAIT_CONTROL 配置

```
MSR 0xE1:
  [31:2] = (cycles_50us >> 2)   🠔 50μs TSC 周期上限
  [0]    = 1                     🠔 允许 C0.2
  计算: cycles_50us = 50 * FS_per_mius / tsc_fs_per_cycle

  生效范围: per-core（BSP 初始化时写入，AP init 序列各自调用 apply_umwait_control()）
```

### gs_complex_t cross-reference

`g_gs_by_apicid[x2apic_id]` → gs_complex_t*（全局映射表）
`conjucnt_GSs.vbase() + pid * GS_COMPLEX_STRIDE` → per-processor 数组

### 配套工具函数

```cpp
// fast_get_ids.asm — NASM 裸汇编
uint32_t fast_get_processor_id();    // mov rax, [gs:8]; and eax, 0xFFFFFFFF
uint32_t fast_get_x2apic_id();       // mov rax, [gs:8]; shr rax, 32

// GS_slots[1] 编码: [31:0]=logical processor id, [63:32]=x2APIC id
// APs_bringup.cpp 写入:
//   BSP: slot[1] = self_x2apicid << 32 | 0
//   AP:  slot[1] = it->apicid << 32 | pid
```

### dead code 移除

- `ipi_descrioptors[256]` 全局数组（已删）
- `ipi_descrioptors` 初始化 + FRED 引用（已删）
- `suprious_interrupt_cpp_enter` 软中断注册（已删）
- v2 向量命名 `return_ipi_vec` / `runaway_ipi_vec` → `ipi_vecs`（已迁移）

### 与 v2 / v1 对比

| 维度 | v1 | v2 | v3 |
|------|-----|-----|-----|
| 投递机制 | 写全局 ptr | 查表 indirect call | cmpxchg16b 原子槽 |
| 竞争保护 | 无 | 局部（表项替换） | 强（cmpxchg 排他 + interrupt_guard） |
| 参数传递 | 无（全局变量） | 无（仅函数指针） | {fn, arg} 16B 内嵌 |
| 返回值 | 无 | 无 | 编码回 slot hi64 |
| ACK 机制 | 无 | 外挂计数器 | 槽归零即 ACK |
| 死核检测 | 不可能 | 困难（计数器盲） | 轮询超时可知哪核 |
| 向量用量 | 1 个（240） | 5+ 个 | 3 个 |
| 等待策略 | spin | spin | UMONITOR/UMWAIT + pause fallback |
| 临界区保护 | 无 | spinlock | interrupt_guard |
| per-CPU 差异化 | 无 | 有（per-vector） | 有（per-slot fn） |

## 附录 A：剩余待实现

1. 迁移 `broadcast_exself_fixed_ipi` → `ipi_broadcast_exself_unret`
2. 迁移 TLB shootdown → `ret_ipi_send`
3. 迁移 `panic()` 中的 `other_processors_froze_handler` → `broadcast_halt()`
4. 废弃 `global_ipi_handler` 残留
5. 废弃 `soft_interrupt_functions[256]` 中不再使用的槽位

## 附录 B：参考实现

- 发端: `x86_vecs_deliver_mgr.cpp` → `ret_ipi_send()` / `fly_ipi_send()`
- 收端: `x86_vecs_deliver_mgr.cpp` → `idt_vec_demux_entry()` switch cases
- FRED: `x86_vecs_deliver_mgr.cpp` → `fred_vec_demux_hw_dispatch()`
- 槽: `GS_complex.h` → `local_ipi_complex`
- CAS: `Processor/cmpxchg16b.asm`
- ID 读取: `Processor/fast_get_ids.asm`
- UMWAIT 配置: `x86_arch/tsc.cpp` → `g_umwait_control_value`
- UMWAIT 探测: `exec_env_detect.cpp` → `g_env`
