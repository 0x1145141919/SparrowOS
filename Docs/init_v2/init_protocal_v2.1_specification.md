# init.elf v2.1 协议规范

文档定位：以 v2.0 (commit 7cfa253) 为基线，阐明 v2.1 的核心设计演进与变更动机。

---

## 1. v2.0 → v2.1 总览

v2.1 在 v2.0 基础上做了 10 项关键演进：

| 维度 | v2.0 | v2.1 | 变更动机 |
|------|------|------|----------|
| 内核文件映像用途 | keep-end 分配，是执行态的一部分 | 瞬态端分配，**仅供自省**；可删除/swap 到 VFS | kernel.elf 只是初始状态，不绑定执行路径 |
| 段物理地址 | `kimg_pbase + p_offset` 公式计算 | 写入 ELF **`p_paddr`**，自省时按图索骥 | 消除文件映像对段布局的约束，为大页映射创造条件 |
| 独立段分配 | 仅 BSS 特殊处理 | 所有段均可独立（**`p_flags & 0x100`** 标志），链接脚本逐段声明 | 数据段/代码段可独立 2MB/1GB 映射 |
| BSS 追踪 | `kBSS_interval` 单独 vm_interval | 普通 PT_LOAD(0x100)，从程序头表 `p_paddr` 读 PA | 统一段处理路径，消除特殊分支 |
| BSP 入口栈 | `kernel_entry_stack` 瞬态端 32KB 专用栈 | BSP GS 复合体 **`tss.rsp0`**（hdstacks 内嵌） | 消除冗余瞬态区间，复用已有 per-CPU 资源 |
| GS 复合体栈 | 设想为内嵌在 `gs_complex_t` 中 | 分离到 **`per_processor_hardware_stack_t`**，含 guard 页，逐栈映射 | guard 页需要逐栈映射，与元数据区间解耦 |
| 持久映射接口 | `KspacePageTable::enable_VMentry`（只写页表） | `Kspace_phyaddr_direct_map` / `Kspace_pinterval_alloc_and_map`（**建红黑树**） | 恒定映射需要区间追踪，不可丢失 |
| Phase 4.5 | 仅 self-destruct + pages_arr 移交 | 额外：**遍历所有处理器填 GDT/TSS**、`gs_complex_load_gdt_tss(BSP)` | AP 醒来无需分配，直接采纳预填架构状态 |
| 内置自省 | `KImage_Introspection` 全功能（215 行，10 导出接口） | 精简为 **2 接口**（58 行），BSS 物理地址从程序头表实时查 | 废弃零调用接口，唯一依赖 `kIMG_self_window` 全局 |
| `kernel_entry_stack` | 信息包有效字段 | **归零废弃** | 已无对应分配 |

---

## 2. kernel.elf 加载策略（v2.0→v2.1 重构）

### 2.1 v2.0：非破坏性加载

```
kernel.elf 完整文件 → available_meminterval_probe_keep() 分配物理区间
                   → ksystemramcpy(ramfs → kimg_pbase)
                   → PT_LOAD 段在文件镜像内原位解析
                   → BSS: probe_keep 独立分配，记录 kBSS_interval
                   → KMMU 映射只建页表，不修改文件镜像
                   → kIMG_self_window 映射整个文件到内核 VA 空间
```

**局限：**
1. 文件映像`kimg_pbase + p_offset`约束各段物理布局，无法独立 2MB 对齐
2. 数据段寄生在文件映像内，大页映射受文件映像布局限制
3. BSS `bss_idx`特殊路径增加代码复杂度

### 2.2 v2.1：自省映像 + 独立段加载

```
kernel.elf 完整文件 → available_meminterval_probe() 瞬态端分配（仅供自省）
                   → ksystemramcpy(ramfs → kimg_pbase)
                   → 解析 ELF 程序头表
                   → 对每个 PT_LOAD 段：
                       ├ p_flags & 0x100 → probe_keep 独立分配物理页
                       │                  拷贝文件内容，清零 BSS 余部
                       │                  实际 PA 写回 ELF 程序头表 p_paddr
                       │                  KMMU 映射
                       └ 否则 → 按 ELF 声明的 p_paddr 映射 + 拷贝内容
                   → kIMG_self_window 映射瞬态端文件至内核 VA 空间
```

**核心变化：**

| 方面 | v2.0 | v2.1 |
|------|------|------|
| 文件映像分配器 | `available_meminterval_probe_keep()`（保持端） | `available_meminterval_probe()`（瞬态端） |
| 段 PA 来源 | `kimg_pbase + p_offset`（公式计算） | `ph->p_paddr`（init.elf 改写后的值） |
| 0x100 段 | 不存在此概念 | `probe_keep` 独立物理页，PA 写回 `p_paddr` |
| 非 0x100 段 | 不存在此概念 | 按 `p_paddr` 映射，同样拷贝内容 |
| BSS 处理 | `bss_idx` 特殊路径，记录 `kBSS_interval` | 普通 PT_LOAD(0x100)，统一循环 |
| 2MB 对齐 | 无要求 | 链接脚本 `ALIGN(2M)` + 段起止 2MB 边界 |

### 2.3 链接脚本配合

```ld
text_main PT_LOAD FLAGS(5 | 0x100);   /* PF_R | PF_X + 独立分配 */
data      PT_LOAD FLAGS(6 | 0x100);   /* PF_R | PF_W + 独立分配 */
rodata    PT_LOAD FLAGS(4 | 0x100);   /* PF_R + 独立分配 */
bss       PT_LOAD FLAGS(6 | 0x100);   /* PF_R | PF_W + 独立分配 */
```

每段 `ALIGN(2M)` 起止，`p_paddr` 初始值 = `p_vaddr`，init.elf 覆盖为真实 PA。

### 2.4 Phase 3a 伪代码

```cpp
static ctx_kernel_loaded phase_3a_load_kernel(kernel_mmu* kmmu, ...) {
    // 1. 瞬态端分配
    kimg_pbase = probe(transient);  // 不再是 probe_keep
    
    // 2. 遍历 PT_LOAD
    for each PT_LOAD:
        if (flags & 0x100):
            pa = probe_keep(npg);          // 独立分配
            memcpy(pa ← file_image + offset);
            if (memsz > filesz) memset(pa + filesz, 0);
            ph->p_paddr = pa;              // 写回 ELF
        else:
            pa = ph->p_paddr;              // 指定的物理地址
            memcpy(pa ← file_image + offset);
            if (memsz > filesz) memset(pa + filesz, 0);
        kmmu->map(pa, va, memsz, flags);
    
    // 3. kIMG_self_window 映射瞬态文件
    kIMG_self_window = kmmu->map(kimg_pbase, va, size);
}
```

---

## 3. Phase 4.5 扩展

v2.0 的 Phase 4.5：仅 `relinquish_mem_map` → `pages_arr` → `shift_kernel`。

v2.1 扩展为：

```
Phase 4.5:
  ├── 4.5-1: relinquish_mem_map → 取 mem_map 物理地址
  ├── 4.5-2: pages_arr 填入 header，KMMU 映射
  ├── 4.5-3: CR3 ← KMMU root table（切换页表）
  ├── 4.5-4: 遍历所有处理器，填充：
  │           ├── GDT 条目（kspace_CS/kspace_DS/userspace_CS/userspace_DS）
  │           ├── TSS 描述符（基址 = &gs_complex_t::tss）
  │           ├── TSS 栈指针（rsp0/ist[1-4] → hdstacks 对应栈顶）
  │           └── stacks_ptr + rsp0 slot
  ├── 4.5-5: gs_complex_load_gdt_tss(BSP)  ← LGDT + LTR
  └── 4.5-6: init_jump_to_kernel(entry, BSP.rsp0)  ← iretq
```

**变更动因：** AP 醒来后不需要自己分配/构建 GDT/TSS，Phase 4.5 已全部预备好。
AP 只需 `wrmsr IA32_GS_BASE` + `gs_complex_load_gdt_tss(self)` 即可就绪。
深度睡眠恢复同理。

**入口栈变更：** `kernel_entry_stack` 被完全废弃，改用 BSP 的 `gs_complex_t::tss.rsp0`，
该栈已在 hdstacks 中分配并持久化，无需额外 VM 区间。

---

## 4. GS 复合体与硬件栈分离

### 4.1 v2.0 设计（文档层面）

```
gs_complex_t（单一连续区间 96KB/proc）:
  ┌────────────────────┐
  │ slots[256]    2KB  │
  │ dispatch[256] 2KB  │
  │ gdt/tss     ~168B  │
  │ fpu_area     8KB   │
  │ stack_rsp0  32KB   │  ← 内嵌
  │ stack_ist1   8KB   │  ← 内嵌
  │ stack_ist2  12KB   │  ← 内嵌
  │ stack_ist3  12KB   │  ← 内嵌
  │ stack_ist4  12KB   │  ← 内嵌
  └────────────────────┘
  无 guard 页，无分离
```

### 4.2 v2.1 实际实现

**两个独立 VM 区间：**

```
conjunc_GSs 区间（stride = 0x14000, 20 pages/proc）:
  ┌────────────────────┐
  │ slots[256]    2KB  │
  │ dispatch[256] 2KB  │
  │ gdt/tss     ~168B  │
  │ fpu_area     8KB   │
  │ stacks_ptr         │  ← 指向 hdstacks 中对应实例
  └────────────────────┘

hdstacks 区间（stride ≈ 96KB/proc，含 guard 页）:
  ┌────────────────────┐
  │ guard1        4KB  │  ← 不映射
  │ stack_rsp0   32KB  │  ← 映射
  │ guard2        4KB  │  ← 不映射
  │ stack_ist1    8KB  │  ← 映射
  │ guard3        4KB  │  ← 不映射
  │ stack_ist2   12KB  │  ← 映射
  │ guard4        4KB  │  ← 不映射
  │ stack_ist3   12KB  │  ← 映射
  │ guard5        4KB  │  ← 不映射
  │ stack_ist4   12KB  │  ← 映射
  └────────────────────┘
  + 尾 guard 4KB（不映射）
```

**boot.h 新增字段：**

```cpp
struct x86_specify_init_to_kernel_info {
    vm_interval hpet_mmio;
    vm_interval conjunc_GSs;                    // gs_complex_t 实例
    phyaddr_t   hdstacks_interval_pbase;        // 新增
    uint64_t    hdstacks_4kbpgs_count;          // 新增
    vaddr_t     hdstacks_interval_vbase;        // 新增
    GlobalBasicGraphicInfoType gop_info;
    vaddr_t Gop_vbase;
    phyaddr_t XSDT_base;
};
```

**映射逻辑（Phase 3b，kernel.elf 侧 `kimg_affiliate_property_map1` 镜像）：**

```cpp
for each processor p:
    proc_off = p × sizeof(per_processor_hardware_stack_t);
    map(proc_off + RSP0_BASE_OFF, RSP0_STACKSIZE);  // 跳过 guard1
    map(proc_off + IST1_BASE_OFF, DF_STACKSIZE);    // 跳过 guard2
    map(proc_off + IST2_BASE_OFF, MC_STACKSIZE);    // 跳过 guard3
    map(proc_off + IST3_BASE_OFF, NMI_STACKSIZE);   // 跳过 guard4
    map(proc_off + IST4_BASE_OFF, BP_DBG_STACKSIZE);// 跳过 guard5
```

---

## 5. 持久映射接口统一

v2.0 使用 `KspacePageTable::enable_VMentry()` 映射持久区间（只写页表，不维护地址空间红黑树）。
v2.1 统一使用：

| 接口 | 用途 | 行为 |
|------|------|------|
| `Kspace_phyaddr_direct_map(interval)` | 已知 VA 的恒定映射 | 建页表 + 插入区间树 |
| `Kspace_pinterval_alloc_and_map(interval, &kurd)` | 需分配 VA 的新映射 | 分配 VA + 建页表 + 插入区间树 |

受影响代码路径：

| 函数 | v2.0 接口 | v2.1 接口 |
|------|-----------|-----------|
| `KImage_map_rebuild()` | `KspacePageTable::enable_VMentry` | `Kspace_phyaddr_direct_map` |
| `kimg_affiliate_property_map1()` hw_stacks | `KspacePageTable::enable_VMentry` | `Kspace_phyaddr_direct_map` |
| `kimg_affiliate_property_map1()` 通用列表 | `KspacePageTable::enable_VMentry` | 保持不变（列表本身） |
| `acpimgr_t::Init()` | `alloc_available_space` + `enable_VMentry` | `Kspace_pinterval_alloc_and_map` |

---

## 6. 自省接口精简

v2.0 `KImage_Introspection` 导出 12 个接口（`get_KImage_sections`、`get_KImage_phdrs`、
`get_KImage_note`、`is_bss_vaddr` 等），实际外部调用仅 2 个。

v2.1 精简为：

```cpp
// 唯一初始化入口：读 kIMG_self_window 全局，缓存 BSS 程序头
void self_introspection_init();

// BSS VA→PA 转换：从缓存程序头读 p_paddr 计算
phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr);
```

- 废弃 `vm_interval KImage, Kbss` 全局，唯一依赖 `mem_init.cpp` 的 `kIMG_self_window`
- BSS 程序头识别：`p_type==PT_LOAD && p_filesz==0 && p_memsz>0 && is_kernel_address()`
- 物理地址计算：`sg_bss_phdr->p_paddr + (vaddr - sg_bss_phdr->p_vaddr)`

---

## 7. 执行时序对比

### 7.1 v2.0 时序

```
bootloader → init.elf:
  Phase 1: heap + console
  Phase 2: basic_allocator→page_allocator
  Phase 3a: kernel.elf keep-end 加载，BSS 独立
  Phase 3b: 恒等映射 + 辅助区间分配
  Phase 4: 构造 init_to_kernel_header
  Phase 4.5: relinquish → pages_arr → shift_kernel
  ↓
kernel.elf:
  very_early_init: 解包信息包
  mem_init: KspacePML4 重建 + KImage_map_rebuild（pbase+offset 计算 PA）
  kimg_affiliate_property_map1: 辅助区间映射
  CR3 切换 → properties_modify_stage1 → multi_heap_enable
```

### 7.2 v2.1 时序

```
bootloader → init.elf:
  Phase 1-2: 同 v2.0
  Phase 3a: kernel.elf 瞬态端加载，0x100 段 probe_keep 独立分配，p_paddr 写回
  Phase 3b: 恒等映射 + 辅助区间 + conjunc_GSs + hdstacks（逐栈跳过 guard）
  Phase 4: 构造 init_to_kernel_header（kBSS_interval=0，kernel_entry_stack=0）
  Phase 4.5:
    ├── relinquish → pages_arr
    ├── CR3 ← KMMU root table
    ├── 遍历所有处理器填充 GDT/TSS
    ├── gs_complex_load_gdt_tss(BSP)
    └── init_jump_to_kernel(entry, BSP.tss.rsp0)  ← iretq
  ↓
kernel.elf:
  very_early_init: 解包信息包（kBSS_interval=0，无影响）
  self_introspection_init(): 从 kIMG_self_window 扫 BSS 程序头缓存
  mem_init:
    KImage_map_rebuild: 扫描程序头表，p_paddr 即 PA，is_kernel_address 过滤
    kimg_affiliate_property_map1: hw_stacks 逐栈映射（跳过 guard）
    CR3 切换 → properties_modify_stage1: 4 资产转生 + kIMG_self_window 自身
    multi_heap_enable 后: 补映射非内核地址段（TLS/ap_bootstrap）
```

---

## 8. 关键文件变更清单

| 文件 | v2.0 → v2.1 变更 |
|------|------------------|
| `kld.ld` | 0x100 FLAGS + `ALIGN(2M)` → text_main/rodata/bss |
| `src/init/init_init.cpp` | Phase 3a 重写：瞬态端 + 0x100 独立分配 + p_paddr 改写 + 拷贝补齐 |
| `src/init/info_fill.cpp` | `kBSS_interval = {}`; `kernel_entry_stack = {}` |
| `src/init/init_phase_ctx.h` | `ctx_kernel_loaded` 移除 `kBSS_interval`; `ctx_intervals` 移除 `kernel_entry_stack` |
| `src/include/arch/x86_64/boot.h` | 新增 `hdstacks_interval_*` 三个字段 |
| `src/include/arch/x86_64/abi/GS_complex.h` | `per_processor_hardware_stack_t` 含 guard 页 |
| `src/arch/x86_64/boot/mem_init.cpp` | KImage_map_rebuild 重写、hw_stacks 逐栈映射、资产转生 lambda 化、非内核段补映射 |
| `src/arch/x86_64/boot/mem_init.h` | 移除 `kBSS_interval` extern |
| `src/arch/x86_64/boot/kinit.cpp` | 适配 `self_introspection_init()` 无参数 |
| `src/KImage_Introspection.cpp` | 215→58 行，10 接口→2 接口 |
| `src/firmware/acpimgr.cpp` | `Kspace_pinterval_alloc_and_map` 替代两步手动映射 |

---

## 9. 与 v2.0 的向后兼容性

- `init_to_kernel_header` 字段兼容：`kBSS_interval` = 0，`kernel_entry_stack` = 0
- kernel.elf 侧读取 0 区间时跳过（`npages==0 || ppn==0`），不需立即修改
- `p_paddr` 语义变化：v2.0 的 `p_paddr` = `p_vaddr`（占位符），v2.1 中被覆盖为真实 PA
- kernel.elf 需要 ELF 程序头表扫描能力（`KImage_map_rebuild` 已具备）
