# per_processor_hardware_stack_t — 硬件栈复合体

## 0. 文档定位

`per_processor_hardware_stack_t` 的独立文档。它与 `gs_complex_t` 的关系是：

```
gs_complex_t                per_processor_hardware_stack_t
  ┌──────────────┐              ┌──────────────────┐
  │ slots[256]   │              │ guard1     4KB   │ ← 未映射
  │ tokens[256]  │              │ stack_rsp0 32KB  │ ← 映射
  │ gdt/tss      │  ──stack──►  │ guard2     4KB   │ ← 未映射
  │ fpu_area     │    s_ptr     │ stack_ist1 32KB  │ ← 映射
  │ local_ipi    │              │ ...               │
  │ stacks_ptr ──┘              └──────────────────┘
```

硬件栈复合体不是元数据，它是 **x86_64 架构硬件强制要求的栈区域**——TSS 的 IST 指针和 rsp0 都指向这里。FRED 模式下，`IA32_FRED_RSP0~3` 同样指向此区域。guard 页隔离各栈，捕获溢出。

---

## 1. 与 gs_complex_t 的本质区别

`per_processor_hardware_stack_t` 和 `gs_complex_t` 是两个**性质相反**的结构体：

| 维度 | `gs_complex_t` | `per_processor_hardware_stack_t` |
|------|---------------|----------------------------------|
| 本质 | **ABI 容器** | **用途明确单一的硬件结构** |
| 布局 | 编译器决定 | 结构体固定、预期稳定 |
| ABI 范围 | 仅 `slots[256]` 是 ABI，其余是注入的 guest 字段 | **整个结构体就是 ABI**——硬件（TSS/FRED）的栈指针依赖精确的每字节位置 |
| offset 访问 | **禁止**直接算偏移（slots 除外） | **允许且稳定**——`offsetof` 结果是硬件契约的一部分 |
| 变动容忍度 | 增删 guest 字段是日常操作，不影响 ABI | 任何字段变动都会级联影响 TSS 栈指针、FRED RSP 配置、映射代码 |
| 大小 | 由 `alignas(4096)` 吸入到 4K 倍数 | 由 `sizeof` 精确决定，包含 guard 页 |

**一句话**：`gs_complex_t` 是各子系统往里塞东西的容器；`per_processor_hardware_stack_t` 是硬件往里取东西的固定结构。

> 这意味着 `per_processor_hardware_stack_t` 中可以直接使用 `offsetof` 硬编码偏移访问（例如映射代码中的 `RSP0_BASE_OFF`），这些偏移在可见未来是稳定的。而 `gs_complex_t` 中的任何非 slot 字段都必须通过 `->` 成员访问，假设其 hex 偏移是脆弱的。

---

## 2. 成型史

### 2.1 v1：嵌入 gs_complex_t 末尾

最早设计将栈区域直接内嵌在 `gs_complex_t` 末尾，偏移约 0x4000–0x17000。结构体大致为：

```
gs_complex_t v1:
  [0x0000] slots[256]     2 KB
  [0x0800] 其他元数据     ...
  [0x4000] stack_rsp0     32 KB
  [0xC000] stack_ist1     8 KB
  ...
  sizeof ≈ 76 KB
```

**问题**：要在每个栈之间插入 4KB guard 页捕获溢出，结构体布局剧烈变化：

```
// 加 guard 后的内嵌布局
  [0x4000] guard1        4 KB
  [0x5000] stack_rsp0   32 KB
  [0xD000] guard2        4 KB
  [0xE000] stack_ist1    8 KB
  ...
```

这导致：
- `gs_complex_t` 的总大小被 guard 页大幅推高（每处理器约 76KB → ~180KB）
- guard 页不映射但占用地址空间，`vm_interval` 区间内有空洞，CR3 重建时需要逐段处理
- 元数据访问路径（中断分发、slot 查表）与栈无关，耦合在一起没有意义

### 2.2 v2：分离硬件栈

**决定**：将硬件栈独立为 `per_processor_hardware_stack_t`，与 `gs_complex_t` 并列分配。

分离后的好处：

| 优势 | 说明 |
|------|------|
| **Guard 页自由** | guard 页不会搞乱元数据区布局。栈的内部结构独立开发 |
| **KMMU 映射灵活** | 元数据一次连续映射；栈需要逐段映射（跳过 guard），两者互不干扰 |
| **区间语义清晰** | 元数据与栈分属不同 `vm_interval`，CR3 重建时各走各的路径 |
| **零额外分配** | 所有 per-CPU 栈在 Phase 3b 一次分配完毕 |
| **AP 无 alloc** | AP 醒来直接使用已就绪的栈，不依赖任何分配器 |
| **深度睡眠恢复** | 丢弃架构状态后重新 LGDT + LTR 即可 |

`gs_complex_t` 中只保留一个指针 `stacks_ptr` 指向对应的 `per_processor_hardware_stack_t` 实例。

### 2.3 栈大小的演进（FRED 驱动力）

栈大小常量的历史沿革（`loacl_processor.h`）：

| 阶段 | 提交 | RSP0 | DF | MC | NMI | BP/DBG |
|------|------|------|----|----|----|--------|
| 原始（`b485546`） | Refactor memory management | 0x8000 | **0x2000** | **0x3000** | **0x3000** | 0x3000 |
| FRED 统一（`602f4e7`） | Implement atomic 16B CAS + refactor interrupt | 0x8000 | **0x8000** | **0x8000** | **0x8000** | 0x3000 |

DF/MC/NMI 从 8~12KB 统一提升到 32KB（与 rsp0 一致）。原因：

1. **FRED 取消了 IST 自动切换**——在传统 IDT 模式下，#DF 发生时 CPU 自动切换 IST 栈，切断异常链。FRED 模式下事件在**当前栈**上递送，需要更深的事件嵌套能力。
2. **栈复用深度增加**——一个栈可能承载多层事件嵌套（如 #PF → #DF → #MC → NMI 在同一栈上推进），不再像传统 TSS 那样每层切到独立栈。
3. **统一管理**——32KB 对 rsp0 来说足够大，对 IST 来说也足够安全，统一大小简化了跨处理器映射逻辑。

> FRED 模式的栈指针由 `IA32_FRED_RSP0~3` MSR 提供，但物理栈区域仍然是 `per_processor_hardware_stack_t` 中的这些栈段。栈大小的提升服务于 FRED 更深的嵌套需求。

---

## 3. 物理存在

### 3.1 分配（init.elf Phase 3b）

```cpp
// init_init.cpp Phase 3b — hardware stacks 分配
uint64_t stack_stride = sizeof(per_processor_hardware_stack_t);  // 含 5 guard 页
uint64_t total_phys   = logical_processor_count * stack_stride + 4096;  // + 尾 guard

iv.arch_info.hdstacks_interval_pbase = page_allocator::available_meminterval_probe_keep(...);
iv.arch_info.hdstacks_interval_vbase = va_alloc_up(total_virt, 12);
iv.arch_info.hdstacks_4kbpgs_count   = total_phys >> 12;
```

### 3.2 传递路径

```
init.elf (Phase 3b)
  ──→ iv.arch_info.{hdstacks_interval_pbase, hdstacks_interval_vbase, hdstacks_4kbpgs_count}
      ──→ info_fill.cpp embed → arch_specify info packet
          ──→ kernel.elf very_early_init: hw_stacks = {arch->hdstacks_interval_*}
              ──→ mem_init.cpp CR3 重建
```

在 kernel.elf 端 `hw_stacks` 是全局 `vm_interval`：

```cpp
// kinit.cpp
extern vm_interval hw_stacks;  // 定义在 mem_init.cpp

// very_early_init
hw_stacks.vpn      = arch->hdstacks_interval_vbase >> 12;
hw_stacks.ppn      = arch->hdstacks_interval_pbase >> 12;
hw_stacks.npages   = arch->hdstacks_4kbpgs_count;
```

### 3.3 文件清单

| 文件 | 角色 |
|------|------|
| `init/init_init.cpp` (Phase 3b) | 分配 + 逐栈映射 + 设 stacks_ptr |
| `init/info_fill.cpp` | 转运至信息包 |
| `boot/kinit.cpp` | `very_early_init` 读取 `hw_stacks` |
| `boot/mem_init.cpp` | 持久化 + CR3 重建逐栈映射 |
| `Processor/local_processor_manage.cpp` | AP 引到栈上 |

---

## 4. 结构体布局

### 4.1 per_processor_hardware_stack_t

```cpp
struct per_processor_hardware_stack_t {
    uint8_t guard1[4096];                // 0x0000 — 未映射
    uint8_t stack_rsp0[RSP0_STACKSIZE];  // 0x1000 — CPL3→0 核心栈
    uint8_t guard2[4096];                // 未映射
    uint8_t stack_ist1[DF_STACKSIZE];    // Double Fault 栈
    uint8_t guard3[4096];                // 未映射
    uint8_t stack_ist2[MC_STACKSIZE];    // Machine Check 栈
    uint8_t guard4[4096];                // 未映射
    uint8_t stack_ist3[NMI_STACKSIZE];   // NMI 栈
    uint8_t guard5[4096];                // 未映射
    uint8_t stack_ist4[BP_DBG_STACKSIZE];// Breakpoint/Debug 栈
};
```

### 4.2 4K 铁律：为什么结构体必须精确对齐

`per_processor_hardware_stack_t` 的精确布局只有一个目的：**让 MMU 成为最好的栈溢出探测器**。

策略：guard 页在页表中**不建立 PTE**（not-present），访问时直接触发 #PF。这意味着：

1. 每个 guard 页必须独占一个页框（否则会误伤相邻栈段）
2. 每个栈段必须从页边界开始（否则映射边界会切进 guard 页）
3. 整个结构体大小必须是 4K 倍数（否则 stride 内会有错位）

因此有以下**不可打破的铁律**：

| 铁律 | 要求 | 违反后果 |
|------|------|---------|
| `sizeof(per_processor_hardware_stack_t) % 4096 == 0` | 总大小 4K 对齐 | stride 不对齐 → 处理器 N 的 guard1 可能与处理器 N-1 的栈段共享页面 → 无法独立设置 not-present |
| 分配基址 % 4096 == 0 | 页对齐 | 同上 |
| 每个 guard 字段 offset % 4096 == 0, size % 4096 == 0 | 页对齐且页大小倍数 | guard 无法独立为 not-present PTE |
| 每个栈段字段 offset % 4096 == 0, size % 4096 == 0 | 页对齐且页大小倍数 | 栈段映射会跨入 guard 页范围 |

**在满足这些铁律的前提下，各字段大小可以调整**。例如将 `stack_rsp0` 从 32KB 增加到 64KB，只要保持 4K 倍数即可（会增加 `sizeof` 和 stride，但这只是资源问题）。

> 当前布局满足铁律的验证：
> - `guard1` at offset 0x0000, size 0x1000 ✓
> - `stack_rsp0` at offset 0x1000, size 0x8000 ✓
> - `guard2` at offset 0x9000, size 0x1000 ✓
> - `stack_ist1` at offset 0xA000, size 0x8000 ✓
> - 依此类推，每段都以 0x1000（4KB）为步长排列 ✓

### 4.3 栈大小常量（当前值）

| 栈 | 大小 | TSS 字段 | FRED MSR | 用途 |
|----|------|---------|----------|------|
| `stack_rsp0` | 0x8000 (32 KB) | `tss.rsp0` | `IA32_FRED_RSP0` | CPL3→0 异常级切换时自动加载到 RSP |
| `stack_ist1` | 0x8000 (32 KB) | `tss.ist[1]` | `IA32_FRED_RSP1` | Double Fault (#DF) |
| `stack_ist2` | 0x8000 (32 KB) | `tss.ist[2]` | `IA32_FRED_RSP2` | Machine Check (#MC) |
| `stack_ist3` | 0x8000 (32 KB) | `tss.ist[3]` | `IA32_FRED_RSP3` | NMI |
| `stack_ist4` | 0x3000 (12 KB) | `tss.ist[4]` | — | Breakpoint (#BP) / Debug (#DB) |

定义位置：`src/include/arch/x86_64/Interrupt_system/loacl_processor.h`

### 4.4 栈底偏移常数与访问约定

#### 访问约定

- **推荐路径**：通过 C++ 高级语言结构体指针访问（`complex->tss.rsp0`、`st->stack_rsp0[offset]`）
- **汇编直接偏移**：理论上可以通过汇编硬编码偏移量直接寻址，但**不保证一致性**——编译器可能因不同版本或架构选项改变 packed 排列的中间字段偏移
- **映射代码例外**：`offsetof` 计算的 `RSP0_BASE_OFF` 等常量仅用于 KMMU 逐段映射（映射代码必须知道物理位移以建立页表），这不是运行时数据访问，而是启动时一次性的页表操作

#### 栈底计算

```cpp
constexpr uint64_t RED_ZONE        = 0x40;
constexpr uint64_t RSP0_BOTTOM_OFF = RSP0_BASE_OFF + RSP0_STACKSIZE - RED_ZONE;
```

栈在 x86_64 中**向下增长**（向低地址方向）。所以：

```
低地址
  │
  │  stack_rsp0 基址 (RSP0_BASE_OFF)
  ▼  ┌──────────────────────────┐
     │ 未使用的栈空间            │
     │                          │
     │     ...                  │
     │                          │
  ↑  ├──────────────────────────┤ ← RSP0_BOTTOM_OFF （初始 RSP）
  │  │ RED_ZONE (64B 安全裕量)  │
高地址 └──────────────────────────┘ ← RSP0_BASE_OFF + RSP0_STACKSIZE
```

`RSP0_BOTTOM_OFF` 是**初始 RSP 指向的地址**，相对栈基址的偏移。因为栈向下增长，初始 RSP 设在栈区间的**顶部（最高地址）**减去 RED_ZONE：

$$\text{RSP}_{\text{init}} = \text{stack\_base} + \text{stack\_size} - \text{RED\_ZONE}$$

`RED_ZONE = 0x40 (64 bytes)` 的选择依据：

| 压栈内容 | 字节数 |
|---------|-------|
| SS（x86_64 长模式中断门自动压栈） | 8 |
| RSP | 8 |
| RFLAGS | 8 |
| CS | 8 |
| RIP | 8 |
| 错误码（#PF、#DF、#MC 等异常可选） | 8 |
| **硬件帧小计** | **最多 48** |
| 安全余量 | 16 |
| **RED_ZONE 总计** | **64 (0x40)** |

0x40 保证在第一个中断/异常帧被压栈时（40~48 字节），RSP 不会回卷超出栈页边界触发 #PF。16 字节的余量给软件处理程序可能的少量现场保存留出空间。

所有 IST 栈使用相同的计算方式：

```cpp
constexpr uint64_t IST1_BOTTOM_OFF = IST1_BASE_OFF + DF_STACKSIZE - RED_ZONE;
constexpr uint64_t IST2_BOTTOM_OFF = IST2_BASE_OFF + MC_STACKSIZE - RED_ZONE;
constexpr uint64_t IST3_BOTTOM_OFF = IST3_BASE_OFF + NMI_STACKSIZE - RED_ZONE;
constexpr uint64_t IST4_BOTTOM_OFF = IST4_BASE_OFF + BP_DBG_STACKSIZE - RED_ZONE;
```

硬件在 TSS 中加载的 `rsp0` / `ist[N]` 直接指向 `BOTTOM_OFF` 对应的地址，第一压栈帧落在 RED_ZONE 范围内。

### 4.5 区间内存布局

```
hw_stacks.vbase (页对齐)
  │
  ├── per_processor_hardware_stack_t[0] (BSP)
  │   ┌──────────────────┐
  │   │ guard1      4KB  │  未映射
  │   ├──────────────────┤
  │   │ stack_rsp0 32KB  │  映射 (WB+RW)
  │   ├──────────────────┤
  │   │ guard2      4KB  │  未映射
  │   ├──────────────────┤
  │   │ stack_ist1 32KB  │  映射
  │   ├──────────────────┤
  │   │ guard3      4KB  │  未映射
  │   ├──────────────────┤
  │   │ stack_ist2 32KB  │  映射
  │   ├──────────────────┤
  │   │ guard4      4KB  │  未映射
  │   ├──────────────────┤
  │   │ stack_ist3 32KB  │  映射
  │   ├──────────────────┤
  │   │ guard5      4KB  │  未映射
  │   ├──────────────────┤
  │   │ stack_ist4 12KB  │  映射
  │   └──────────────────┘
  │   stride = sizeof(per_processor_hardware_stack_t) ≈ 160 KB
  │
  ├── per_processor_hardware_stack_t[1] (AP #1)
  ├── ...
  └── per_processor_hardware_stack_t[N-1] (AP #N-1)
      └── tail_guard 4KB  未映射
```

---

## 5. 实际加载链

### 5.1 Phase 3b：分配 + 映射

```cpp
// init_init.cpp Phase 3b
for (uint32_t p = 0; p < logical_processor_count; p++) {
    uint64_t  proc_off = p * stack_stride;
    phyaddr_t proc_p   = hdstacks_pbase + proc_off;
    vaddr_t   proc_v   = hdstacks_vbase + proc_off;

    // 只映射栈段，跳过 5 个 guard 页
    kmmu->map({proc_p + RSP0_BASE_OFF, proc_v + RSP0_BASE_OFF, RSP0_STACKSIZE}, ...);
    kmmu->map({proc_p + IST1_BASE_OFF, proc_v + IST1_BASE_OFF, DF_STACKSIZE}, ...);
    kmmu->map({proc_p + IST2_BASE_OFF, proc_v + IST2_BASE_OFF, MC_STACKSIZE}, ...);
    kmmu->map({proc_p + IST3_BASE_OFF, proc_v + IST3_BASE_OFF, NMI_STACKSIZE}, ...);
    kmmu->map({proc_p + IST4_BASE_OFF, proc_v + IST4_BASE_OFF, BP_DBG_STACKSIZE}, ...);

    // 在 gs_complex_t 中设 stacks_ptr 和 rsp0 slot
    gs_complex_t* complex = ...;
    complex->slots[PROCESSOR_RSP0_STACK_BTM_IDX] = proc_v + RSP0_BOTTOM_OFF;
    complex->stacks_ptr = (per_processor_hardware_stack_t*)proc_v;
}
```

关键设计：
- 不映射 guard 页 → 访问 guard 页触发 #PF，捕获栈溢出
- 不清零栈区间 → 仅写入栈底指针，栈内容按需使用
- 所有处理器一次分配完毕

### 5.2 Phase 4.5：TSS 栈指针填充

```cpp
// init_init.cpp Phase 4.5
for (uint32_t p = 0; p < pcount; p++) {
    gs_complex_t* cx = (gs_complex_t*)(gs_base + p * GS_COMPLEX_STRIDE);
    per_processor_hardware_stack_t* st = cx->stacks_ptr;
    vaddr_t st_va = (vaddr_t)st;

    cx->tss.rsp0   = st_va + RSP0_BOTTOM_OFF;
    cx->tss.ist[1] = st_va + IST1_BOTTOM_OFF;
    cx->tss.ist[2] = st_va + IST2_BOTTOM_OFF;
    cx->tss.ist[3] = st_va + IST3_BOTTOM_OFF;
    cx->tss.ist[4] = st_va + IST4_BOTTOM_OFF;
}
```

### 5.3 Phase 4.5：BSP 跳转用栈

```cpp
// init_init.cpp Phase 4.5 — jump_to_kernel
gs_complex_t* bsp = ...;
vaddr_t bsp_rsp0 = bsp->tss.rsp0;
// 用 bsp_rsp0 作为跳入 kernel.elf 的初始 RSP
```

kernel.elf 入口的 `kernel_start` 函数的第一个栈帧就位于 BSP 的 `stack_rsp0` 顶部。

### 5.4 kernel.elf very_early_init：接收

```cpp
// kinit.cpp
void very_early_init(init_to_kernel_header* transfer) {
    x86_specify_init_to_kernel_info* arch = ...;
    hw_stacks.vpn    = arch->hdstacks_interval_vbase >> 12;
    hw_stacks.ppn    = arch->hdstacks_interval_pbase >> 12;
    hw_stacks.npages = arch->hdstacks_4kbpgs_count;
}
```

### 5.5 kernel.elf mem_init：CR3 重建映射

CR3 刷新后，Phase 3b 建立的映射丢失。`kimg_affiliate_property_map1` 按照完全相同的逐段策略重建映射：

```cpp
// mem_init.cpp
for (uint32_t p = 0; p < logical_processor_count; p++) {
    uint64_t  off    = p * stack_stride;
    vaddr_t   proc_v = hw_v + off;
    phyaddr_t proc_p = hw_p + off;

    // 相同的 5 个 map 调用，跳过 guard 页
    Kspace_phyaddr_direct_map(RSP0_BASE_OFF, RSP0_STACKSIZE);
    Kspace_phyaddr_direct_map(IST1_BASE_OFF, DF_STACKSIZE);
    Kspace_phyaddr_direct_map(IST2_BASE_OFF, MC_STACKSIZE);
    Kspace_phyaddr_direct_map(IST3_BASE_OFF, NMI_STACKSIZE);
    Kspace_phyaddr_direct_map(IST4_BASE_OFF, BP_DBG_STACKSIZE);
}
```

`hw_stacks` 区间本身不纳入 `aff_entry` 列表做整体映射——因为整体映射会连 guard 页一起建页表，破坏溢出检测。它走专用路径逐个栈段映射。

### 5.6 AP 引导：直接用栈

```cpp
// local_processor_manage.cpp
ap_bootstrap_init():
    self = g_gs_by_apicid[x2apicid];
    wrmsr(IA32_GS_BASE, self);
    gs_complex_load_gdt_tss(self);
    return self->tss.rsp0;   // ← 返回本核的 rsp0 栈底
```

asm 跳板将返回值设为 RSP，AP 从 Phase 4.5 已填好的栈顶开始执行。**零分配、零构造**。

### 5.7 完整启动链

```
init.elf Phase 3b:
  1. sizeof(per_processor_hardware_stack_t) × N + tail guard → 计算总大小
  2. available_meminterval_probe_keep → 物理分配
  3. va_alloc_up → 虚拟区间
  4. 逐处理器、逐栈段映射（跳过 5 guard 页 + tail guard）
  5. 每个 gs_complex_t::stacks_ptr = 对应 per_processor_hardware_stack_t*
  6. slots[0] = rsp0 栈顶地址
    ↓
init.elf Phase 4:
  7. arch_info 嵌入信息包
    ↓
init.elf Phase 4.5:
  8. 填充 TSS 的 rsp0、ist[1..4]（指向各栈顶 - 0x40）
  9. wrmsr BSP IA32_GS_BASE
  10. gs_complex_load_gdt_tss(BSP) → LGDT+LTR 使 TSS 生效
  11. BSP 的 tss.rsp0 作为 jump_to_kernel 栈
    ↓
kernel.elf very_early_init:
  12. hw_stacks = arch->hdstacks_interval_*
  13. BSP 继续使用 Phase 4.5 设好的栈
    ↓
kernel.elf mem_init (kimg_affiliate_property_map1):
  14. 逐处理器逐栈段重建页表映射（镜像 Phase 3b）
  15. CR3 load → 新地址空间生效
    ↓
kernel.elf APs_bringup:
  16. g_gs_by_apicid[apicid] 映射表 → self
  17. wrmsr IA32_GS_BASE + gs_complex_load_gdt_tss
  18. self->tss.rsp0 → 栈就绪
```

---

## 6. 与 TSS 的关系

x86_64 硬件在事件分发时自动从 TSS 加载栈指针：

| 事件类型 | TSS 字段 | 谁设置 | 指向位置 |
|---------|---------|--------|---------|
| CPL 变更（CPL3→0） | `tss.rsp0` | Phase 4.5 | `stack_rsp0` 顶 - 0x40 |
| Double Fault (#DF) | `tss.ist[1]` | Phase 4.5 | `stack_ist1` 顶 - 0x40 |
| Machine Check (#MC) | `tss.ist[2]` | Phase 4.5 | `stack_ist2` 顶 - 0x40 |
| NMI | `tss.ist[3]` | Phase 4.5 | `stack_ist3` 顶 - 0x40 |
| Breakpoint/Debug | `tss.ist[4]` | Phase 4.5 | `stack_ist4` 顶 - 0x40 |

`gs_complex_load_gdt_tss` 只加载 GDT 和 LTR，**不设置 TSS 栈指针**——这些在 Phase 4.5 由 C++ `offsetof` 计算填入。

TSS 描述符中的基址（GS 复合体中的 `tss_descriptor.base0~3`）指向 `gs_complex_t::tss` 成员，由 Phase 4.5 填入。

---

## 7. 与 FRED 的关系

### 7.1 FRED 栈模型

在支持 FRED（Flexible Return and Event Delivery）的处理器上：

- FRED **不使用** TSS 中的 IST 栈指针
- 栈指针来源从 `TSS.ist[]` 变为 `IA32_FRED_RSP0~3` MSR
- `IA32_FRED_RSP0` —— CPL3→0 的常规栈（等效 TSS.rsp0）→ 使用 `stack_rsp0`
- `IA32_FRED_RSP1` —— FRED 事件栈 1（等效 TSS.ist[1]）→ 使用 `stack_ist1`
- `IA32_FRED_RSP2` —— FRED 事件栈 2（等效 TSS.ist[2]）→ 使用 `stack_ist2`
- `IA32_FRED_RSP3` —— FRED 事件栈 3（等效 TSS.ist[3]）→ 使用 `stack_ist3`

物理栈区域不变——`per_processor_hardware_stack_t` 中的各段，FRED 和 TSS 模式下都是从相同的物理栈区域加载。

### 7.2 FRED 对栈大小的驱动力

栈大小从 8~12KB 统一到 32KB（`602f4e7`）的核心原因是 FRED 的**事件递送模型**：

| 特性 | 传统 TSS/IDT 模式 | FRED 模式 |
|------|------------------|-----------|
| 栈切换 | 硬件自动 IST 切换，切断嵌套链 | **在当前栈上递送**，不自动切换 |
| 嵌套深度 | 浅（每层切不同栈） | **深**（同栈可能多层嵌套） |
| #DF 保护 | 切 IST1，#DF 有独立栈 | 若 FRED_RSP1 = FRED_RSP0 则同栈；即使分开仍可能在同一栈上积累 |
| 栈压力 | 每栈仅需覆盖单层异常 + 处理路径 | 单栈需覆盖多层嵌套 + 处理路径 |

DF/MC/NMI 栈从 8~12KB 提升到 32KB 是为了给 FRED 模式下可能的深嵌套事件链留足空间。`BP_DBG_STACKSIZE` 保持在 12KB（0x3000）——断点和调试事件通常不嵌套。

### 7.3 FRED 的 MSR 配置

```cpp
// 定义位置：include/arch/x86_64/abi/msr_offsets_definitions.h
msr::syscall::IA32_FRED_RSP0    = 0x1CC
msr::syscall::IA32_FRED_RSP1    = 0x1CD
msr::syscall::IA32_FRED_RSP2    = 0x1CE
msr::syscall::IA32_FRED_RSP3    = 0x1CF
msr::syscall::IA32_FRED_STKLVLS = 0x1D0   // 每向量栈等级
msr::syscall::IA32_FRED_CONFIG  = 0x1D4
```

FRED 启用通过 `fred_enable(gs_complex_t*)`，在 `kinit.cpp` 中 FRED 检测到支持后调用。

---

## 8. Guard 页设计与溢出检测

### 8.1 策略

| guard 页 | 位置 | 保护对象 | 未映射后果 |
|---------|------|---------|-----------|
| `guard1` | stack_rsp0 之下 | 防止 rsp0 栈向下溢出冲入相邻处理器 | 访问 → #PF → panic |
| `guard2` | rsp0 ↔ IST1 之间 | 防止栈误用、数据污染 | 同上 |
| `guard3` | IST1 ↔ IST2 之间 | 同上 | 同上 |
| `guard4` | IST2 ↔ IST3 之间 | 同上 | 同上 |
| `guard5` | IST3 ↔ IST4 之间 | 同上 | 同上 |
| tail guard | 最后一个处理器之后 | 防止最后一个栈向下溢出冲入其他数据 | 同上 |

### 8.2 注意事项

- Guard 页只是**物理不映射**，PTE 缺失。不占用实际物理页框
- 访问 guard 页触发 #PF 而非静默写穿，适合调试和 panic 分析
- 栈段本身（如 `stack_rsp0`）只有栈顶被写入，栈底区域也留出了溢出空间
- KMMU `map()` / `Kspace_phyaddr_direct_map()` 在传参时会按页面对齐，每个映射的栈段需满足 `size % 4096 == 0`（当前常量均满足）

---

## 9. 验证点

| # | 验证 | 方法 |
|---|------|------|
| 1 | 每个栈段页对齐 | `static_assert(offsetof(..., stack_rsp0) % 4096 == 0)` |
| 2 | Guard 页未映射 | 触发 #PF 时检查地址 |
| 3 | TSS 栈指针正确 | 打印 `tss.rsp0` 与计算值对比 |
| 4 | CR3 重建后映射仍在 | CR3 load 后写栈不 #PF |
| 5 | AP 引导致栈相同 | AP 打印 `self->tss.rsp0` 与 BSP 计算对比 |
| 6 | 栈底 = 基址 + 大小 - 0x40 | 手动验证每个 IST |
| 7 | `stacks_ptr` 在 Phase 3b 和 4.5 一致 | 对比两次写入值 |
| 8 | tail guard 确实未映射 | 超出最后一个栈 4KB 访问应触发 #PF |
| 9 | FRED_RSP MSR 指向正确栈 | FRED 启用后打印各 MSR 值 |

---

## 10. 相关文件索引

| 文件 | 角色 |
|------|------|
| `Docs/arch/x86_64/per_processor_hardware_stack_design.md` | 本文档 |
| `Docs/arch/x86_64/conjunc_GSs_design_v3.md` | `gs_complex_t` 容器文档（含 `stacks_ptr` 引用说明） |
| `include/arch/x86_64/abi/GS_complex.h` | `per_processor_hardware_stack_t` 定义 + 偏移常量 |
| `include/arch/x86_64/Interrupt_system/loacl_processor.h` | 栈大小常量 (`RSP0_STACKSIZE` 等)、TSS entry、GDTR 定义 |
| `include/arch/x86_64/abi/msr_offsets_definitions.h` | FRED_RSP0~3、FRED_CONFIG MSR 定义 |
| `include/arch/x86_64/boot.h` | `hdstacks_interval_*` 字段 |
| `init/init_init.cpp` (Phase 3b) | 分配 + 首次映射 + `stacks_ptr` 设置 |
| `init/init_init.cpp` (Phase 4.5) | TSS 栈指针填充 |
| `boot/kinit.cpp` | `very_early_init` 接收 `hw_stacks` |
| `boot/mem_init.cpp` | 持久化 + CR3 重建逐栈映射 |
| `Processor/local_processor_manage.cpp` | AP 引导（取用栈） |
| `Processor/gs_complex_load_gdt_tss.cpp` | LGDT + LTR（使 TSS 有效） |
