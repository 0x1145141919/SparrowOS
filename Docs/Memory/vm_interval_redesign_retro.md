# vm_interval 设计债务 — 诊断与修复议程

日期：2026-05-18 ~ 19
决策者：Raven + 你
状态：Phase 1 完成（结构体定义已改），后续按议程推进

---

## 1. 症状发现

源起于规格文档 `init_protocal_v2_initelf_specification.md` 审查期间，
`init_to_kernel_header` 中 `vm_interval.size` 字段的语义模糊引起警觉。

**不安感**：每次消费端使用 `.size` 时都要自己做 `align_up(..., 4096)` 或
`alignup_and_shift_right(..., 12)`，证明了生产端和消费端对 `.size`
的内涵认知不一致。

## 2. 根因分析

`vm_interval` 最初设计时默认 `.size` = 字节数，且 `.vbase` / `.pbase` 是字节地址。

但 `KspacePageTable::enable_VMentry()` 对此有硬约束：

```cpp
if (interval.size % 0x1000 != 0) return FAIL;  // 必须是页对齐
```

这个底层页表函数的要求向上泄露，迫使所有生产端保证 `.size` 是页对齐的字节数。
同时，信息包需要传递文件大小（`kelf_sz`、`sym_sz`）等原始字节数，
导致 `size` 字段在不同场景下承载**三种不同语义**：

| 语义 | 实例 | 后果 |
|------|------|------|
| 映射大小（页对齐字节） | `log_buffer`, `FPA_bitmaps` | 正常 |
| 文件原始大小（非对齐字节） | `kIMG_self_window`, `symtable_file` | 消费端手动补齐 |
| 硬编码固定值 | `hpet_mmio = 0x1000` | 偶然正确 |

消费端（`mem_init.cpp`）被迫使用 `alignup_and_shift_right(size, 12)`
作为掩码补丁，进一步模糊了 API 契约。

## 3. 影响范围

### 结构体本身

```cpp
struct vm_interval {    // 原设计
    vaddr_t   vbase;    // 字节地址，低 12 位必然为 0
    phyaddr_t pbase;    // 字节地址，低 12 位必然为 0
    uint64_t  size;     // 字节数，语义模糊
    pgaccess  access;
};
```

### 资产规模

- **7 个头文件** 定义持久化 `vm_interval` 成员
- **9 个模块** 内嵌 `vm_interval` 资产（NVMe、DMAR、IOAPIC、PCIe、HPET 等）
- **25+ 个 .h / 30+ 个 .cpp** 涉及 `.vbase` / `.pbase` / `.size` 字段读写
- **11 个全局变量** 跨 init.elf ↔ kernel.elf 信息包传递
- **4 个独立调用路径** 进入 `enable_VMentry()` /  `disable_VMentry()`

### 具体模块清单

| 模块 | 文件 | 成员数 | 用途 |
|------|------|--------|------|
| init_to_kernel_header | `abi/boot.h` | 9 | 信息包资产传递 |
| x86_specify_info | `arch/x86_64/boot.h` | 1 | HPET MMIO |
| 内核全局（mem_init） | `arch/x86_64/mem_init.{h,cpp}` | 11 | 各类映射区间 |
| PCIe ECAM | `arch/x86_64/PCIe/prased.h` | 1 | ECAM 配置空间 |
| DMAR | `core_hardwares/DMAR.h` | 2 | DMA remap 寄存器 |
| IOAPIC | `core_hardwares/ioapic.h` | 1 | IOAPIC 寄存器 |
| NVMe | `core_hardwares/NVMe.h` | 8+ | BAR/SQ/CQ/HMB |
| NVMe_surface | `NVMe/NVMe_surface.h` | 10 | 同上业务层 |
| HPET | `core_hardwares/HPET.h` | 1 | 仅构造参数 |
| KImage_Introspection | `KImage_Introspection.{h,cpp}` | 2 | 函数参数 |

### 消费者代码模式（4 种掩码补丁）

1. **`alignup_and_shift_right(size, 12)`** — mem_init.cpp 4 处
2. **`.size * 0x1000` / `<< 12`** — out_surfaces.cpp 3 处，驱动文件 N 处
3. **`% 0x1000` 对齐校验** — KspacMapMgr 2 处，kernel_mmu 1 处，out_surfaces 3 处
4. **直接传 `.size` 给字节级 alloc/memcpy** — properties_modify_stage1 3 处

## 4. 修复决策

### 最终方案：字段名 + 语义双重升格

```cpp
struct vm_interval {
    uint64_t vpn;    // Virtual Page Number: (vaddr >> 12)
    uint64_t ppn;    // Physical Page Number: (paddr >> 12)
    uint64_t npages; // Number of 4KB pages
    pgaccess access;

    // 辅助方法
    vaddr_t   vaddr()    const;
    phyaddr_t paddr()    const;
    uint64_t  byte_cnt() const;
};
```

### 设计原则

1. **存储态 = 页框号**，运行时加法/索引用 `vaddr()` / `paddr()` 提取
2. **释放低位冗余**：`vpn` 低 52bit 为页号，高 12bit 允许模块打 tag
3. **`npages` 单位自明**：不再有"是不是字节"的猜测负担
4. **`loaded_VM_interval` 同级改造**：同字段同语义
5. **`vinterval`（kernel_mmu.h）保持字节不变**：map 参数，不是资产描述子
6. **4KB 硬编码粒度**：不搞 `PAGE_SHIFT` 抽象，跨 ISA 时再处理

### 否决的方案

- **不动字段名，只注释语义**：无法通过编译器强制执行新契约
- **新造 `asset_descriptor`**：过度工程，现有接口只需内联辅助方法即可适配
- **`PAGE_SHIFT` 抽象**：前置优化，SparrowOS 当前仅 x86_64

## 5. 修复议程

### Phase 1 ✅ 已完成 — 结构定义改动

- 文件：`src/include/memory/memory_base.h`
- 内容：`vm_interval` 字段名 + 语义变更 + 内联辅助方法
- 效果：全库编译爆破，暴露所有涉及点

### Phase 2 — 分 3 步修复

```
2.1 内存核心接口层             2.2 运行时模块层             2.3 初始化资产层
┌──────────────────────┐    ┌──────────────────────┐    ┌──────────────────────┐
│ KspacePageTable      │    │ NVMe/BAR/SQ/CQ/HMB   │    │ init_to_kernel_header│
│   ::enable_VMentry   │    │ DMAR                  │    │ boot.h               │
│   ::disable_VMentry  │    │ IOAPIC                │    │ x86_64/boot.h        │
│ AddressSpace         │    │ PCIe ECAM             │    │ init_init.cpp 生产   │
│   ::enable_VM_desc   │    │ HPET                  │    │ info_fill.cpp 转运   │
│   ::disable_VM_desc  │    │ i8042                 │    │ kinit.cpp 暂存       │
│ out_surfaces 系列    │    │ ACPI_mgr              │    │ mem_init.cpp 消化    │
│ kernel_mmu::map      │    │ kpoolmemmgr_HCBv3     │    │                      │
└──────────────────────┘    │ mem_kshell_commands   │    └──────────────────────┘
                            │ KImage_Introspection   │
                            └──────────────────────┘
```

#### 2.1 核心接口层

目标：`enable_VMentry` / `disable_VMentry` / `phyaddr_direct_map` / `kernel_mmu::map`
内部将 `.vpn / .ppn / .npages` 还原为字节后再走现有页表逻辑。

改动特征：
- 入口参数类型不变（`const vm_interval&`）
- 内部的 `.end = vbase + size` → `.end = vaddr() + byte_cnt()`
- 删除 `size % 0x1000` 对齐校验（`npages > 0` 即可）

#### 2.2 运行时模块层

目标：各驱动模块中持久化 `vm_interval` 成员的构造和使用。

改动特征：
- `.pbase = mmio_base` → `.ppn = mmio_base >> 12`
- `.size = bytes` → `.npages = bytes >> 12`
- `.vbase = 0`（动态分配）→ `.vpn = 0`
- `interval.vbase + offset` → `interval.vaddr() + offset`
- `phyaddr_direct_map(&interval, ...)` 不受影响（内部已适配 2.1）

#### 2.3 初始化资产层

目标：`init_to_kernel_header` 中 9 个 `vm_interval` 资产的生产、转运、消费。

改动特征：
- `init_init.cpp` 中赋值 `{vpn, ppn, npages, access}` —— `>> 12` 转换
- `info_fill.cpp` 中 `h->xxx = g_xxx` —— 结构体赋值兼容
- `kinit.cpp` `very_early_init` 中 `copy = transfer->xxx` —— 同上
- `mem_init.cpp` 中消费端直接取 `.npages` 不再 `alignup_and_shift_right`

## 6. 相关文件索引

| 文件 | 角色 |
|------|------|
| `src/include/memory/memory_base.h` | vm_interval 定义（已改） |
| `src/include/memory/AddresSpace.h` | enable_VMentry / disable_VMentry 声明 |
| `src/include/abi/boot.h` | init_to_kernel_header 资产 |
| `src/include/arch/x86_64/boot.h` | x86_specify_init_to_kernel_info |
| `src/include/arch/x86_64/mem_init.h` | 内核全局 vm_interval extern |
| `src/memory/arch/x86_64/KspacMapMgr.cpp` | enable_VMentry / disable_VMentry 实现 |
| `src/memory/out_surfaces.cpp` | phyaddr_direct_map / alloc/vfree 系列 |
| `src/init/kernel_mmu.cpp` | kernel_mmu::map（vinterval 不受影响） |
| `src/init/init_init.cpp` | 生产端赋值（Phase 3 产物） |
| `src/init/info_fill.cpp` | header 构建 |
| `src/arch/x86_64/boot/kinit.cpp` | 消费端暂存 |
| `src/arch/x86_64/boot/mem_init.cpp` | 消费端消化 + 全链路调用 |
| `Docs/init_v2/init_protocal_v2_initelf_specification.md` | 协议规范 |

---

## 附：会话中发现的其他结构体观察（你认领，不做改动）

### `vphypair_t` — 死代码，全库 0 引用

```cpp
struct vphypair_t {
    vaddr_t   vaddr;      // 字节地址
    phyaddr_t paddr;      // 字节地址
    uint32_t  size;       // 字节，uint32_t 上限 4GB
};
```

定位：`src/include/memory/memory_base.h`。建议删除。

### `VM_DESC.SEG_SIZE_ONLY_UES_IN_BASIC_SEG` — 名字差但语义不差

```cpp
struct VM_DESC {
    vaddr_t start;
    vaddr_t end;
    phyaddr_t phys_start;
    ...
    uint64_t SEG_SIZE_ONLY_UES_IN_BASIC_SEG;  // typo: USES
};
```

历史：`MemoryModuleDocv3.1.txt` 时代对是否访问低位内存摇摆不定的残留。
v4 时期决策明确后，`identity_map_window` 作为信息包一等字段传递，
`PhyAddrAccessor` 初始化时序改为从 `mem_init.cpp` 获取该字段，
`SEG_SIZE_ONLY_UES_IN_BASIC_SEG` 的边界守卫职责自然由 `byte_cnt()` 替代，
该字段已可扬弃。字段名 typo 是当年命名草率的印记。

`VM_DESC` 的角色不同于 `vm_interval`：后者描述物理资产+完整 VA 映射，
前者描述 VA 区间 + 可选物理/文件/匿名映射。仍有其他字段可去冗余，
但已不是一天能完成的改造。

### `BootInfoHeader` 跨域裸指针

`gST_ptr`、`pass_through_devices`、`loaded_files`、`memory_map_ptr` 均为裸指针。
v2 已修复 `init_to_kernel_header` 内的指针问题，但 `BootInfoHeader`（UEFI bootloader → init.elf）
尚未统一。是否需要改用偏移量取决于 bootloader 代码是否在控制范围内。

### `mem_interval`（init.elf 侧）

```cpp
struct mem_interval { uint64_t start; uint64_t size; };
```

和旧 `vm_interval` 同样的低 12bit 冗余，但使用范围仅限于 init.elf，影响面小。
建议等 `vm_interval` 改造稳定后视情况处理。

---

_本文档随修复进度更新。_
