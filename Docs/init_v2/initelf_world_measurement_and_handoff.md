# init.elf 世界测量与资产交接

> 历史溯源：`Docs/init_v2/init_protocal_v2_initelf_specification.md`
> 记录了 v2 协议的技术规格。本文档记录各个资产存在的**架构意义**和**淘汰逻辑**，
> 以及"什么必须由 init.elf 做而不能推给 kernel.elf"的设计决策。

---

## 1. 基本模型

init.elf 是一个**临时测量员**。其生命周期只有三个阶段：

1. **介入期**（Phase 1–3b）：接管 UEFI bootloader 的粗糙数据，用命短的临时分配器测量和准备世界
2. **打包期**（Phase 4）：把所有测量结果 + 准备好的资产打包成 `init_to_kernel_header`
3. **自裁期**（Phase 4.5）：释放自身占用的非必要资源，移交 `page[]` 控制权，跳转到 kernel.elf

跳转后 init.elf 的 16MB 映像在物理上依然存在，但从 `page[]` 标记看它只是"被标记为 init 相关的若干物理页"。kernel.elf 的 allocator 不会主动分配它，但可以释放它。init.elf **不留下任何长驻代码**。

---

## 2. 什么必须由 init.elf 做？

### 2.1 MEM_MAP + FPA_bitmaps：分配器自举的双胞胎

| 字段 | 内容 | 谁分配 | 大小依据 |
|------|------|--------|---------|
| MEM_MAP (`pages_arr`) | 每物理页 1 字节 `page_state_t` | `page_allocator` 在 Phase 2 内部建立，Phase 4.5 `relinquish_mem_map()` 转交内核 | `dram_free_page_count × 1B` |
| FPA_bitmaps (`FPA_bitmaps`) | 所有 BCB 的位图池（3bit/页） | init.elf Phase 3b `probe_keep` | `dram_page_count × 3/8 B` |

**为什么不能 kernel.elf 自己分配？**

kernel.elf 内部的 FreePagesAllocator（BCB）需要位图才能运行。BCB 不知道自己的位图在哪里。这是一个**硬鸡生蛋问题**：

- "请分配位图给我" → "好，位图大小是 X"
- → "好，给我一个空闲物理地址来放位图"
- → "我需要分配器才能判断哪些物理地址是空闲的"

唯一的解法是让**一个已经能分配的东西**来分配位图。这个"已经能分配的东西"不能是 kernel.elf 自身，因为 kernel.elf 的 allocator 正是需要位图才能启动的那个。

init.elf 的 `page_allocator` 能分配，是因为它之上还有 `basic_allocator`（单链表线性分配器），而 `basic_allocator` 之上还有 UEFI memory map。这是三层分配器栈：

```
UEFI MemDesc → basic_allocator(单链表) → page_allocator(page[]两区) → kernel.elf BCB
```

每一层只依赖上一层，不依赖自身。

更微妙的问题是**难以挪动性**。即使 kernel.elf 绕过循环依赖硬分配了位图，那几页也会被钉在分配当时的位置——可能与 PT_LOAD 段的加载地址交错、可能落在低 1MB 碎片区、可能紧挨着 GCC 生成的不可移动数据。init.elf 在 Phase 3b 分配时，整个物理地址空间除了自身 16MB 映像和 wait 中的 kernel.elf 文件之外几乎全空，`probe_keep` 能从最干净的位置切出连续保持端。

这就是"临时可死"的独特优势：init.elf **不在物理内存中留长驻足迹**，所以不在乎自己的数据被放到哪个角落。

### 2.2 LOG_BUFFER：跨阶段日志连续性

**为什么不能 kernel.elf 自己做？**

在物理上可以——kernel.elf 也可以分配 2MB 当日志缓冲区。但时序上不行：

- init.elf 的 Phase 1 就开始打 log（`[INIT] Phase 1: I/O + heap ready`）
- Phase 4.5 跳转前还有 log（`[Phase4.5] CR3 <- 0x...`）
- kernel.elf 接手的第一个函数 `mem_init()` 也在打 log（`[mem_init] ...`）

如果 LOG_BUFFER 是 kernel.elf 启动后才建立的，那中间的几十万条 log 就丢光了。这不仅仅是调试体验问题——当 kernel.elf 在 `multi_heap_enable` 之前崩溃且没有日志，你就失去了唯一的信息源。init.elf 的输出（串口）在 kernel.elf 初始化串口之前也断掉了。

LOG_BUFFER 作为物理上已有、不受 CR3 切换影响的内存区域，提供了**无缝日志延续**。

### 2.3 KSYMBOLS：让第一次崩溃可读

kernel.elf 接手后，从 `kinit.cpp` 到 `multi_heap_enable` 之间有十几个函数调用。这些函数中任何一处 crash，裸 RIP 进了 `panic`，如果没有符号表，只能对着 `0xFFFF800000123456` 发呆。

符号表文件通过 `symtable_file`（`movable_file_entry_t`）传递，它的 `.interval` 已被映射到 KMMU，kernel.elf 初始化 addr_to_sym 几行代码后就能用。它必须在 kernel.elf 初始化**任何自己分配的内存之前**就已就位。

### 2.4 logical_processor_count：分配上限前置

参见 §3。

---

## 3. logical_processor_count：一个 uint32_t 的架构重量

### 3.1 传递链

```
UEFI bootloader → ExitBootServices 前数完所有 Local APIC
    → BootInfoHeader.logical_processor_count
        → init.elf Phase 3b (分配 per-CPU 资源)
            → init_to_kernel_header.logical_processor_count
                → kernel.elf mem_init.cpp 全局变量
                    → 调度器 / NVMe / 内存 / TLB / IPI / 中断
```

### 3.2 驱动哪些分配

**init.elf 侧（Phase 3b）：**

```
conjunc_GSs 总量 = proc_count × GS_COMPLEX_STRIDE
hdstacks    总量 = proc_count × stack_stride + 尾 guard
逐处理器:    GS slot 0 写 rsp0 栈顶
             映射 rsp0/ist1/ist2/ist3/ist4 栈（跳过 5 个 guard 页）
```

**kernel.elf 侧（所有 per-CPU 结构以此作为数组容量）：**

| 组件 | 数组/结构 | 容量 |
|------|----------|------|
| kpoolmemmgr | HCB array | `proc_count × (1 << PER_PROCESSOR_HCB_COUNT_ALIGN2)` |
| KspacePageTable | page table statistics | `proc_count` |
| NVMe | SQ complex array | `proc_count + 1` |
| NVMe | CQ count (MSI-X) | `1 + min(max_msix-1, proc_count)` |
| TLB shootdown | 等待响应数 | `proc_count - 1` |
| 调度器 | per-CPU scheduler | `proc_count` |
| AP bringup | IPI 循环 | `pid = 1 → proc_count - 1` |
| broadcast_shutdown | 跳过自身 | 遍历 `0 → proc_count` |

### 3.3 为何不让 init.elf 解析 MADT？

绝大多数内核的做法：

```
init → 解析 XSDT → 定位 MADT → 遍历 Local APIC 结构 → 数出 proc_count → 再分配 per-CPU 资源
```

当前做法：

```
BootInfoHeader 已给出 proc_count → 直接分配 per-CPU 资源
```

差异在于：

- **无需 ACPI 子系统**。init.elf 对 ACPI 的最低需求只有 XSDT+HPET（一个表定位做 MMIO 映射）。不需要了解 MADT 结构、Local APIC 条目类型、Processor UID 等。这直接减少了 init.elf 的代码量和对 ACPI data structure 的编译期依赖。
- **分配上限前置**。`conjunc_GSs` 需要连续物理空间，`probe_keep` 一次完成。如果 init.elf 先分配了 BSP 的动态资源，数完 MADT 发现 32 个 CPU，之前的分配不保证可扩展。
- **原生 SMP 结构**。系统从第一条 C++ 代码起就是 SMP 感知的——所有 per-CPU 结构在分配阶段就以 `proc_count` 为容量创建，即使是单核机器也走同一路径（N=1）。不存在 "UP → 添加 SMP 支持" 的重构事件。

### 3.4 信任模型

init.elf 对 `BootInfoHeader.logical_processor_count` 是完全信任的。这个值是 `probe_keep` 的参数量，决定了两个持久分配的大小。如果 bootloader 汇报的数小于实际硬件：

- 第 `N+1` 个 AP 启动时，其 `gs_complex_t` 落在 `conjunc_GSs` 区间之外 → 写入未映射或未标记 `kernel_persisit` 的物理页
- 这一页可能被 kernel.elf 的 allocator 视为空闲分配给其他用途 → 静默覆盖

**这是 init.elf 唯一需要绝对信任 BootInfoHeader 的地方**。其他信息（内存布局、ACPI 表指针）都经过二次验证或只是参考。

---

## 4. VM_ID 资产清单：生死簿

### 4.1 v1 全貌（fa5cbd8, 2026-03-11）

| ID | 名称 | 分配器 | 大小 | 命运 |
|----|------|--------|------|------|
| 0x1001 | BSP_INIT_STACK | `anonymous_mem_map` | 32KB | 废弃（用 GS rsp0） |
| 0x1002 | FIRST_HEAP_BITMAP | `anonymous_mem_map` | 16KB | 淘汰（放 BSS） |
| 0x1003 | FIRST_HEAP | `anonymous_mem_map` | 4MB | 淘汰（放 BSS） |
| 0x1004 | LOGBUFFER | `anonymous_mem_map` | 4MB | **升格一等字段** |
| 0x1005 | FIRST_BCB_BITMAP | `anonymous_mem_map` | 固定 | 被 0x1008 取代 |
| 0x1006 | KSYMBOLS | `map_symbols_file` | 动态 | **升格一等字段** |
| 0x2001 | UP_KSPACE_PDPT | `anonymous_mem_map` | 1MB | 淘汰（从 kBSS 推导） |
| 0x2002 | GRAPHIC_BUFFER | `map_gop_buffer` | 动态 | 移入 arch_specify |

### 4.2 四个淘汰逻辑

| 淘汰项 | 淘汰原因 | 新归属 |
|--------|---------|--------|
| FIRST_HEAP / 位图 | 固定大小固定用途，直接放 kernel.elf **BSS**，不走信息包通道 | kernel.elf BSS |
| BSP_INIT_STACK | 入口栈统一为 GS slot 0，所有 CPU 走同一结构 | GS 复合体 |
| UP_KSPACE_PDPT | 高半唯一 PDPT 地址可通过 **kBSS 的程序头表解析** 间接获得——init.elf 加载后程序头表 `p_paddr` 被覆写为实际物理地址，本身已构成加载日志 | 程序头表自省 |
| FIRST_BCB_BITMAP | 静态固定大小 → 按 `dram_pages_count` **动态**计算的 `FPA_bitmaps` | `header.FPA_bitmaps` |

### 4.3 四个升格者

| 字段 | 升格形态 | 架构使命 |
|------|---------|---------|
| `pages_arr` (0x1007) | header 一等 vm_interval | 物理内存地形图：每个页框的状态数组。**分配器自举的核心交付件** |
| `FPA_bitmaps` (0x1008) | header 一等 vm_interval | BCB 位图池。**分配器自举的第二个核心交付件** |
| `log_buffer` (0x1004) | header 一等 vm_interval | 跨 init→kernel 的日志延续。**从开始打 log 就不中断** |
| `symtable_file` (0x1006) | header 一等 `movable_file_entry_t` | 拿到文件就可用 addr_to_sym。**让第一条可能崩溃可调** |

### 4.4 架构特定化

| ID | 新归属 | 原因 |
|----|--------|------|
| GRAPHIC_BUFFER (0x2002) | `x86_specify.Gop_vbase + gop_info` | MMIO（WC 语义）不是 allocator 内存 |
| HPET_MMIO (0x2003) | `x86_specify.hpet_mmio` | MMIO（UC 语义），遍历 XSDT 发现 |

### 4.5 迁移全景

```
v1 (fa5cbd8, Mar 11)              v2 (f34f7f3, May 9)              当前 (Jun 30)
=====================             ====================              ==============
0x1001 BSP_INIT_STACK             → header.kernel_entry_stack        🔴 废弃 (用 GS rsp0)
0x1002 FIRST_HEAP_BITMAP          → extra VM arr                     🔴 放 BSS
0x1003 FIRST_HEAP                 → extra VM arr                     🔴 放 BSS
0x1004 LOGBUFFER                  → header.log_buffer                🟢 一等字段
0x1005 FIRST_BCB_BITMAP           🔴 删除                            🔴 消灭
                                  └→ 0x1008 BCBS_BITMAPS             🟢 header.FPA_bitmaps
0x1006 KSYMBOLS                   → header.symtable_file             🟢 movable_file_entry_t
                                  └→ 0x1007 MEM_MAP                  🟢 header.pages_arr
0x2001 UP_KSPACE_PDPT             → extra VM arr                     🔴 程序头表推导
0x2002 GRAPHIC_BUFFER             → arch_specify.Gop_vbase           🟢 arch_specify
                                  └→ 0x2003 HPET_MMIO                🟢 arch_specify.hpet_mmio
```

---

## 5. 信息包最终布局

```
init_to_kernel_header (≈200B, 4 页 = 16KB, 瞬态端分配):
┌─────────────────────────────────┐
│  header 本身                     │
│  ├── magic / self_pages_count   │
│  ├── kmmu_interval              │ KMMU 自身物理区间
│  ├── memory_map_offset         →│ pure_view (所有内存类型)
│  ├── VM_intervals_offset       →│ extra VM (已空)
│  ├── pass_through_*            →│ GOP 信息
│  ├── logical_processor_count    │ [3] 多核世界规模
│  ├── kIMG_self_window + size    │ kernel.elf 完整文件窗口
│  ├── kBSS_interval = {}         │ (已废弃，从程序头表自省)
│  ├── pages_arr (Phase 4.5 填入) │ [2.1] MEM_MAP
│  ├── FPA_bitmaps                 │ [2.1] FPA_bitmaps
│  ├── log_buffer                  │ [2.2] 日志延续
│  ├── symtable_file               │ [2.3] 符号表
│  ├── initramfs_file              │ initramfs 窗口
│  ├── Kspace_phyaddr_access_window│ [0,dram_top) → Kspace
│  └── arch_specify_offset        →│ x86_specify
│                                  │  ├── hpet_mmio
│                                  │  ├── conjunc_GSs
│                                  │  ├── hdstacks
│                                  │  ├── gop_info + Gop_vbase
│                                  │  └── XSDT_base
├─────────────────────────────────┤
│  phymem_segment[]                │ 纯净内存视图
├─────────────────────────────────┤
│  loaded_VM_interval[]            │ extra VM (当前为空)
├─────────────────────────────────┤
│  pass_through_device_info[]      │
├─────────────────────────────────┤
│  GOP info blob                   │
├─────────────────────────────────┤
│  x86_specify_init_to_kernel_info │ 架构特定信息
└─────────────────────────────────┘
```

---

## 6. 设计原则总结

| 原则 | 例子 |
|------|------|
| **需要分配器才能启动分配器的不可径直接推给 kernel** | MEM_MAP + FPA_bitmaps |
| **测量时点在运行 kernel 之前**不被复用的优先前移 | logical_processor_count → bootloader |
| **跨阶段必须不中断的记录**由 init 准备 | LOG_BUFFER |
| **第一条可能出错就能找到原因** | KSYMBOLS |
| **固定大小固定用途**不走信息包、直接放 BSS | FIRST_HEAP |
| **MMIO 不是 allocator 可分配的**不进通用区间 | GOP, HPET |
| **程序头表既声明又日志** → 可推导的就不显式传 | kBSS ← PDPT 地址 |
| **所有 per-CPU 结构一次分配完毕**，N=1 时单核走同路径 | conjunc_GSs + hdstacks |
| **分配时尽可能干净**：在几乎全空的内存中选择连续区域 | page_allocator 两区 |
| **完成使命后自裁**，不留下长驻代码 | relinquish_mem_map |
