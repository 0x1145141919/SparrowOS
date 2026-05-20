# init.elf v2 协议规范

文档定位：对比 v2 (当前 master) 与 v1 (commit a6a111d) 的 init.elf 实现，
阐明核心设计与变更动机。

---

## 1. 概述

v2 init.elf 对 v1 做了三项根本性重构：

| 维度 | v1 | v2 |
|------|----|----|
| 物理内存分配器 | `basic_allocator` 单链表线性分配 | `page_allocator` 基于 page 数组的两区分配 |
| 内核加载策略 | 破坏性加载（段按需单载） | 非破坏性加载（保留完整 ELF 文件镜像） |
| 信息包 | 指针式 `init_to_kernel_info` | 偏移式 `init_to_kernel_header` + 显式 vm_interval 声明 |

以下逐项展开。

---

## 2. 物理内存分配器

### 2.1 v1：basic_allocator 单链表线性分配

```
UEFI MemDesc 链表 → 排序/填充空洞/回收/合并 → pure_mem_view[]
                                                      ↓
                                          pages_alloc(size, align)
                                          (从 low 向 high，线性扫描 + 单指针 cursor)
```

**核心缺陷：**

1. **单一方向**：所有分配从低地址向上，高地址长期空闲，无法利用整体空间
2. **无页面粒度跟踪**：只有 `pure_mem_view` 的区间级描述，分配后仅修改区间类型，无法记录每个物理页的状态
3. **无自我保护**：`basic_allocator` 自身分配的内存（如 page[] 数组）未标记到自身，后继内核无法感知
4. **算法局限**：线性分配 = O(n) 扫描，cursor 单调递增，不释放

### 2.2 v2：page_allocator page 数组两区分配

```
pure_mem_view → page_allocator::init()
                    ↓
              page[] mem_map (每物理页 1 字节 state)
              phyinterval_t[] intervals (区间描述)
              scan_top_base (瞬态端光标，高→低递减)
              scan_down_base (保持端光标，低→高递增)
```

**两区分配设计：**

```
低地址 ←---------- scan_down_base -----→ ←------- scan_top_base -----→ 高地址
         保持端已分配区       ↑                ↑       瞬态端已分配区
                         向上生长          向下生长
     
         保持端: 不可移动的持久分配（内核代码/数据/堆）
         瞬态端: 可释放的临时分配（日志缓冲/initramfs 搬迁/init ELF 映射）
         光标: 每次 allocate 后推进，中间交错碎片最小化
```

**关键接口：**

| 接口 | 方向 | 用途 |
|------|------|------|
| `available_meminterval_probe()` | `scan_top_base` 向下扫 | 瞬态分配 |
| `available_meminterval_probe_keep()` | `scan_down_base` 向上扫 | 保持分配 |
| `pages_set(interval, state)` | 标记区间 | 设置页状态 + 追踪 free_pages |
| `interval_top_to_bottom_ff_scan()` | 区间内高→低首次适应 | 瞬态扫描子算法 |
| `interval_bottom_to_top_ff_scan()` | 区间内低→高首次适应 | 保持扫描子算法 |

**v2 解决的核心问题：**

1. **页面粒度**：每个物理页 1 字节 `page_state_t`，确切追踪每个页的状态
2. **自引用保护**：`init()` 结束后 `pages_set` 标记 mem_map 自身和 init.elf 映像，防止被误分配
3. **两区分离**：持久/临时分配从两端生长，避免长期低地址碎片导致大块分配失败
4. **free_pages 实时计数**：每次 pages_set 自动更新，可即时查询剩余空闲页

---

## 3. kernel.elf 加载策略

### 3.1 v1：破坏性加载

```
kernel.elf ELF 解析
  ↓
每个 PT_LOAD 段 → basic_allocator::pages_alloc() 分配物理页
                → 从 ELF 文件镜像复制内容
                → 映射到 KMMU
                → ELF 文件镜像可覆写
```

问题：kernel.elf 的 section headers、symbol table、debug 信息全部丢失，后续无法做 addr_to_sym、栈回溯等自省操作。

### 3.2 v2：非破坏性加载

```
kernel.elf 完整文件 → available_meminterval_probe_keep() 分配物理区间
                   → ksystemramcpy(ramfs → g_kimg_pbase)
                   → PT_LOAD 段在文件镜像内原位解析
                   → KMMU 映射只建页表，不修改文件镜像
                   → kIMG_self_window 映射整个文件到内核 VA 空间
```

**不变性保证：**

- `g_kimg_pbase`：kernel.elf 文件镜像的物理基址（已验证可读可执行）
- `kIMG_self_window`：映射整个文件到内核高半空间，虚拟地址为 `kIMG_self_window.vbase`
- 所有 section（含 `.symtab`、`.comment`、`.note.*`、`.debug_*`）均可通过 `vbase + sh_offset` 访问
- BSS 段**独立分配**（`kBSS_interval`），不与文件镜像重叠

**加载时序：**

```
Phase 3a:
  1. 从 initramfs 提取 kernel.elf（完整文件）
  2. probe_keep 分配连续物理区间，拷贝整个文件 → g_kimg_pbase
  3. 解析 ELF program headers 找到 PT_LOAD 段
  4. 原位解析各段虚拟地址/文件偏移
  5. 非 BSS 段：在 g_kimg_pbase 文件镜像内直接映射（文件内容即段内容）
  6. BSS 段：probe_keep 另分配物理页，清零
  7. KMMU 高半映射所有段
  8. kIMG_self_window：整个文件映射到内核 VA
  9. 记录 kBSS_interval

Phase 3b:
  10. 恒等映射 + 分配并映射辅助区域（日志/栈/堆/位图/页框数组）
  11. ksymbols.bin 映射 → 符号表可访问
  12. initramfs 文件窗口映射

Phase 4:
  13. 构造 init_to_kernel_header（含所有 vm_interval）
  14. x86 架构信息（GOP/HPET/ACPI）

Phase 4.5:
  15. page_allocator::relinquish_mem_map() → 交出 mem_map
  16. 构建 pages_arr（page 数组的转生体）
  17. shift_kernel → 跳转
```

---

## 4. init_to_kernel_header 信息包

### 4.1 v1：指针式结构体

```c
struct init_to_kernel_info {        // 48 bytes + 3 pointers
    uint64_t magic;
    uint64_t self_pages_count;
    void* gST_ptr;                  // ← 指针，依赖运行时的物理地址
    uint64_t ksymbols_file_size;
    phyaddr_t kmmu_root_table;
    phymem_segment kmmu_interval;
    uint64_t phymem_segment_count;
    phymem_segment* memory_map;     // ← 指针
    uint64_t loaded_VM_interval_count;
    loaded_VM_interval* loaded_VM_intervals;  // ← 指针
    uint64_t pass_through_device_info_count;
    pass_through_device_info* pass_through_devices;  // ← 指针
    uint32_t logical_processor_count;
};
```

缺陷：
- **指针不可重定位**：物理地址指针，kernel.elf 若在不同基址映射则全部失效
- **隐含继承**：`gST_ptr`、`ksymbols_file_size` 是 bootloader 层泄漏到内核的细节
- **未描述已分配区域**：内核不知哪些物理区间已被占用（需自行从 memory_map 推断）

### 4.2 v2：偏移式 + 显式 vm_interval 声明

```c
struct init_to_kernel_header {      // 固定大小 + offset 数组 + vm_interval 列表
    uint64_t magic;
    uint64_t self_pages_count;
    phymem_segment kmmu_interval;
    uint64_t phymem_segment_count;
    uint64_t memory_map_offset;           // ←  偏移，可重定位
    uint64_t loaded_VM_interval_count;
    uint64_t loaded_VM_intervals_offset;  // ←  偏移
    uint64_t pass_through_device_info_count;
    uint64_t pass_through_devices_offset; // ←  偏移
    uint32_t logical_processor_count;
    vm_interval kIMG_self_window;     // 内核 ELF 文件镜像
    vm_interval kBSS_interval;        // 内核 BSS 段
    vm_interval pages_arr;            // 页框数组转生
    vm_interval FPA_bitmaps;          // Free-Page-Allocator 位图
    vm_interval log_buffer;           // 日志缓冲
    vm_interval kernel_entry_stack;   // BSP 初始化栈
    vm_interval symtable_file;        // ksymbols.bin 符号文件
    vm_interval initramfs_file;       // initramfs 文件
    vm_interval identity_map_window;  // [0x4000, dram_top) 恒等映射
    uint64_t arch_specify_offset;     // ←  偏移
};
```

**设计原则：**

1. **所有指针改为相对于信息包基址的偏移量**
   ```
   loaded_VM_interval* arr = (loaded_VM_interval*)(pkt_base + header->loaded_VM_intervals_offset);
   ```
   使信息包可在任意虚拟基址访问，不依赖物理地址。

2. **显式声明所有已分配物理区间**
   每个 vm_interval 明确告诉内核：这块物理内存已被占用，不必再分配。
   内核接手后可直接复用或重新映射。区间包括：
   - `kIMG_self_window`：kernel.elf 完整文件镜像
   - `kBSS_interval`：BSS 独立分配
   - `pages_arr`：page 数组转生体（phase 4.5 移交）
   - `FPA_bitmaps`：Free-Page-Allocator 内存池位图
   - `log_buffer` / `kernel_entry_stack`：瞬态分配
   - `symtable_file` / `initramfs_file`：文件映射
   - `identity_map_window`：恒等映射区域

3. **删除 v1 中的隐含字段**
   - `gST_ptr`：UEFI 系统表在 phase 4 之后不再需要，从信息包中移除
   - `ksymbols_file_size`：符号表信息移入 `symtable_file.vm_interval`

4. **新增 `arch_specify_offset`**
   架构特定信息（GOP 帧缓冲、HPET MMIO、XSDT 基址等）以偏移量形式挂在末尾，
   支持不同架构（x86_64、ARM64 等）自定义信息包内容。

### 4.3 内存布局

```
pkt_pbase → [init_to_kernel_header]          // 固定大小 ~200 bytes
             [padding to align 8]
             [phymem_segment[]]               // memory_map_offset
             [padding to align 8]
             [loaded_VM_interval[]]           // loaded_VM_intervals_offset
             [padding to align 8]
             [pass_through_device_info[]]     // pass_through_devices_offset
             [arch_specify data]              // arch_specify_offset
```

所有数组和变长数据均通过 offset 索引，信息包总大小 = `self_pages_count × 4096`。

---

## 5. 执行流程对比

### 5.1 v1 流程

```
bootloader
  ↓ 加载 kernel.elf + ksymbols.bin
init.elf:
  Phase 1: basic_allocator::Init() (UEFI MemDesc → 视图 + 空洞填充)
  Phase 2: basic_allocator::pages_alloc() 分配 kernel.elf PT_LOAD 区域
           kernel_load() 逐段加载 + 映射 + 填充 BSS
  Phase 3: 分配并映射匿名区域（栈/堆/日志/页表）
           anonymous_mem_map() × 6 + map_symbols_file() + GOP
  Phase 4: 构造 init_to_kernel_info (指针式)
  Phase 5: shift_kernel → 跳转到 kernel.elf
```

### 5.2 v2 流程

```
bootloader
  ↓ 加载 kernel.elf + ksymbols.bin + initramfs（完整文件）
init.elf:
  Phase 1: heap 初始化 + 透传设备初始化 + 控制台初始化
  Phase 2: basic_allocator::Init()
           标记已占用区域
           page_allocator::init() (构建 page[] + 两区分配器)
           relocation page_allocator 内部自保护
  Phase 3a (串行):
           从 initramfs 提取 kernel.elf
           probe_keep 分配物理区间，拷贝完整 ELF 文件
           非破坏性解析 PT_LOAD → 原位映射 + BSS 独立分配
           kIMG_self_window 映射整个文件
           kBSS_interval 记录
  Phase 3b (串行):
           恒等映射 [0x4000, dram_top)
           probe_keep/probe 分配辅助区域
           FPA_bitmaps/日志/栈/堆/页表/符号表/initramfs 映射
           GOP/HPET 等硬件信息收集
  Phase 4:
           构造 init_to_kernel_header (偏移式 + 显式 vm_interval)
           写入所有已分配区域的 vm_interval
  Phase 4.5:
           page_allocator::relinquish_mem_map() → 清零并移交 mem_map
           构建 pages_arr → 内核 Free-Page-Allocator 接管
           shift_kernel → 跳转到 kernel.elf
```

### 5.3 关键差异

| 方面 | v1 | v2 |
|------|----|----|
| kernel.elf 文件 | 读取后丢弃 | 完整保留在 kIMG_self_window |
| 内存分配器 | basic_allocator 单一 | basic_allocator → page_allocator 两阶段 |
| BSS 加载 | 在 PT_LOAD 段内分配 | 独立 probe_keep 分配 |
| 页面跟踪 | 无（仅有区间级） | page[] 数组 + free_pages 计数 |
| 信息包 | 指针，不可重定位 | 偏移，可重定位 |
| 移交页表 | 不涉及 | phase 4.5 移交 mem_map + pages_arr |
| 恒等映射 | 低端部分约 4GB | [0x4000, dram_top) 全量 |

---

## 6. 数据结构对比

### 6.1 vm_interval（v2 引入的通用区间描述）

```c
struct vm_interval {
    vaddr_t vbase;          // 虚拟基址
    phyaddr_t pbase;        // 物理基址
    uint64_t size;          // 区间大小
    pgaccess access;        // 访问权限 / 缓存策略
};
```

v1 中这些信息散布在多个数组中：`loaded_VM_interval` 用于 PT_LOAD 段和匿名区域，
内存映射段用 `phymem_segment`。v2 统一用 `vm_interval` 表达所有已映射区间。

### 6.2 page_state_t（v2 page 数组的页状态枚举）

```c
enum class page_state_t : uint8_t {
    free = 0,               // 空闲
    kernel_persisit = 1,    // 内核持久数据
    kernel_anonymous = 2,   // 内核匿名映射
    user_file = 3,          // 用户文件映射
    user_anonymous = 4,     // 用户匿名映射
    dma = 5,                // DMA
    kernel_pinned = 10,     // 内核锁页
    reserved = 63           // 保留（不可分配）
};
```

v1 中页状态隐式存在于 `PHY_MEM_TYPE`（`freeSystemRam`/`OS_KERNEL_DATA` 等），
但类型粒度粗且与 UEFI 类型枚举耦合。v2 独立出 `page_state_t`，明确区分物理页角色。

---

## 7. 自裁与移交（Phase 4.5）

v2 引入的关键阶段：init.elf 在跳转到内核前需要**自裁释放自身占用的管理结构**。

### 7.1 移交内容

| 资源 | 移交方式 | 接收方 |
|------|----------|--------|
| `page[]` mem_map | `relinquish_mem_map(pbase, pcount)` | 内核 pages_arr |
| `phyinterval_t[]` 数组 | 随信息包给地址 | 内核可读（但通常重建） |
| `scan_top_base/scan_down_base` | 丢弃 | — |
| free_pages 计数 | 在信息包中携带？ | 内核重建后重新统计 |

### 7.2 mem_map 转生

```
init.elf 的 page_allocator
  ↓ PHASE 4.5: relinquish_mem_map
  ↓ mem_map 清零后交出物理地址
  ↓
kernel.elf 的 FreePageAllocator
  ↓ 基于 mem_map（已清空）重建自身 page 数组
  ↓ memory_map 中的 freeSystemRam 区间 → 可用物理页池
```

所有由 init.elf `pages_set()` 设置的 state 信息在 relinquish 时被清零，
内核从纯净的 `phymem_segment` 视图重新构建位图和管理结构。

---

## 8. 非破坏性加载的内核自省能力

保留完整 kernel.elf 文件镜像的直接收益：

| 能力 | 数据源 | 是否立即可用 |
|------|--------|-------------|
| `addr_to_sym(RIP)` | `.symtab` + `.strtab` | 是（ksymbols.bin 也可） |
| `.comment` 查询 | `KImage.vbase + sh_offset` | 是 |
| `.note.gnu.build-id` 查询 | `.note` 段 + PT_NOTE PHDR | 需 `--build-id` 链接 |
| Section 表全量查询 | `Shdr` 表 | 是（KImage_Introspection 已实现） |
| DWARF 栈回溯 | `.eh_frame` / `.debug_frame` | 需 `.eh_frame` 编译支持（待发布） |
| `.debug_*` 调试信息 | `.debug_info` 等 | 需要 DWARF 解析器 |

---

## 9. 阶段上下文交付范式

### 9.1 动机

`init_init.cpp` 初期使用文件级 static 全局变量在各阶段之间传递数据：

```cpp
// ⚠️ 旧范式：隐式依赖，编译器不检查
static phyaddr_t  g_kimg_pbase;
static vaddr_t    g_entry_vaddr;
vm_interval g_kIMG_self_window;
// phase_3a 写 → phase_3b 读，但无显式契约
```

问题：
1. 全局变量的生产/消费关系靠注释和约定，重构时容易遗漏
2. 无法通过类型系统检查阶段间的数据依赖
3. 新增阶段时不知道该读/写哪些变量

### 9.2 规范

每个阶段定义为**返回一个上下文结构体**，该结构体包含本阶段的所有产物。
后续阶段通过参数显式获取需要的上下文。

```cpp
// ✅ 新范式：每个阶段产出显式上下文
// 上下文定义
struct ctx_early_mem {
    phyaddr_t xsdt_base;
    phyaddr_t ramfs_base;
    uint64_t  ramfs_size;
};

struct ctx_kernel_loaded {
    kernel_mmu*   kmmu;
    phyaddr_t     kimg_pbase;
    uint64_t      kimg_file_size;
    vm_interval   kIMG_self_window;
    vm_interval   kBSS_interval;
    vaddr_t       entry_vaddr;
    uint64_t      kernel_vaddr_top;
};

struct ctx_intervals {
    vm_interval           FPA_bitmaps;
    vm_interval           log_buffer;
    vm_interval           kernel_entry_stack;
    movable_file_entry_t  symtable_file;
    movable_file_entry_t  initramfs_file;
    vm_interval           identity_map_window;
    vaddr_t               pages_arr_vbase;
    loaded_VM_interval*   extra_vm_arr;
    uint64_t              extra_vm_count;
    x86_specify_init_to_kernel_info arch_info;
};
```

阶段函数签名：

```cpp
// Phase 1：I/O + 堆（纯副作用，无产出）
void          phase_1(BootInfoHeader*);

// Phase 2：内存早期初始化
ctx_early_mem phase_2(BootInfoHeader*);

// Phase 2.5：initramfs 高位搬迁（修改 ctx_early_mem.ramfs_base/size）
void          phase_25_relocate_initramfs(BootInfoHeader*, ctx_early_mem*);

// Phase 3a：kernel.elf 基础加载
ctx_kernel_loaded phase_3a(const ctx_early_mem*);

// Phase 3b：恒等映射 + 各类区间分配
ctx_intervals     phase_3b(const ctx_kernel_loaded*, BootInfoHeader*);

// Phase 4：构建 init_to_kernel_header
phyaddr_t         phase_4(const ctx_kernel_loaded*, const ctx_intervals*,
                          BootInfoHeader*);

// Phase 4.5：自裁 + CR3 切换 + shift_kernel（终止性，无返回）
void              phase_45(const ctx_kernel_loaded*, const ctx_intervals*,
                           phyaddr_t info_pbase);
```

`init()` 主流程：

```cpp
extern "C" void init(BootInfoHeader* header) {
    phase_1(header);
    auto em  = phase_2(header);
    phase_25_relocate_initramfs(header, &em);
    auto kl  = phase_3a(&em);
    auto iv  = phase_3b(&kl, header);
    auto pkt = phase_4(&kl, &iv, header);
    phase_45(&kl, &iv, pkt);
}
```

### 9.3 规则

1. **每个 phase 函数要么返回一个上下文，要么是纯副作用。** 全局变量（`g_va_alloc_base` 等极少数例外）全部消除。
2. **上下文按值传递**（小结构体），或 const 指针（大结构体如 `ctx_kernel_loaded`）。
3. **阶段的消费品必须来自前序阶段的上下文**，不允许隐式读取未经过参数传递的全局变量。
4. **上下文字段的类型随 `vm_interval` 升级同步更新**。例如 `symtable_file` 从 `vm_interval` 变为 `movable_file_entry_t` 时，只需修改 `ctx_intervals` 中的字段类型，编译器会标出所有使用点。

### 9.4 收益

| 维度 | 全局变量范式 | 上下文交付范式 |
|------|------------|--------------|
| 依赖检查 | 人工审查 | 编译器保证 |
| 重构效率 | 全文搜索 → 人工确认 | 改字段类型 → 编译器报错 |
| 新增阶段 | 不知道该用哪些变量 | 上下文签名为文档 |
| 测试 | 无法 mock | 可构造上下文实例注入 |

---

## 10. 与 v1 的向后兼容性

- v2 `init_to_kernel_header` 与 v1 `init_to_kernel_info` **完全不兼容**
- 结构体字段、布局、语义全部改变
- 协议版本号在 bootloader 与 init.elf 之间协商
- kernel.elf 必须与 init.elf 同时升级
