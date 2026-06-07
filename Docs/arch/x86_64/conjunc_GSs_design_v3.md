# conjunc_GSs — GS 复合体设计 v3

## 0. v2 → v3 哲学转变

| 维度 | v2 视角 | v3 视角 |
|------|---------|---------|
| `gs_complex_t` 本质 | 精心设计的固定布局结构体，含手动 hex 偏移和显式 `_pad` 字段 | **ABI 容器**：只有 `slots[256]` 是结构体"拥有的"，其余字段是其他子系统注入的**客（guest）** |
| 布局 | 开发者手算 offset，`_pad0`/`_pad1` 推到目标位置 | 编译器通过 `__attribute__((packed)) alignas(4096)` 自动布局，开发者不应假设或硬编码成员偏移 |
| `[gs:offset]` 访问 | 预期可用 | **只允许 slot 索引**（`[gs:slot_idx]`），其他成员仅通过 `->` 成员访问 |
| 物理存在 | 抽象描述为"per-CPU 复合体" | 明确为一个 `vm_interval` 数组：`conjunc_GSs[processor_id]` |
| 自引用 | BSP 直接设 GS_BASE，AP 从区间计算 | **g_gs_by_apicid 映射表**——AP 醒来用 x2APIC ID 查表 |
| struct 大小 | ≈ 76 KB（20 pages per CPU） | ≈ 16 KB（4 pages per CPU），实际由编译器根据 guest 字段决定 |

下文的 "v2" 指代 `conjunc_GSs_design_v2.md`，"v3" 指本文档。

---

## 1. gs_complex_t 本质：ABI 容器

`gs_complex_t` 不是一个"per-CPU 数据结构设计"，它是一个 **ABI 容器**。

### 1.1 所有权边界

```
┌──────────────────────────────────────────────┐
│  gs_complex_t                                │
│                                              │
│  ● slots[256]        ← 结构体拥有           │
│                      这是唯一可通过 [gs:ofs]  │
│                      寻址的区域，ABI 保证     │
│                      固定 256 × uint64_t     │
│                                              │
│  ● tokens[256]       ← 中断子系统注入        │
│  ● gdt[6]            ← GDT/TSS 子系统注入   │
│  ● tss_descriptor    ← 同上                  │
│  ● tss               ← 同上                  │
│  ● fpu_area[]        ← FPU 子系统注入        │
│  ● local_ipi_complex ← IPI 子系统注入        │
│  ● padding[]         ← 对齐垫片              │
│  ● stacks_ptr        ← 硬件栈子系统注入      │
│                                              │
│  这些字段不构成 ABI，位置由编译器根据 packed  │
│  规则自动排列，任何组件不得假设其精确偏移。   │
└──────────────────────────────────────────────┘
```

v2 用 `_pad0[0x10C0 - 0x10A8]`、`_pad1[0x4000 - 0x30C0]` 等手动计算推着字段走——这本身就是对"ABI 容器"理念的误解。正确的做法是：结构体用 `packed` + `alignas`，布局交给编译器，各子系统只管用 `->` 成员访问。

### 1.2 容器契约

| 契约 | 说明 |
|------|------|
| `slots[256]` | 唯一 ABI。可通过 `[gs:slot_idx]` 在汇编/C++ 中寻址。8 字节对齐 |
| 其余字段 | 作为 C++ 结构体成员通过 `complex->field` 访问。**禁止**计算或依赖其 `[gs:offset]` |
| 布局规则 | `__attribute__((packed))` —— 不插填充；`alignas(4096)` —— 实例 4K 对齐、size 为 4K 倍数 |
| 不变量 | `sizeof(gs_complex_t) % 4096 == 0` 编译期断言保证 |

### 1.3 为什么这样设计

1. **去 ABI 耦合** — 如果每个字段的 offset 都变成 ABI，改字段排序或增删字段就需要更新所有 `[gs:offset]` 的引用处。v2 的手算 _pad 体系本质上是手写 ABI 表，改了 struct 就得重算。
2. **编译器保证正确性** — `offsetof` 比手算可靠，`packed` + 编译器自动排列比手写 _pad 可维护。
3. **热路径性能不损失** — C++ `complex->tokens[vec]()` 编译为 `mov rax, [gs:offset_of_tokens + vec*24]; call [rax]`，与手动 `[gs:0x800 + vec*24]` 效果相同，但前者不依赖偏移常数。

---

## 2. 物理存在：conjunc_GSs 数组

`gs_complex_t` 的肉体存在是 **init.elf Phase 3b 分配的一个全局 `vm_interval`**：

```cpp
// init_init.cpp Phase 3b
uint64_t total_bytes = logical_processor_count * GS_COMPLEX_STRIDE;
iv.arch_info.conjunc_GSs = allocate_probe_keep(...) → 清零 → map → 写入 iv
```

### 2.1 数组语义

```
conjunc_GSs.vbase()      → gs_complex_t[0]   (BSP)
conjunc_GSs.vbase() + stride → gs_complex_t[1]   (AP #1)
conjunc_GSs.vbase() + N*stride → gs_complex_t[N] (AP #N)

下标 = processor_id
stride = GS_COMPLEX_STRIDE = (sizeof(gs_complex_t) + 4095) & ~4095
```

这个 `conjunc_GSs` 的作用：

1. **跨处理器访问** — 任意处理器可通过 `conjucnt_GSs.vbase() + pid * stride` 访问另一个处理器的 gs_complex_t（中断子系统分发 IPI 时查目标 token 表）
2. **一次分配减少偶然复杂度** — 不逐个处理器独立分配，整个区间连续、一次映射、物理连续
3. **持久化方便** — `simp_pages_set` 一个区间标记即可
4. **CR3 重建映射** — 一个 `vm_interval` 直接喂给 `Kspace_phyaddr_direct_map`

### 2.2 跨处理器访问模式

```cpp
// 从 x86_vecs_deliver_mgr.cpp — 按 processor_id 找目标 gs_complex_t
gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + processor_id * GS_COMPLEX_STRIDE);
```

---

## 3. 两个访问视图

运行时通过两种方式索引到 gs_complex_t：

### 3.1 视图 A：conjunc_GSs[processor_id]

| 属性 | 值 |
|------|-----|
| 索引方式 | `processor_id`（0 = BSP，1..N = AP） |
| 访问路径 | `conjucnt_GSs.vbase() + pid × stride` |
| 用途 | 跨处理器访问、中断分发查目标 token |
| 类型 | 密集连续数组 |
| 定址范围 | 全部处理器，直接通过 processor_id |

### 3.2 视图 B：g_gs_by_apicid[x2APIC ID]

```cpp
// APs_bringup.cpp
gs_complex_t** g_gs_by_apicid = new gs_complex_t*[max_apicid + 1]();
```

| 属性 | 值 |
|------|-----|
| 索引方式 | `x2apicid`（硬件 APIC ID，由 MADT 枚举） |
| 访问路径 | `g_gs_by_apicid[x2apicid]` |
| 用途 | **AP 自引导**（AP 醒来只知道自己的 x2APIC ID） |
| 类型 | 稀疏映射表（`max_apicid + 1` 条目，可能有空槽） |
| 定址范围 | 只有表中存在的 APIC ID |

#### 为什么需要这个表

AP 在 `ap_bootstrap_init()` 入口点只知道自己的硬件 x2APIC ID（从 x2APIC MSR 读到），不知道 processor_id。没有这个映射表，AP 无法找到自己在 conjunc_GSs 数组中的位置。

BSP 在第一遍扫描 MADT 时分配表，第二遍填值：

```cpp
// BSP
g_gs_by_apicid[self_x2apicid] = &conjunc_GSs[0];

// 每个 AP
g_gs_by_apicid[apicid] = &conjunc_GSs[pid];  // pid = processor_id
```

#### 自引导路径完整流程（local_processor_manage.cpp）

```cpp
ap_bootstrap_init():
    apicid = x2apic_core_init()            // 1. 获知自己的 APIC ID
    self   = g_gs_by_apicid[apicid]        // 2. 查表得到 gs_complex_t
    wrmsr(IA32_GS_BASE, self)              // 3. 设 GS_BASE
    wrmsr(IA32_KERNEL_GS_BASE, self)       //    MSR 双备份
    gs_complex_load_gdt_tss(self)          // 4. LGDT + LTR
    return self->tss.rsp0                  // 5. 返回栈底给 asm 跳板
```

#### 视图关系

```
                    processor_id（运行时可计算）
                         │
                    ┌────▼────┐
                    │conjunc_GSs array│
                    │ [pid_0..pid_N]  │
                    └────┬────┘
                         │
           ┌─────────────┼──────────────┐
           │             │              │
      ┌────▼───┐   ┌────▼───┐    ┌─────▼───────┐
      │自引用   │   │跨核访问│    │g_gs_by_apicid│
      │        │   │        │    │[x2apicid]    │
      │rdmsr   │   │conjunc │    │ 映射表        │
      │GS_BASE │   │[pid]   │    │ AP self-path │
      └────────┘   └────────┘    └──────────────┘
```

---

## 4. Guest Field 注入模型

其他子系统如何向 `gs_complex_t` 注入字段：

### 4.1 注入方式

直接在 `GS_complex.h` 的结构体中添加成员。因为是 `__attribute__((packed))`，成员按声明顺序连续排列，编译器自动处理对齐需求（`alignas` 对单个成员有效）。

### 4.2 当前注入清单

| 子系统 | 注入字段 | 类型 | 用途 |
|--------|---------|------|------|
| 中断子系统 | `tokens[256]` | `interrupt_token_t[256]` (24B × 256) | 256 向量中断 token 表 |
| GDT/TSS 子系统 | `gdt[6]` | `x64_gdtentry[6]` | 全局描述符表（Kcode/Kdata/Ucode/Udata + TSS 描述符） |
| GDT/TSS 子系统 | `tss_descriptor` | `TSSDescriptorEntry` | TSS 段描述符（基址 = &tss） |
| GDT/TSS 子系统 | `tss` | `TSSentry` | 任务状态段（含 IST 栈指针） |
| FPU 子系统 | `fpu_area` | `uint8_t[XSAVE_SIZE_MAX]` | XSAVE64/XRSTOR64 暂存区 |
| IPI 子系统 | `local_ipi_complex` | `__uint128_t` | 本地 IPI 通信字段 |
| — | `padding[6]` | `uint64_t[6]` | 填充至 64B 满足 umwait 监视要求 |
| 硬件栈子系统 | `stacks_ptr` | `per_processor_hardware_stack_t*` | 指向 hdstacks 区间中对应实例 |

### 4.3 注入约束

1. **不能动 `slots[256]`** —— 这是结构体唯一的 ABI 部分，位置必须在开头，布局不可变
2. **使用 `->` 成员访问，不用 `[gs:offset]`** —— 编译器自动处理 offset
3. **使用 `offsetof` 宏** —— 需要编译期偏移量时（如 `static_assert`），用 `offsetof` 而非手算 hex
4. **不能假设成员间距离** —— `packed` 下无填充，但增删字段会影响后续所有成员的偏移，依赖 `pad1` 之前正好 N 字节是脆弱的

### 4.4 `interrupt_token_t` 说明

v2 使用 `hard_interrupt_func_t dispatch[256]`（纯函数指针表），v3 改用三层 token：

```cpp
struct interrupt_token_t {
    uint64_t flags;
    uint64_t token_private;
    uint64_t (*func)(interrupt_token_t* token);
};
```

- 每个 token 24 字节（vs v2 的 8 字节函数指针）
- 支持 token 级私有数据、flag 标记
- 通过 `x86_vecs_deliver_mgr` 分配：`alloc_vec(&token, processor_id, kurd)`
- 仍然零间接分发：`self->tokens[vec].func(&self->tokens[vec])`

---

## 5. compil 实际布局（仅供参考，非 ABI）

以下布局由 `__attribute__((packed))` + 当前字段声明顺序 + 各子系统注入产生。**这些偏移不是契约，只是当前编译器产生的快照。** 增删任何 guest 字段都会改变后续偏移。

```
gs_complex_t (packed, alignas 4096):

  0x0000  slots[256]                      256 × 8   = 2048 B
  0x0800  tokens[256]                     256 × 24  = 6144 B
  0x2000  gdt[6]                          6 × 8     = 48 B
  0x2030  tss_descriptor                  1 × 16    = 16 B
  0x2040  tss                             1 × 104   = 104 B
  0x20A8  fpu_area[XSAVE_SIZE_MAX]        8192 B    (alignas(64) → 0x20C0 起步)
  0x40C0  local_ipi_complex               16 B      (alignas(64))
  0x40D0  padding[6]                      6 × 8     = 48 B
  0x4100  stacks_ptr                      8 B
  ──────────────────────────────
  sizeof ≈ 0x4108 → alignas(4096) → 0x5000
  GS_COMPLEX_STRIDE = 0x5000 (= 20 KB, 5 页)
```

<readers-note>
实际 `sizeof(gs_complex_t)` 由编译器产出为准。此表仅为撰写时的参考快照。
若要验证，读 `build/` 产物中的 `gs_complex_t` 调试信息或插入 `static_assert`。
</readers-note>

---

## 6. 硬件栈：per_processor_hardware_stack_t

硬件栈复合体是 TSS 和 FRED 定位栈指针的区域，与 `gs_complex_t` 并列分配，通过 `gs_complex_t::stacks_ptr` 引用。详细文档见：

> **[`per_processor_hardware_stack_design.md`](per_processor_hardware_stack_design.md)**

`gs_complex_t` v3 中只保留一个指针（`stacks_ptr`），不再内嵌硬件栈。分配、布局、映射、CR3 重建的完整内容参见独立文档。

---

## 7. 分配详情

### 7.1 conjunc_GSs

| 属性 | 值 |
|------|-----|
| 总字节 | `logical_processor_count × GS_COMPLEX_STRIDE` |
| 分配器 | `page_allocator::available_meminterval_probe_keep()` |
| 页状态 | `page_state_t::kernel_persisit` |
| 清零 | Phase 3b `ksetmem_8` |
| 映射 | KMMU `map()` 全区间 WB+RW |
| stride | `GS_COMPLEX_STRIDE = (sizeof(gs_complex_t) + 4095) & ~4095ULL` |

### 7.2 per_processor_hardware_stack_t (hdstacks)

| 属性 | 值 |
|------|-----|
| 总物理字节 | `logical_processor_count × sizeof(per_processor_hardware_stack_t) + 4096 (tail guard)` |
| 分配器 | `page_allocator::available_meminterval_probe_keep()` |
| 页状态 | `page_state_t::kernel_persisit` |
| 映射 | 逐处理器逐段映射——跳过 5 个 guard 页 |
| 栈底偏移 | 通过 `offsetof` + 栈大小 - `RED_ZONE (0x40)` 计算 |

### 7.3 关键 Phase 序列

```
Phase 3b:
  1. 分配 conjunc_GSs (区间)
  2. 分配 hdstacks (区间)
  3. 逐处理器逐段映射栈页
  4. 设置 stacks_ptr 指向各处理器在 hdstacks 中的实例
  5. 设置 slots[PROCESSOR_RSP0_STACK_BTM_IDX]

Phase 4:
  6. arch_info 嵌入信息包

Phase 4.5:
  7. 在恒等映射下（pbase == vbase）填充所有处理器的 GDT/TSS
  8. wrmsr BSP 的 IA32_GS_BASE
  9. gs_complex_load_gdt_tss(BSP)
  10. shift_kernel → kernel.elf

kernel.elf very_early_init:
  11. conjucnt_GSs = arch->conjunc_GSs

kernel.elf APs_bringup:
  12. 第一遍：分配 g_gs_by_apicid 映射表
  13. 第二遍：写 processor_id 到 slot, 建立 apicid→gs_complex_t 映射

kernel.elf ap_bootstrap_init (各 AP):
  14. g_gs_by_apicid[x2apicid] → self
  15. wrmsr IA32_GS_BASE = self
  16. gs_complex_load_gdt_tss(self)

kernel.elf mem_init:
  17. simp_pages_set → 持久化两个区间
  18. Kspace_phyaddr_direct_map → CR3 重建映射
```

### 7.4 gs_complex_load_gdt_tss 语义

仅做 LGDT + LTR，**不设置 TSS 栈指针**。TSS 栈指针在 Phase 4.5 已由 init_init.cpp 通过 C++ `offsetof` 计算填入所有处理器的 tss 成员中。`gs_complex_load_gdt_tss` 从 gs_complex_t 读取已填入的描述符，构造 GDTR，然后调用 asm 原语装载。

---

## 8. v2 vs v3 关键差异对照

### 8.1 设计理念

| 维度 | v2 | v3 |
|------|----|----|
| 结构体角色 | 精心设计的固定布局 | **ABI 容器**，只保证 `slots[256]` |
| 偏移管理 | 手算 hex + `_pad0`/`_pad1` 显示垫片 | **编译器决定** — `packed` + `alignas`，用 `offsetof` 验证 |
| `[gs:offset]` | 期望使用 | **禁区**（slots 除外） |
| 跨核访问 | 隐含 | 显式为 `conjunc_GSs[pid]` 数组 |
| AP 自引用 | `conjunc_GSs 计算` | **g_gs_by_apicid 映射表** |

### 8.2 struct 布局

| 差异 | v2 | v3 |
|------|----|----|
| `dispatch` | `hard_interrupt_func_t[256]` (8B/call = 2KB) | `interrupt_token_t[256]` (24B/token = 6KB) |
| `local_ipi_complex` | 无 | 有 (__uint128_t, alignas(64)) |
| `padding [6]` | 无 | 有 (对齐至 64B，umwait 监视用) |
| `_pad0` / `_pad1` | 有 (显式推到位) | 无 (编译器自动布局) |
| per-CPU stride | 0x14000 (20 pages) | 0x5000 (5 pages) (由编译器产出决定) |
| sizeof 声明 | ≈ 76 KB | 约 20 KB (编译器决定) |

### 8.3 硬件栈独立化

v2 将 `per_processor_hardware_stack_t` 作为 `gs_complex_t` 文档的子章节描述。v3 将其**独立成专门文档**（`per_processor_hardware_stack_design.md`），因为：
- 硬件栈是 TSS/FRED 架构强制要求的区域，独立于 GS 元数据体系
- v2 的栈大小声明存在多处偏差（DF/MC/NMI 实际为 32KB 而非 v2 声明的 8~12KB）
- Guard 页的设计、分配、CR3 重建映射需要专门的详述

`gs_complex_t` 只在末尾保留一个 `stacks_ptr` 指针指向对应实例。

### 8.4 布局 vs ABI

```
v2: 结构体布局 = ABI。手算 offset，手动 _pad，每个人都要知道自己字段在哪。
v3: 结构体布局 ≠ ABI。slots 是 ABI，其他字段通过 C++ 成员访问，
    偏移只是编译器当前输出的快照，不是契约。
```

### 8.5 gs_complex_t 结构体伪代码对比

**v2（伪代码 + 手算偏移）**：
```cpp
struct gs_complex_t {
    uint64_t slots[256];                    // 0x0000 (2 KB)
    hard_interrupt_func_t dispatch[256];   // 0x0800 (2 KB)
    x64_gdtentry        gdt[6];             // 0x1000
    TSSDescriptorEntry  tss_descriptor;     // 0x1030
    TSSentry            tss;                // 0x1040
    uint8_t             _pad0[...];         // 推算填充
    uint8_t             fpu_area[...];      // 0x10C0
    uint8_t             _pad1[...];         // 推到 0x4000
    per_processor_hardware_stack_t* stacks_ptr;  // 0x4000
};
// sizeof ≈ 76 KB, stride = 0x14000
```

**v3（实际代码）**：
```cpp
struct __attribute__((packed)) alignas(4096) gs_complex_t {
    uint64_t slots[256];                     // ABI 部分
    interrupt_token_t tokens[256];
    x64_gdtentry        gdt[6];
    TSSDescriptorEntry  tss_descriptor;
    TSSentry            tss;
    alignas(64) uint8_t fpu_area[XSAVE_SIZE_MAX];
    alignas(64) __uint128_t local_ipi_complex;
    uint64_t padding[6];
    per_processor_hardware_stack_t* stacks_ptr;
};
// sizeof % 4096 == 0, GS_COMPLEX_STRIDE = (sizeof + 4095) & ~4095
```

### 8.6 启动时序关键变化

v2 时序中 AP 自引用走 `conjunc_GSs.vbase + proc_id × stride` + `wrmsr`。v3 中 AP 自引用走 **两步**：

1. `g_gs_by_apicid[x2apicid]` —— 查表得到 `gs_complex_t*`
2. `wrmsr(IA32_GS_BASE, self)` —— 设置 GS

映射表在 kernel.elf `APs_bringup` 阶段动态分配并填充，不在 init.elf Phase 3b 阶段。

---

## 9. 验证点（v3 更新）

| # | 验证 | 方法 |
|---|------|------|
| 1 | `sizeof(gs_complex_t) % 4096 == 0` | `static_assert` |
| 2 | `fpu_area` 64B 对齐 | `static_assert(offsetof(gs_complex_t, fpu_area) % 64 == 0)` |
| 3 | 栈页映射、guard 页不映射 | mem_init 后验证缺页 |
| 4 | TSS 栈指针指向各栈顶 -0x40 | 打印对比 |
| 5 | `self->tokens[vec].func(&self->tokens[vec])` 可执行 | 中断触发验证 |
| 6 | BSP/AP 的 `IA32_GS_BASE` 指向各自的 `gs_complex_t` | 读取 MSR 对比 |
| 7 | `g_gs_by_apicid[x2apicid]` 正确映射 | BSP 打印对比 |
| 8 | `stacks_ptr` 在 Phase 3b 设置无误 | Phase 4.5 填充前验证 |

---

## 10. 相关文件索引

| 文件 | 角色 | v3 变更说明 |
|------|------|------------|
| `Docs/arch/x86_64/conjunc_GSs_design_v3.md` | 本文档 | 全新 |
| `src/include/arch/x86_64/abi/GS_complex.h` | gs_complex_t + per_processor_hardware_stack_t 定义 | v3 实现（packed + alignas，含 local_ipi_complex 等） |
| `src/include/arch/x86_64/abi/GS_Slots_index_definitions.h` | slot 索引定义 | 不变 |
| `src/include/arch/x86_64/boot.h` | x86_specify_init_to_kernel_info | conjunc_GSs vm_interval 字段 |
| `src/include/arch/x86_64/Interrupt_system/loacl_processor.h` | 栈大小常量、GDT/TSS entry 定义 | 栈大小实际值 |
| `src/init/init_init.cpp` (phase_3b) | conjunc_GSs + hdstacks 分配 | — |
| `src/init/init_init.cpp` (phase_45_finalize) | GDT/TSS 填充 | — |
| `src/init/info_fill.cpp` | 转运 | — |
| `src/arch/x86_64/Processor/gs_complex_load_gdt_tss.cpp` | LGDT+LTR 入口 | 不变 |
| `src/arch/x86_64/Processor/runtime_processor_regist.asm` | LGDT/retfq/LTR asm 原语 | 不变 |
| `src/arch/x86_64/Processor/APs_bringup.cpp` | `g_gs_by_apicid` 映射表建立 | v3 新增 |
| `src/arch/x86_64/Processor/local_processor_manage.cpp` | AP 自引导（查表 + wrmsr） | v3 新增 |
| `src/arch/x86_64/boot/kinit.cpp` | `very_early_init` | 读取 `conjunc_GSs`、建立 `hw_stacks` |
| `src/arch/x86_64/boot/mem_init.cpp` | 持久化 + CR3 重建 | conjunc_GSs 走 `aff_entry` 列表，hw_stacks 走专用逐栈路径 |
| `src/arch/x86_64/Interrupts/x86_vecs_deliver_mgr.cpp` | 中断 token 分配 + 跨核查表 | 使用 `conjucnt_GSs[pid]` 访问 |
| `Docs/arch/x86_64/per_processor_hardware_stack_design.md` | 硬件栈复合体独立文档 | 本文档不包含硬件栈细节 |

## 11. 约定速查

```
✅ 正确做法：
   complex->slots[PROCESSOR_ID_GS_INDEX] = packed_id;     // slot ABI
   complex->tss.rsp0 = stack_bottom;                       // 成员访问
   complex->tokens[vec].func(&complex->tokens[vec]);       // 成员访问
   uintptr_t self = rdmsr(IA32_GS_BASE);                   // 自引用
   gs_complex_t* target = conjucnt_GSs.vbase() + pid * GS_COMPLEX_STRIDE; // 跨核
   gs_complex_t* self = g_gs_by_apicid[x2apicid];          // AP 引导

❌ 错误做法：
   *(uint64_t*)(gs_base + 0x1040) = ...;               // 手算偏移
   #define TSS_OFFSET 0x1040                            // 定义 hex 偏移常数
   "[gs:0x800]"                                         // asm 硬编码 offset
```

---

_本文档描述的设计哲学和分配/索引模型是稳定的 ABI 契约，但具体布局偏移不是。对于任何对布局偏移的依赖，请使用 `offsetof` + `static_assert` 验证，而非硬编码 hex。_
