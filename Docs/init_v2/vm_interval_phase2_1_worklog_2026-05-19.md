# 工作日志 2026-05-19

## vm_interval 重构 — Phase 2.1 核心接口层改动

### 背景

`vm_interval` 结构体字段从 `{vbase, pbase, size}` 变更为 `{vpn, ppn, npages}`，
字节地址改为页框号，消除「size 是字节还是页数」的语义模糊。

**阶段角色**：本文档记录 Phase 2.1（核心接口层）的改动日志。

---

### 改动清单

#### 1. `KspacePageTable::enable_VMentry` / `disable_VMentry`

- **文件**：`src/memory/arch/x86_64/KspacMapMgr.cpp`
- 入参类型保持 `const vm_interval&`
- 内部 `interval.vbase` → `interval.vaddr()`
- `interval.pbase` → `interval.paddr()`
- `interval.size` → `interval.byte_cnt()`
- 删除 `% 0x1000` 对齐校验（`vaddr()/paddr()/byte_cnt()` 天然页对齐）
- 仅保留 `interval.npages == 0` 判空

#### 2. 新增 `vm_interval_to_pages_info(vm_interval)` 重载

- **文件**：`src/include/memory/memory_base.h`（声明），`src/utils/util.cpp`（实现）
- 参数：`vm_interval`（值传）
- 内部构造临时 `VM_DESC adjusted = {.start = interval.vaddr(), .end = ..., .phys_start = ...}`
- 委托给已有 `vm_interval_to_pages_info(VM_DESC)` 实现
- **目的**：调用方（如 `enable_VMentry`）不再需要手动构造 `VM_DESC` 中间层

#### 3. `AddressSpace::enable_low_half_vm_interval` / `disable_low_half_vm_interval`

- **文件**：`src/include/memory/AddresSpace.h`（声明），`src/memory/arch/x86_64/AddresSpace.cpp`（实现）
- 入参类型：`VM_DESC desc` → `vm_interval interval`
- `enable` 内部引入局部变量 `curr_vaddr`/`curr_paddr`/`end_vaddr` 处理 clamp 到 `ADDR_VM_BOTTOM` 逻辑
- 因 vm_interval 以页框号存储，不可直接改字段值，改为操作局部字节变量
- `vm_interval_to_pages_info` 调用传临时 `VM_DESC adjusted`（实现内部细节）
- `disable` 校验改用 `interval.vaddr()/paddr()`，无 clamp 逻辑

#### 4. 新增 `vm_interval::to_pages_info()` 成员函数

- **文件**：`src/include/memory/memory_base.h`（声明），`src/utils/util.cpp`（实现）
- 返回 `seg_to_pages_info_pakage_t`（值），不再通过输出参数
- 逻辑为现有 `vm_interval_to_pages_info(VM_DESC)` 的移植版，直接使用 `vaddr()/paddr()/byte_cnt()`
- 供 `enable_VMentry/disable_VMentry` 内部 TLB 自适应拆分使用

#### 5. 新增 `vm_interval::is_kernel_address()` 成员函数

- 检查 `vaddr()` 和 `vaddr()+byte_cnt()-1` 是否均通过 `is_addr_kernel_address()`
- 供后续 `Kspace_phyaddr_direct_map/unmap` 使用

#### 6. 新增 `vm_interval::vaddr_belong()` / `paddr_belong()` 成员函数

- 检查字节地址是否落在 `[vbase, vbase+byte_cnt)` / `[pbase, pbase+byte_cnt)` 区间内
- 供 `PhyAddrAccessor` 等使用

#### 7. 三接口改造：`phyaddr_direct_map` → `Kspace_*`

- **文件**：`src/include/memory/AddresSpace.h`（声明），`src/memory/out_surfaces.cpp`（实现）

| 旧接口 | 新接口 | 变化 |
|--------|--------|------|
| `phyaddr_direct_map(vm_interval*, KURD_t*)` → `vaddr_t` | `Kspace_phyaddr_direct_map(vm_interval)` → `KURD_t` | 固定 VA 模式，值传，返回 KURD |
| — | `Kspace_pinterval_alloc_and_map(vm_interval, KURD_t*)` → `vaddr_t` | vpn=0 动态分配，返回 VA |
| `phyaddr_direct_unmap(vm_interval*, uint64_t)` → `KURD_t` | `Kspace_phyaddr_direct_unmap(vm_interval)` → `KURD_t` | 值传，无 size 参数 |

**关键决策**：
- `Kspace_phyaddr_direct_map` 强制 `interval.is_kernel_address()`，vpn 必须非零
- `Kspace_pinterval_alloc_and_map` 强制 `vpn == 0`，内部分配 VA + 建映射
- `Kspace_phyaddr_direct_unmap` 强制 `interval.is_kernel_address()`

#### 8. `__wrapped_pgs_valloc` / `__wrapped_pgs_vfree` / `stack_alloc` 字段迁移

- **文件**：`src/memory/out_surfaces.cpp`
- 所有 `vm_interval` 构造从 `{.vbase=vaddr, .pbase=paddr, .size=bytes}` 改为 `{.vpn=vaddr>>12, .ppn=paddr>>12, .npages=pages}`

#### 9. `kpoolmemmgr_t::HCB_v3::online()` / `offline()` 字段迁移

- **文件**：`src/memory/kpoolmemmgr_HCBv3.cpp`
- 4 处 `vm_interval` 构造字段迁移；offline 中 bitmap 区间注释清理

#### 10. `PhyAddrAccessor` 三阶段重构

- **文件**：`src/include/memory/phyaddr_accessor.h`，`src/memory/PhyAddrAccessor.cpp`
- 引入三个阶段：
  - `not_ready`（`BASIC_interval.npages == 0`，静默 fail）
  - `stage1`（`BASIC_interval` 内可访问，外返回 0 / no-op）
  - `stage2`（`BASIC_interval` 优先 → `cache_tb[16]` 查找 → miss 返回 0）
- `BASIC_DESC` + `SEG_SIZE_ONLY_UES_IN_BASIC_SEG` 移除，替换为 `vm_interval BASIC_interval`
- `paddr_memcpy` 仅 stage1 可用，stage2 返回 false
- 缓存策略为 **LFU**（Least Frequently Used），`cache_touch` 递增频次
- `resolve_addr` 统一静态方法：PA→VA 按阶段决策

#### 11. `mem_domain` 设计（已回滚）

- 引入「内存域」概念（root domain + 可热插拔非根域），讨论后决定当前不实现
- 回滚所有 `mem_domain.h` / `mem_domain.cpp` 文件及 `buddy_alloc_params.domain_id` 字段
- 结论：SparrowOS 无 CXL/热插拔需求，保持单根域静态模型

#### 12. 驱动层调用迁移（共 9 处）

| 文件 | 改动 |
|------|------|
| `i8042.cpp` | 3 处 `phyaddr_direct_map` → `Kspace_pinterval_alloc_and_map` + 字段迁移 |
| `NVMe_init_and_shutdown.cpp` | 2 处 BAR 映射 + 所有 `admin_buffer`/`sq_ring`/`cq_ring`/`hmb_buffer` 字段迁移 |
| `io_queue_cmd.cpp` | 4 处 sq_ring/cq_ring 构造 + 清理字段迁移 |
| `PCIe/PCIe.cpp` | ECAM 构造字段迁移 + `Kspace_pinterval_alloc_and_map` |
| `DMAR.cpp` | regs_interval + interrupt_remmaptable_interval 字段迁移 |
| `ioapic.cpp` | regs_interval 字段迁移 |

### 待推进

- `boot/mem_init.cpp`（5 处 `phyaddr_direct_map` 调用 + 新字段）
- `mem_kshell_commands.cpp`（4 处）
- `init_init.cpp` / `info_fill.cpp` / `kinit.cpp` 的 `vm_interval` + `movable_file_entry_t` 适配
- `init_to_kernel_header` 中 `kIMG_self_size` 和 `movable_file_entry_t` 的全链路改造
- `pcie_kshell_commands.cpp` / `NVMe_kshell.cpp` 的 `.vbase` 字段访问更新

### 未解决的设计问题

- `movable_file_entry_t` 引入后，`init_to_kernel_header` 的 `symtable_file`/`initramfs_file` 从 `vm_interval` 变为 `movable_file_entry_t`，需全链路适配
- `init_init.cpp` 阶段上下文交付范式（见 [`init_protocal_v2_initelf_specification.md`](init_protocal_v2_initelf_specification.md)）
