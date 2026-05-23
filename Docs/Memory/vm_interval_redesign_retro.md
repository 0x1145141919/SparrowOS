# vm_interval 设计债务 — 诊断与修复议程

日期：2026-05-18 ~ 20
决策者：Raven + 你
状态：Phase 1 完成（结构体定义已改），Phase 2.1/2.2 完成，Phase 2.3 进行中

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

### 结构体本身（现状 — 实际实现）

```cpp
struct vm_interval {
    uint64_t vpn;      // Virtual Page Number: (vaddr >> 12)
    uint64_t ppn;      // Physical Page Number: (paddr >> 12)
    uint64_t npages;   // Number of 4KB pages
    pgaccess access;

    // 辅助方法
    vaddr_t   vbase()    const;   // 右对齐提取低 52bit 后左移 12
    phyaddr_t pbase()    const;   // 同上
    uint64_t  byte_cnt() const;   // npages << 12

    bool is_kernel_address() const;
    bool vaddr_belong(vaddr_t addr) const;
    bool paddr_belong(phyaddr_t addr) const;
    seg_to_pages_info_pakage_t to_pages_info() const;
};
```

> **注：** `vbase()` / `pbase()` 命名沿用旧字段名，保持调用处自然的"基址"语义。
> `vaddr()` / `paddr()` 方案在评审中确认更简洁但实际落地时保留了 `vbase/pbase`。

**低位掩码处理：** `vbase()` 和 `pbase()` 内部对 `vpn / ppn` 做了 `& 0x000FFFFFFFFFFFFFULL`
掩码提取低 52bit，允许高 12bit 被模块用于打 tag（如 NVMe SQ ID 等）。

### 资产规模（更新至 Phase 2.3 进展）

- **7 个头文件** 定义持久化 `vm_interval` 成员
- **9 个模块** 内嵌 `vm_interval` 资产（NVMe、DMAR、IOAPIC、PCIe、HPET 等）
- **25+ 个 .h / 30+ 个 .cpp** 涉及 `.vbase` / `.pbase` / `.size` 字段读写
- **11 个全局变量** 跨 init.elf ↔ kernel.elf 信息包传递
- **4 个独立调用路径** 进入 `enable_VMentry()` / `disable_VMentry()`

### 具体模块清单（已迁移状态）

| 模块 | 文件 | 成员数 | 迁移状态 |
|------|------|--------|---------|
| init_to_kernel_header | `abi/boot.h` | 9 | ✅ Phase 2.3 |
| x86_specify_info | `arch/x86_64/boot.h` | 2 | ✅ Phase 2.3 |
| 内核全局（mem_init） | `arch/x86_64/mem_init.{h,cpp}` | 12 | ✅ Phase 2.3 |
| PCIe ECAM | `arch/x86_64/PCIe/prased.h` | 1 | ✅ Phase 2.2 |
| DMAR | `core_hardwares/DMAR.h` | 2 | ✅ Phase 2.2 |
| IOAPIC | `core_hardwares/ioapic.h` | 1 | ✅ Phase 2.2 |
| NVMe | `core_hardwares/NVMe.h` | 8+ | ✅ Phase 2.2 |
| NVMe_surface | `NVMe/NVMe_surface.h` | 10 | ✅ Phase 2.2 |
| HPET | `core_hardwares/HPET.h` | 1 | ✅ Phase 2.2 |
| KImage_Introspection | `KImage_Introspection.{h,cpp}` | 2 | ⏳ 待更新 |
| i8042 | `core_hardwares/i8042.h` | 3 | ✅ Phase 2.2 |
| ACPI_mgr | — | 1 | ✅ Phase 2.2 |
| kpoolmemmgr_HCBv3 | `memory/kpoolmemmgr_HCBv3.cpp` | 4 | ✅ Phase 2.2 |

### 消费端代码模式（迁移前 — 作为历史记录）

1. **`alignup_and_shift_right(size, 12)`** — mem_init.cpp 4 处（已清）
2. **`.size * 0x1000` / `<< 12`** — out_surfaces.cpp 3 处，驱动文件 N 处（已清）
3. **`% 0x1000` 对齐校验** — KspacMapMgr 2 处，kernel_mmu 1 处，out_surfaces 3 处（已清）
4. **直接传 `.size` 给字节级 alloc/memcpy** — properties_modify_stage1 3 处（已改为 `.byte_cnt()`）

## 4. 修复决策（最终版）

### 最终方案：字段名 + 语义双重升格

```cpp
struct vm_interval {
    uint64_t vpn;    // Virtual Page Number: (vaddr >> 12)
    uint64_t ppn;    // Physical Page Number: (paddr >> 12)
    uint64_t npages; // Number of 4KB pages
    pgaccess access;
    // 辅助方法
    vaddr_t   vbase()    const;
    phyaddr_t pbase()    const;
    uint64_t  byte_cnt() const;
};
```

### 设计原则

1. **存储态 = 页框号**，运行时加法/索引用 `vbase()` / `pbase()` 提取字节地址
2. **释放低位冗余**：`vpn` 低 52bit 为页号，高 12bit 允许模块打 tag
3. **`npages` 单位自明**：不再有"是不是字节"的猜测负担
4. **`loaded_VM_interval` 保持旧字段不变**：`{pbase, vbase, size, id, access}`，独立于 vm_interval
5. **`vinterval`（kernel_mmu.h）保持字节不变**：`{phybase, vbase, size}`，map 参数，不是资产描述子
6. **辅助方法命名为 `vbase()/pbase()`** 而非 `vaddr()/paddr()`，与旧字段名一致

### 否决的方案

- **不动字段名，只注释语义**：无法通过编译器强制执行新契约
- **新造 `asset_descriptor`**：过度工程，现有接口只需内联辅助方法即可适配
- **`PAGE_SHIFT` 抽象**：前置优化，SparrowOS 当前仅 x86_64

## 5. 修复议程（更新至实际进度）

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
│ kernel_mmu::map      │    │ kpoolmemmgr_HCBv3     │    │ KImage_Introspection │
│ PhyAddrAccessor      │    │ mem_kshell_commands   │    │ (待清理)             │
│                      │    │ KImage_Introspection  │    │                      │
└──────────────────────┘    └──────────────────────┘    └──────────────────────┘
```

#### 2.1 核心接口层 ✅

目标：`enable_VMentry` / `disable_VMentry` / `Kspace_phyaddr_direct_map` / `kernel_mmu::map`
内部将 `.vpn / .ppn / .npages` 还原为字节后再走现有页表逻辑。

改动特征：
- 入口参数类型不变（`const vm_interval&`）
- 内部的 `.end = vbase + size` → `.end = vbase() + byte_cnt()`
- 删除 `size % 0x1000` 对齐校验（`npages > 0` 即可）

附加成果：
- `phyaddr_direct_map` 接口三元化（`Kspace_phyaddr_direct_map` / `Kspace_pinterval_alloc_and_map` / `Kspace_phyaddr_direct_unmap`）
- `PhyAddrAccessor` 三阶段重构（not_ready → stage1 → stage2，LFU 缓存）
- `vm_interval::to_pages_info()` / `vaddr_belong()` / `paddr_belong()` / `is_kernel_address()` 成员函数

#### 2.2 运行时模块层 ✅

目标：各驱动模块中持久化 `vm_interval` 成员的构造和使用。

改动特征：
- `.pbase = mmio_base` → `.ppn = mmio_base >> 12`
- `.size = bytes` → `.npages = bytes >> 12`
- `.vbase = 0`（动态分配）→ `.vpn = 0`
- `interval.vbase + offset` → `interval.vbase() + offset`
- `Kspace_phyaddr_direct_map` 替代 `phyaddr_direct_map`

#### 2.3 初始化资产层 🔴 进行中

目标：`init_to_kernel_header` 中 9 个 `vm_interval` 资产的生产、转运、消费。

改动特征：
- `init_init.cpp` 中赋值 `{vpn, ppn, npages, access}` —— `>> 12` 转换
- `info_fill.cpp` 中 `h->xxx = ctx->xxx` —— 结构体赋值兼容，参数从 extern 全局改为 `ctx_kernel_loaded*` / `ctx_intervals*`
- `kinit.cpp` / `mem_init.cpp` 中消费端直接取 `.npages` 不再 `alignup_and_shift_right`

已知残留风险：
- `KImage_Introspection.cpp` 内部仍使用旧字段名（`.vbase`），需改为 `.vbase()` 或 `.vpn`

## 6. 相关文件索引（更新）

| 文件 | 角色 | 迁移状态 |
|------|------|---------|
| `src/include/memory/memory_base.h` | vm_interval 定义 + 辅助方法 | ✅ Phase 1 |
| `src/include/memory/AddresSpace.h` | enable_VMentry / Kspace_* 声明 | ✅ Phase 2.1 |
| `src/include/abi/boot.h` | init_to_kernel_header 资产 + `movable_file_entry_t` | ✅ Phase 2.3 |
| `src/include/arch/x86_64/boot.h` | x86_specify_init_to_kernel_info（含 conjunc_GSs） | ✅ Phase 2.3 |
| `src/include/arch/x86_64/mem_init.h` | 内核全局 vm_interval extern | ✅ Phase 2.3 |
| `src/include/init/init_phase_ctx.h` | 阶段交付上下文（NEW） | ✅ Phase 2.3 |
| `src/init/init_init.cpp` | 生产端赋值（Phase 2.3 产物） | ✅ Phase 2.3 |
| `src/init/info_fill.cpp` | header 构建 + 转运 | ✅ Phase 2.3 |
| `src/arch/x86_64/boot/kinit.cpp` | 消费端暂存、very_early_init | ✅ Phase 2.3 |
| `src/arch/x86_64/boot/mem_init.cpp` | 消费端消化 + 全链路调用 | ✅ Phase 2.3 |
| `src/memory/out_surfaces.cpp` | Kspace_* 实现 | ✅ Phase 2.1 |
| `src/memory/arch/x86_64/KspacMapMgr.cpp` | enable_VMentry 实现 | ✅ Phase 2.1 |
| `src/memory/kpoolmemmgr_HCBv3.cpp` | HCB_v3 在线/离线 | ✅ Phase 2.2 |
| `src/init/kernel_mmu.cpp` | kernel_mmu::map（vinterval 不受影响） | ✅ Phase 2.1 |
| `src/KImage_Introspection.cpp` | ELF 自省（内部 `KImage.vbase` 待修） | ⏳ |
| `Docs/init_v2/init_protocal_v2_initelf_specification.md` | 协议规范（含 ctx 交付范式） | ✅ 已同步 |

## 7. 附产结构体

### `movable_file_entry_t`（新增，Phase 2.3）

```cpp
struct movable_file_entry_t {
    vm_interval interval;  // 物理/虚拟区间（页对齐）
    uint64_t offset;       // 文件在区间内的偏移量
    uint64_t size;         // 文件原始字节数（可非对齐）
};
```

用于 `symtable_file` 和 `initramfs_file`，替代直接用 `vm_interval` 表示文件区间的方式。
`interval.byte_cnt()` 是页对齐的映射大小，`size` 是文件实际大小，二者可能不一致。

### `vm_interval_payload`（新增）

```cpp
struct vm_interval_payload {
    vm_interval interval;
    uint64_t is_fixed_property:1;
};
```

用于标识区间是否具有固定属性（在特定上下文下不可变）。

### `vphypair_t` — 已删除

```cpp
struct vphypair_t {
    vaddr_t   vaddr;
    phyaddr_t paddr;
    uint32_t  size;
};
```

定位：`src/include/memory/memory_base.h`。全库 0 引用，Phase 1 清理时删除。

## 8. Phase 2 实际成果总结

| 里程碑 | 日期 | 内容 |
|--------|------|------|
| Phase 1 | 5/18 | vm_interval 定义变更 + 辅助方法 + 全库编译爆破 |
| Phase 2.1 | 5/19 | 核心接口层（enable_VMentry、AddressSpace、out_surfaces、PhyAddrAccessor） |
| Phase 2.2 | 5/19 | 运行时模块层（NVMe/DMAR/IOAPIC/PCIe/HPET/i8042/ACPI/HCBv3） |
| Phase 2.3 🏗️ | 5/20 | 初始化资产层（init_init.cpp → info_fill.cpp → kinit.cpp → mem_init.cpp） |
| 堆替换 | 5/20 | `heap_alloc.h` (INIT_HCB) → `init_heap_v3.h` (BCB-based) |
| 上下文范式 | 5/20 | 全局变量消除 → ctx_early_mem / ctx_kernel_loaded / ctx_intervals 交付 |

---

*本文档随修复进度更新。*
