# conjunc_GSs — GS 复合体设计

## 1. GS 复合体理念

GS（全局段）在 x86_64 长模式下不再是传统段选择子，而是通过
`IA32_GS_BASE` / `IA32_KERNEL_GS_BASE` MSR 指向的一块线性地址区域。
实际上 GS 是一个**元数据区指针**——内核通过 `GS:offset` 访问每处理器数据。

**GS 复合体（GS Complex）** 是将 GS 视为结构化元数据区、
**slots + first-class 动态字段**的组合设计。

```
GS 不是"段"             → 是 per-CPU 复合体基址
SLOT_IS_SLOT 区          → slots[0..255]，每槽 uint64_t，按槽使用
动态区                    → 一等字段以 slot-aligned 偏移嵌入
硬件栈独立分配            → 见 per_processor_hardware_stack_t（含 guard 页）
```

### 为什么不是纯 slot 模型

纯 `uint64_t[]` 模型要求所有字段通过槽位的指针间接访问。对于热路径（中断分发），
多一次访存开销明显。动态区的一等字段通过 `GS_BASE + 结构体偏移` 直接命中，零间接。

### 设计演进：硬件栈从内嵌到分离

v1 设计中硬件栈内嵌在 `gs_complex_t` 末尾（偏移 0x4000–0x17000）。
v2 将其分离到独立的 `per_processor_hardware_stack_t`，原因：

1. **Guard pages** — 每个栈之间需要 4KB guard 页捕获栈溢出，内嵌会导致 gs_complex_t 布局剧烈变化
2. **KMMU 映射灵活性** — 栈需要逐段映射（跳过 guard），元数据区只需一次连续映射
3. **区间语义清晰** — 元数据与栈分属不同 vm_interval，便于 CR3 重建时按需映射

分离后的好处：
- **零额外分配** — 所有 per-CPU 栈在 Phase 3b 一次分配完毕
- **物理连续** — 元数据与栈双区间物理连续，TLB 友好
- **AP 无 alloc** — AP 醒来到处有栈用，不依赖 FPA/kpoolmemmgr
- **深度睡眠恢复友好** — 丢弃架构状态后重新 LGDT + LTR 即可

### 所有权

| 属性 | 说明 |
|------|------|
| 架构范围 | x86_64 独有 |
| 分配 | init.elf Phase 3b 预分配 |
| 生命周期 | boot → 全程常驻 |
| 栈大小 | 见 `loacl_processor.h` 常量，每处理器约 92KB（含 guard 页） |

## 2. 数据结构

### 2.1 gs_complex_t — 元数据复合体

```cpp
// 文件: src/include/arch/x86_64/abi/GS_complex.h
struct gs_complex_t {
    // ═══════════════════════════════════════════════════
    // SLOT_IS_SLOT 区  [0x0000, 0x0800)
    // ═══════════════════════════════════════════════════
    uint64_t slots[256];                     // 0x0000 — 0x0800 (2 KB)

    // ═══════════════════════════════════════════════════
    // 动态区  [0x0800, max)
    // ═══════════════════════════════════════════════════

    // 中断函数指针表（零间接分发）
    hard_interrupt_func_t dispatch[256];     // 0x0800 — 0x1000 (2 KB)

    // GDT + TSS 描述符 + TSS
    x64_gdtentry        gdt[6];              // 0x1000 — 0x1030
    TSSDescriptorEntry  tss_descriptor;      // 0x1030 — 0x1040
    TSSentry            tss;                 // 0x1040 — 0x10A8 (104 B)

    // 对齐 → fpu_area 64B 边界
    uint8_t             _pad0[0x10C0 - 0x10A8];  // 0x10A8 — 0x10C0 (24 B)

    // FPU/SIMD 暂存区 (XSAVE64/XRSTOR64)
    uint8_t             fpu_area[XSAVE_SIZE_MAX]; // 0x10C0 — 0x30C0 (8 KB)

    // 页对齐垫片
    uint8_t             _pad1[0x4000 - 0x30C0];   // 0x30C0 — 0x4000

    // ═══════════════════════════════════════════════════
    // 硬件栈指针  [0x4000)
    // 指向 per_processor_hardware_stack_t 实例（独立分配）
    // ═══════════════════════════════════════════════════
    per_processor_hardware_stack_t* stacks_ptr;
};
// sizeof ≈ 76 KB → stride = 0x14000 (20 pages)
```

### 2.2 per_processor_hardware_stack_t — 硬件栈（含 guard 页）

```cpp
// 文件: src/include/arch/x86_64/abi/GS_complex.h
struct per_processor_hardware_stack_t {
    uint8_t guard1[4096];          // 未映射，溢出捕获
    uint8_t stack_rsp0[RSP0_STACKSIZE];  // 32 KB — CPL3→0 核心栈
    uint8_t guard2[4096];          // 未映射
    uint8_t stack_ist1[DF_STACKSIZE];   //  8 KB — Double Fault 栈
    uint8_t guard3[4096];          // 未映射
    uint8_t stack_ist2[MC_STACKSIZE];   // 12 KB — Machine Check 栈
    uint8_t guard4[4096];          // 未映射
    uint8_t stack_ist3[NMI_STACKSIZE];  // 12 KB — NMI 栈
    uint8_t guard5[4096];          // 未映射
    uint8_t stack_ist4[BP_DBG_STACKSIZE]; // 12 KB — Breakpoint/Debug 栈
};
// sizeof ≈ 96 KB (含 5 × 4KB guard = 20KB guard, 72KB 有效栈)
```

### 2.3 boot.h 扩展字段

```cpp
// 文件: src/include/arch/x86_64/boot.h
struct x86_specify_init_to_kernel_info {
    vm_interval hpet_mmio;
    vm_interval conjunc_GSs;              // gs_complex_t 实例区间
    phyaddr_t   hdstacks_interval_pbase;  // per_processor_hw_stack 物理基址
    uint64_t    hdstacks_4kbpgs_count;    // 栈区间总页数
    vaddr_t     hdstacks_interval_vbase;  // 栈区间虚拟基址
    GlobalBasicGraphicInfoType gop_info;
    vaddr_t Gop_vbase;
    phyaddr_t XSDT_base;
};
```

### 辅助内联函数

Phase 4.5 直接使用 `offsetof` / `sizeof` 计算偏移量设置 TSS 栈指针，无需专用填充函数：

```cpp
// gs_complex_t 初始化（Phase 4.5 对每个处理器执行）
complex->slots[PROCESSOR_RSP0_STACK_BTM_IDX] = st_va + RSP0_BOTTOM_OFF;
complex->tss.rsp0   = st_va + RSP0_BOTTOM_OFF;
complex->tss.ist[1] = st_va + IST1_BOTTOM_OFF;
complex->tss.ist[2] = st_va + IST2_BOTTOM_OFF;
complex->tss.ist[3] = st_va + IST3_BOTTOM_OFF;
complex->tss.ist[4] = st_va + IST4_BOTTOM_OFF;
```

其中 `RSP0_BOTTOM_OFF = offsetof(per_processor_hardware_stack_t, stack_rsp0) + RSP0_STACKSIZE - 0x40`
（64 字节冗余用于异常帧保存，其他 IST 同理）。

## 3. 内存布局

### 3.1 conjunc_GSs 区间

```
conjunc_GSs.pbase (页对齐)
  │
  ├── GS_Complex[0] (BSP)
  │   ┌────────────────┐
  │   │ slots[256] 2KB │
  │   ├────────────────┤
  │   │ dispatch[256]  │
  │   ├────────────────┤
  │   │ gdt+tss  ~168B │
  │   ├────────────────┤
  │   │ fpu_area  8KB  │
  │   ├────────────────┤
  │   │ stacks_ptr     │   ← 指向 hdstacks 中对应实例
  │   └────────────────┘
  │   stride = 0x14000 (20 pages)
  │
  ├── GS_Complex[1] (AP #1)   0x14000
  ├── GS_Complex[2] (AP #2)   0x14000
  ├── ...
  └── GS_Complex[N-1]         0x14000
```

### 3.2 hdstacks 区间（含 guard 页）

```
hdstacks_interval.pbase (页对齐)
  │
  ├── HW_Stack[0] (BSP)
  │   ┌──────────────────┐  ← 整个 per_processor_hardware_stack_t
  │   │ guard1      4KB  │    未映射
  │   │ stack_rsp0 32KB  │    ← 映射
  │   │ guard2      4KB  │    未映射
  │   │ stack_ist1  8KB  │    ← 映射
  │   │ guard3      4KB  │    未映射
  │   │ stack_ist2 12KB  │    ← 映射
  │   │ guard4      4KB  │    未映射
  │   │ stack_ist3 12KB  │    ← 映射
  │   │ guard5      4KB  │    未映射
  │   │ stack_ist4 12KB  │    ← 映射
  │   └──────────────────┘
  │   stride = sizeof(per_processor_hardware_stack_t) ≈ 96KB
  │
  ├── HW_Stack[1] (AP #1)
  ├── ...
  └── HW_Stack[N-1]
      └── tail_guard 4KB      未映射

总页数 = N × sizeof(per_processor_hardware_stack_t) / 4096 + 1 (尾 guard)
映射：只映射每个栈页（跳过 guard 页），KMMU 对这些范围不建立页表项
```

## 4. 分配

### 4.1 conjunc_GSs

```
总字节  = logical_processor_count × GS_COMPLEX_STRIDE
总页数  = align_up(总字节, 4096) >> 12
```

| 属性 | 取值 |
|------|------|
| 分配器 | `page_allocator::available_meminterval_probe_keep()`（保持端） |
| 页状态 | `page_state_t::kernel_persisit` |
| 对齐 | 4KB（`align_log2 = 12`） |
| 清零 | Phase 3b `ksetmem_8` 整个区间 |
| 映射 | KMMU `map()` → 内核高半空间，WB + RW |
| stride | `GS_COMPLEX_STRIDE = (sizeof(gs_complex_t) + 4095) & ~4095` |

### 4.2 hdstacks

```
栈 stride = sizeof(per_processor_hardware_stack_t)  // 含 guard 页
总物理页  = logical_processor_count × 栈 stride / 4096 + 1  // +1 tail guard
```

| 属性 | 取值 |
|------|------|
| 分配器 | `page_allocator::available_meminterval_probe_keep()`（保持端） |
| 页状态 | `page_state_t::kernel_persisit` |
| 对齐 | 4KB（`align_log2 = 12`） |
| 清零 | 不统清零（栈页仅写入栈顶指针，guard 页不映射） |
| 映射 | **逐栈逐段**映射（跳过 5 个 guard 页 + tail guard） |

### 4.3 生产端（init_init.cpp phase_3b）

```cpp
// conjunc_GSs
{
    uint64_t total_bytes = header->logical_processor_count * GS_COMPLEX_STRIDE;
    uint64_t npg         = total_bytes >> 12;
    phyaddr_t pbase      = page_allocator::available_meminterval_probe_keep(npg, 12);
    vaddr_t  vbase       = va_alloc_up(total_bytes, 12);
    ksetmem_8((void*)(uint64_t)pbase, 0, total_bytes);
    iv.arch_info.conjunc_GSs = {.vpn = vbase >> 12, .ppn = pbase >> 12,
                                .npages = npg, .access = KSPACE_RW_ACCESS};
    kmmu->map({vbase, pbase, total_bytes}, KSPACE_RW_ACCESS);
}

// hdstacks
{
    uint64_t stack_stride = sizeof(per_processor_hardware_stack_t);
    uint64_t total_phys   = header->logical_processor_count * stack_stride + 4096;  // + tail guard
    iv.arch_info.hdstacks_interval_pbase  = page_allocator::available_meminterval_probe_keep(total_phys >> 12, 12);
    iv.arch_info.hdstacks_interval_vbase  = va_alloc_up(total_phys, 12);
    iv.arch_info.hdstacks_4kbpgs_count    = total_phys >> 12;

    // 逐处理器映射栈页（跳过 guard 页）
    for (uint32_t p = 0; p < header->logical_processor_count; p++) {
        uint64_t proc_off   = p * stack_stride;
        phyaddr_t proc_pphy = iv.arch_info.hdstacks_interval_pbase + proc_off;
        vaddr_t   proc_pvir = iv.arch_info.hdstacks_interval_vbase + proc_off;

        kmmu->map({proc_pphy + RSP0_BASE_OFF, proc_pvir + RSP0_BASE_OFF, RSP0_STACKSIZE}, KSPACE_RW_ACCESS);
        kmmu->map({proc_pphy + IST1_BASE_OFF, proc_pvir + IST1_BASE_OFF, DF_STACKSIZE}, KSPACE_RW_ACCESS);
        kmmu->map({proc_pphy + IST2_BASE_OFF, proc_pvir + IST2_BASE_OFF, MC_STACKSIZE}, KSPACE_RW_ACCESS);
        kmmu->map({proc_pphy + IST3_BASE_OFF, proc_pvir + IST3_BASE_OFF, NMI_STACKSIZE}, KSPACE_RW_ACCESS);
        kmmu->map({proc_pphy + IST4_BASE_OFF, proc_pvir + IST4_BASE_OFF, BP_DBG_STACKSIZE}, KSPACE_RW_ACCESS);

        // 设置 stacks_ptr 和 rsp0 栈顶 slot
        gs_complex_t* complex = (gs_complex_t*)(gs_pbase + p * GS_COMPLEX_STRIDE);
        complex->slots[PROCESSOR_RSP0_STACK_BTM_IDX] = proc_pvir + RSP0_BOTTOM_OFF;
        complex->stacks_ptr = (per_processor_hardware_stack_t*)proc_pvir;
    }
}
```

### 4.4 Phase 4.5 补完（GDT/TSS 填充）

Phase 4.5 在跳转入 kernel.elf 前为所有处理器构建完整的 GDT/TSS：

```cpp
for (uint32_t p = 0; p < pcount; p++) {
    gs_complex_t* cx = (gs_complex_t*)(gs_base + p * GS_COMPLEX_STRIDE);
    per_processor_hardware_stack_t* st = cx->stacks_ptr;

    // 填写 GDT 条目（静态模板）
    cx->gdt[K_cs_idx]    = kspace_CS_entry;
    cx->gdt[K_ds_ss_idx] = kspace_DS_SS_entry;
    cx->gdt[U_cs_idx]    = userspace_CS_entry;
    cx->gdt[U_ds_ss_idx] = userspace_DS_SS_entry;

    // 填写 TSS 描述符（基址 = &cx->tss）
    cx->tss_descriptor = ktss;  // 静态模板
    cx->tss_descriptor.base0-3 = ... &cx->tss;

    // 填写 TSS 栈指针（指向 hdstacks 中各栈顶部）
    cx->tss.rsp0   = st_va + RSP0_BOTTOM_OFF;
    cx->tss.ist[1] = st_va + IST1_BOTTOM_OFF;
    cx->tss.ist[2] = st_va + IST2_BOTTOM_OFF;
    cx->tss.ist[3] = st_va + IST3_BOTTOM_OFF;
    cx->tss.ist[4] = st_va + IST4_BOTTOM_OFF;
}
// BSP 立即加载 GDT+TSS 以供后续代码使用
gs_complex_load_gdt_tss(bsp_cx);
```

### 4.5 转运（info_fill.cpp）

```cpp
x86_specify_init_to_kernel_info* dst_arch = ...;
ksystemramcpy(&iv->arch_info, dst_arch, sizeof(iv->arch_info));
```

### 4.6 消费端（kinit.cpp）

```cpp
conjucnt_GSs = arch->conjunc_GSs;
gs_complex_t* bsp_complex = (gs_complex_t*)conjucnt_GSs.vbase();
```

### 4.7 持久化（mem_init.cpp）

```cpp
all_pages_arr::simp_pages_set(conjucnt_GSs.pbase(), conjucnt_GSs.npages,
                              page_state_t::kernel_persisit);
// 栈区间同理
all_pages_arr::simp_pages_set(arch->hdstacks_interval_pbase, arch->hdstacks_4kbpgs_count,
                              page_state_t::kernel_persisit);
```

### CR3 重建映射（mem_init.cpp）

```cpp
Kspace_phyaddr_direct_map(conjucnt_GSs);
// hdstacks 需同样重建：逐栈跳过 guard 页
```

## 5. 启动时序

```
BootInfoHeader.logical_processor_count
    ↓
init.elf Phase 3b:
  1. 分配 conjunc_GSs (gs_complex_t[]): probe_keep → va_alloc → 清零 → 映射
  2. 分配 hdstacks (per_processor_hardware_stack_t[]): probe_keep → va_alloc
  3. 逐处理器逐段映射栈页（跳过 guard 页）
  4. 设置 stacks_ptr + rsp0 slot
  5. 写入 iv.arch_info.{conjunc_GSs, hdstacks_*...}
    ↓
init.elf Phase 4 (info_fill):
  6. arch_info 嵌入 arch_specify 信息包
    ↓
init.elf Phase 4.5:
  7. 填充所有处理器的 GDT/TSS 条目、TSS 栈指针
  8. 加载 BSP 的 GDT+TSS (gs_complex_load_gdt_tss)
  9. shift_kernel → 跳入 kernel.elf
    ↓
kernel.elf very_early_init:
  10. conjucnt_GSs = arch->conjunc_GSs
    ↓
kernel.elf mem_init:
  11. simp_pages_set → 持久化 conjunc_GSs + hdstacks
  12. Kspace_phyaddr_direct_map → CR3 重建映射
    ↓
BSP/AP 资源加载:
  13. gs_complex_t* self = conjucnt_GSs.vbase + proc_id × GS_COMPLEX_STRIDE
  14. wrmsr IA32_GS_BASE = self
  15. stacks_ptr = hdstacks 中对应实例 (已在 Phase 3b 设置)
  16. gs_complex_load_gdt_tss(self)  ← LGDT + LTR
```

## 6. gs_complex_load_gdt_tss 流程

```cpp
gs_complex_load_gdt_tss(gs_complex_t* complex)
    ├── 1. 读取已填写的 GDT 条目和 TSS 描述符
    │       所有字段在 Phase 4.5 已就绪
    ├── 2. 构造 GDTR（base = &gdt, limit = 覆盖至 tss_descriptor 末尾）
    ├── 3. 打包 load_resources_struct
    └── 4. runtime_processor_regist(&res)   ← asm 原语 LGDT + LTR
```

C++ 函数用 `offsetof` / `sizeof` 算偏移，避免硬编码。
asm 原语 `runtime_processor_regist` 只做 LGDT/retfq/LTR，零感知结构体细节。

注意：TSS 栈指针在 Phase 4.5 已由 init_init.cpp 通过 C++ `offsetof` 计算填写，
`gs_complex_load_gdt_tss` **不再重设**栈指针，只加载描述符到架构寄存器。

## 7. AP 初始化模型

### 旧模型（AP 自助）

```
ap_init():
    new x64_local_processor():   ← 含 stack_alloc、GS 复制等
    regist_core → new scheduler → ap_final_work
```

### 新模型（BSP 统一准备 → AP 采纳）

```
BSP 在 kernel_start 中:
    1. GS 复合体已由 init.elf 分配完毕并映射
    2. stacks_ptr 已在 Phase 3b 设置（指向 hdstacks 中对应实例）
    3. GDT/TSS 条目已在 Phase 4.5 全部填写
    4. 构造 dispatch 表（全局共享或 per-CPU 特化）

AP 醒来:
    1. 从 conjunc_GSs 计算自己的 gs_complex_t 地址
    2. wrmsr IA32_GS_BASE = self
    3. stacks_ptr = self->stacks_ptr (已在 Phase 3b 设置)
    4. gs_complex_load_gdt_tss(self)   ← LGDT + LTR
    5. 注册到调度器
    6. 闲逛或进入工作
```

好处：
- AP 不再需要自己的构造函数
- 深度睡眠（C6+）丢弃架构状态后，唤醒走完全相同的采纳路径
- 所有 per-CPU 资源在 init 阶段就确定，运行时零动态分配
- guard pages 在栈间提供溢出隔离，无需软件边界检查

## 8. 关键偏移验证

### gs_complex_t

| 字段 | 偏移 | 约束 |
|------|------|------|
| `slots[256]` | 0x0000 | 8B 对齐 |
| `dispatch[256]` | 0x0800 | 8B 对齐 |
| `gdt[6]` | 0x1000 | 8B 对齐 |
| `tss_descriptor` | 0x1030 | 8B 对齐 |
| `tss` | 0x1040 | 8B 对齐 |
| `fpu_area` | 0x10C0 | **64B 对齐**（XSAVE 要求） |
| `stacks_ptr` | 0x4000 | 8B 对齐 |
| `sizeof(gs_complex_t)` | ≈ 0x4008 | — |

### per_processor_hardware_stack_t

| 字段 | 偏移 | 约束 |
|------|------|------|
| `guard1` | 0x0000 | 页对齐，未映射 |
| `stack_rsp0` | 0x1000 | 页对齐 |
| `guard2` | 0x9000 | 页对齐，未映射 |
| `stack_ist1` | 0xA000 | 页对齐 |
| `guard3` | 0xC000 | 页对齐，未映射 |
| `stack_ist2` | 0xD000 | 页对齐 |
| `guard4` | 0x10000 | 页对齐，未映射 |
| `stack_ist3` | 0x11000 | 页对齐 |
| `guard5` | 0x14000 | 页对齐，未映射 |
| `stack_ist4` | 0x15000 | 页对齐 |

## 9. 需改动的文件

| 文件 | 改动 |
|------|------|
| `src/include/arch/x86_64/abi/GS_complex.h` | ✅ 已创建，含 gs_complex_t + per_processor_hardware_stack_t |
| `src/include/arch/x86_64/abi/GS_Slots_index_definitions.h` | 仅保留 slot 区索引 [0..255] |
| `src/include/arch/x86_64/Interrupt_system/loacl_processor.h` | ✅ 共用数据结构 |
| `src/include/arch/x86_64/boot.h` | conjunc_GSs + hdstacks_interval 字段 |
| `src/init/init_init.cpp` (phase_3b) | 双区间分配 + 逐栈逐段映射 |
| `src/init/init_init.cpp` (phase_45_finalize) | 填充 GDT/TSS + gs_complex_load_gdt_tss |
| `src/init/info_fill.cpp` | 转运（无变化） |
| `src/arch/x86_64/Processor/gs_complex_load_gdt_tss.cpp` | ✅ 已创建（仅 LGDT + LTR，不填栈指针） |
| `src/arch/x86_64/Processor/runtime_processor_regist.asm` | 零修改 |
| `src/arch/x86_64/boot/kernel_entry.asm` | stride 改用 `GS_COMPLEX_STRIDE` |
| `src/arch/x86_64/boot/kinit.cpp` | very_early_init + ap_init 适配 |
| `src/arch/x86_64/boot/mem_init.cpp` | 持久化 + CR3 重建（含双区间） |

## 10. 验证点

1. `gs_complex_t` 各字段 offset 符合布局预期
2. `GS_COMPLEX_STRIDE` 按页对齐
3. `fpu_area` 64B 对齐
4. 每个栈页被映射，每个 guard 页不被映射
5. TSS 栈指针指向各栈顶部（-0x40 保留异常帧空间）
6. 中断分发 `self->dispatch[vec]()` 零间接命中
7. BSP/AP 的 `IA32_GS_BASE` 指向各自 `gs_complex_t` 实例
8. `stacks_ptr` 在 Phase 3b 设置无误
9. AP 醒来后 `gs_complex_load_gdt_tss` 正确装载 LGDT + LTR

## 11. 相关文件索引

| 文件 | 角色 |
|------|------|
| `Docs/arch/x86_64/conjunc_GSs_design.md` | 本文档 |
| `src/include/arch/x86_64/abi/GS_complex.h` | gs_complex_t + per_processor_hardware_stack_t 定义 |
| `src/include/arch/x86_64/abi/GS_Slots_index_definitions.h` | slot 区索引 |
| `src/include/arch/x86_64/boot.h` | conjunc_GSs + hdstacks_interval 字段 |
| `src/include/arch/x86_64/Interrupt_system/loacl_processor.h` | GDT/TSS entry 定义，栈大小常量 |
| `src/init/init_init.cpp` (phase_3b) | 生产端 |
| `src/init/init_init.cpp` (phase_45_finalize) | GDT/TSS 填充 + LGDT+LTR |
| `src/init/info_fill.cpp` | 转运 |
| `src/arch/x86_64/Processor/gs_complex_load_gdt_tss.cpp` | LGDT+LTR 入口 |
| `src/arch/x86_64/Processor/runtime_processor_regist.asm` | LGDT/retfq/LTR asm 原语 |
| `src/arch/x86_64/boot/kernel_entry.asm` | BSP/AP GS 设置 |
| `src/arch/x86_64/boot/kinit.cpp` | very_early_init |
| `src/arch/x86_64/boot/mem_init.cpp` | 持久化 + CR3 重建 |
