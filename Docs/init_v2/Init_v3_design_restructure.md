# Init_v3 设计重构总述 — init.elf → kernel.elf 传递与内存初始化体系

> 状态：正式文档（Init_v3 设计重构大方向定稿）
> 基线对比：`8d0060ba`（纯血 v2）↔ 当前 master（v3，已通过 TCG + 实体机测试）
> 上游草稿：`Init_v3_draft.md` / `Init_v3_phase_3b_modify.md` /
>          `Init_v3_phase_4_pkg_migration.md` / `Init_v3_string_tag_container.md`
> 逐条决策的博弈上下文见 git log（本文档只写最终结论与思路演变）。

---

## 一、一句话总览

**v2 移交"页框数组容器"，kernel 消毒复活、从空白重造；v3 移交"物理事实状态账本"，kernel 收养编排、拓扑从状态投影重建。**

v3 的三大范式翻转：

| # | 维度 | v2（纯血） | v3（当前） |
|---|------|-----------|-----------|
| 1 | 传递协议 | 一等字段 + VM_ID 常量硬编码 | 字符串锚点资产注册表（add/read/deal 生命周期） |
| 2 | 内存账本 | `all_pages_arr` 复用页框数组缓冲但**消毒重造（复活）** + 污染模型 | `mem_map` 状态数组（1B/页）**原样穿越** + `page_frame_state_mgr` 收养权威账本 |
| 3 | 分配器拓扑 | 消毒后从 free_segs 派生 plan + `interval_pollute/clean` 冻结掩盖分配不可见 | BCB 收养脏叶子 + 构造即成年（`inherit_init` + `fold_up_from_leaves` 一步），拓扑从状态投影重建 |

--- 分章节说明 ---

## 二、v2 的四个结构性病灶（重构动机）

v2 在「BCB 初始完全纯洁 + 最小内存碎片」双重洁癖约束下，造出了四处恶性设计：

1. **同一批资产被映射两次**
   init.elf 在自建瞬态 `kernel_mmu` 页表上给每个资产 `kmmu->map`；
   kernel.elf 又在全新 `KspacePageTable` 里 `KImage_map_rebuild` + `kimg_affiliate_property_map1`
   把同一批物理区间重放一遍。

2. **同一份内容被拷贝两次**
   `properties_modify_stage1` 对 initramfs/symtable/log/kIMG 做"转生"：
   `FPA alloc 新页 → paddr_memcpy → 更新 ppn → 重映射 → 释放旧页`。
   转生的根因是「init 的分配对 kernel FPA 不可见」——FPA 从 free_segs 派生 plan 时
   根本不知道这些页已被 init 占用。

3. **污染模型是治标**
   `interval_pollute/clean` + `dirty_count` 是为掩盖「分配不可见」这一根因——v2 对跨过来的
   页框数组消毒重造（复活），init 的分配历史全部丢失，kernel 根本看不见哪些页已被 init 占用。
   `can_alloc` 见 `dirty_count != 0` 直接拒绝，等价于用"整段冻结"换"正确性"。

4. **legacy 页表黑洞**
   init 侧把 kmmu 自建页表区间填进 header `kmmu_interval`（专门设计成一个整段区间方便整段丢弃），
   但 kernel 侧 `legacy_mmu_interval` **从未从该字段赋值**，恒为 `{0,0}`；
   `interval_pollute/clean(legacy_mmu_interval)` 命中 `seg.size == 0` 直接 no-op——
   设计好的"整段丢弃"从未执行，init 的 KMMU 页表在 CR3 切走后成为死页表，物理页永久泄漏。

---

## 三、重构一：传递协议 — 一等字段 → 字符串锚点资产注册表

### v2 协议形态

`init_to_kernel_header` 每个资产一个一等字段：

```c
kIMG_self_window / kIMG_self_size / kBSS_interval / pages_arr / FPA_bitmaps /
log_buffer / kernel_entry_stack / symtable_file / initramfs_file /
Kspace_phyaddr_access_window / arch_specify_offset + 各 VM_ID 常量
```

新增一个资产 = 动 header + 动 info_fill + 动 very_early_init + 动各消费方。
资产身份靠隐式编号（VM_ID）或字段名约定，无统一生命周期。

### v3 协议形态

`init_to_kernel_header_v2` 只剩通用骨架 + 一张注册表：

```c
magic / self_pages_count
phymem_segment_count + phymem_segments(offset)          // 纯洁视图逐字拷贝
properties_count + properties_table(offset)             // asset_entry_t[]（扁平 desc 数组）
free_segs_count + free_segs_descriptors_table(offset)   // 索引式，无需重定位
logical_processor_count
```

命名资产统一为 **多arg name 字符串**（`"<arg0> <arg1>..."`）：
- `arg0` = 本名（树键 / 认领锚点），`arg1` = 路由类型 → `asset_route.h` 路由表定 `asset_kind`
- `asset_kind`：`mem`(vm_interval) / `movable`(movable_file_entry_t) / `blob` / `arch` / `scalar`
- 双端各自持有容器：init 侧 `init_asset_registry_t`（红黑树，arg0 为键，纯 add/read/remove），
  kernel 侧 `asset_table_t`（扁平数组 + 有效性位图，pour→read→deal→dispose 单向生命周期）
- 全名单一来源：`asset_names.h` 常量表（生产方/消费方引用同一常量，杜绝散落字符串）

**两级重链**：
- 一级（包内）：name 串 + `asset_entry_t[]` + desc blob 全部在包内，偏移相对包基址
- 二级（包外）：desc 存物理地址数值，kernel 经 `Kspace_phyaddr_access_window` 把 phys 重链成内核 VA

新增一个资产 = 生产方登记 + 消费方认领，各一行，header 不再膨胀。

---

## 四、重构二：内存账本 — free_segs 派生 → mem_map 状态数组 + page_frame_state_mgr

### v2 账本（复活 / 消毒重造）

- `all_pages_arr`：`mem_map`（`page[]`，1B/页）+ 从 EFI 链表解析的区间。
- 交接语义 = **复活**：init 侧 `page_allocator::relinquish_mem_map` 先把缓冲区清零，kernel 侧
  `all_pages_arr::Init` 再全量重置为 `reserved` 并从 phymem_segments 重新解析、重新规划——
  页框数组缓冲被复用，但分配历史完全打碎，kernel 拿到的是干净画布。
- FPA 初始化时 `all_pages_arr::free_segs_get()` 现扫空闲段 → kernel 侧**派生 BCB plan**。
- 因为消毒重造让 init 的分配对 kernel 不可见，才需要污染模型兜底。

### v3 账本（收养 / 编排穿越）

- init 侧 `page_allocator_v2` 的 `mem_map` 状态数组（1B/页，`page_state_t` 语义态）
  作为**权威物理账本整体穿越**（`pages_arr` movable 资产，纯物理描述符 `{base_ppn, size}`）。
- 穿越契约改为**索引式**：`free_seg_descriptor_t { in_pure_memview_idx, baseidx_in_memmap }`
  ——纯洁视图 `phymem_segments[]` 逐字拷贝 + 下标两世界解析一致，**无需重定位**。
- kernel 侧 `page_frame_state_mgr::adopt(pages_arr_file, free_segs, count)` 一次性构建
  `intervals[]`（二分定位表）+ 冻结 `mem_map`，成为唯一权威账本：
  - `state_set / state_query / kind_check`（物理地址型）与 `idx_base_*`（索引型）
  - `intervals_snapshot()` 供 FPA 全量区间重建拓扑
  - `early_alloc / early_alloc_close`：FPA 就绪前的初期分配窗口

**关键语义转变**：穿越的是"每页在做什么"的物理事实，不是"分配器怎么组织"的拓扑。
账本（mem_map）与拓扑（FPA/BCB）彻底分离——账本跨世界，拓扑各世界私有。

> 沿革：草稿（Init_v3_draft）曾主张「BCB_bitmaps 穿越 + kernel 收养位图」；实际决策
> （commit 5434800 记录的设计方反馈）是 pages_arr 比 BCB 更适合穿越——状态数组能看到
> 每页状态（语义信息比 1bit 空闲位全面），且从旧 page_allocator 小幅改动即可，泄露的
> 逻辑复杂度远小于现行 BCB（BCB 会泄露 plan 算法 / 位图布局契约 / 幼成年仪式 / crumbs
> 丢弃）。故 BCB_bitmaps 穿越被废弃，只穿状态数组，一切分配器拓扑由 kernel 从状态投影
> 自行重建——pages_arr 穿越让分配逻辑更简单。
---

## 五、重构三：分配管线 — page_allocator 双光标 → page_allocator_v2 单光标 + mem_map 唯一写入口

### v2 分配管线

`basic_allocator`（自举链表）→ `page_allocator`（瞬态端/持久端双光标 + `probe/probe_keep`）。
分配结果写自己的独立账本，与 kernel 侧账本脱节——这是"分配不可见"的源头。

### v3 分配管线

```
basic_allocator（自举，冻结不重构）
    ↓ get_pure_memory_view（纯洁视图，唯一物理内存描述表，升序固定不可变）
page_allocator_v2（单光标自下而上 first-fit，跨区间分配）
    ↓ free_ram_explore() 勘探 + pages_set() 提交占用（mem_map 唯一写入口）
mem_map 状态数组 → phase_3b 登记 pages_arr 资产 → phase_4.5 翻 free 自裁归还
```

要点：
- **单光标**：删除瞬态端/持久端分界，`scan_top_base` 移除。
- **`pages_set` 支持跨区间**：穿越后状态是权威账本，漏标即错，故提交语义必须先于一切分配动作。
- **"归还"就是翻状态**：phase_4.5 自裁归还 init 镜像/header 页 = `pages_set(_, free)`，
  替代旧 `init_bcb_juvenile::free`。
- 状态语义收紧：穿越只判 free / 非-free；`kernel_persisit / kernel_anonymous`
  双态区分删除，账本只表达占用事实（`page_state_t` 仍保留语义态供消费方覆写）。

---

## 六、重构四：BCB — 纯洁全空闲态 + 污染模型 → 收养脏叶子 + 构造即成年

### v2 的 BCB

- `pure_init`：整区位图清零 + 根 `NODE_FREE`，**全空闲成年态**。
- 配合"复活"语义：消毒后 kernel 的 BCB 从全空闲起跑，再用 `interval_pollute` 把已占用区间
  "冻结"补上 init 的分配（`dirty_count != 0` 时 `can_alloc` 一律拒绝）。
- 位图与物理现实的对齐靠"冻结"而非"写实"——格局小、语义脆。

### v3 的 BCB（收养 + 构造即成年）

- **幼年/成年态是中间 BCB 穿越设计的偶然复杂度**：草稿为「init 递交幼年位图、kernel 决断
  何时成年」构思了幼年态；状态数组穿越落地后运行时不再使用——`juvenile_alloc/free_order0`
  仅存于测试路径（`src/tests/BCB_juvenile_test.cpp`），属遗留。
- **现实路径 = 收养 + 构造即成年**：FPA 初始化拿 `intervals_snapshot()` 全量区间 → 自行分桶
  （`min_bcb_order` 碎片丢弃）→ BCB 3-arg 构造时**逐页查 `page_frame_state_mgr` 账本写实
  order-0 叶子位图**（允许脏叶子）→ 构造函数内 `inherit_init`（幼年态 + `free_count[0]`）+
  `fold_up_from_leaves()`（自底向上折叠，填内部节点 + 全 `free_count` 重算，`max_order >= 6`
  按 u64 字批量归并）→ **一步成年**。FPA::Init 尾部 `page_frame_state_mgr::early_alloc_close()`
  关闭初期分配窗口，运行时全部 BCB 处于成年态。`dirty_count` 污染机制整体删除。

> 一句话：v2 把"分配不可见"藏进冻结；v3 让分配对位图"看得见"，通过收养 + 构造内折叠
> 一步落到真实状态。
> 位图布局（非 v3 演进）：order-0 1bit 叶子区在 `[2<<N, 3<<N)` 由 BCB v4 foundation 文档
> （Docs/Memory/BCB_foundation.md 及其 v4draft，commit 9b48e0d，2026-05-15）早就确立；
> 但同日诞生的实现却把 `leaf_read/leaf_write` 落在 `[1<<N, 2<<N)`——实现从出生就偏离
> 自家文档，直至 commit ae41ad2 才把代码对齐回文档布局（并顺带为 u64 字批量归并铺平）。

---

## 七、重构五：映射哲学 — 双倍映射 / 转生 → 纯物理描述符 + 主窗口完全信任

### v2 的处理

所有资产都要"有 VA 才有命"：init 先 `kmmu->map`，kernel 再 `Kspace_phyaddr_direct_map`
重放一遍；纯内存资产还要走转生（alloc→copy→remap→free）。

### v3 的处理

| 资产类 | 处理方式 |
|--------|----------|
| 纯内存资产（fpa_bitmaps/pages_arr/log_buffer/ksymbols/initramfs/kimg） | 只传 `{base_ppn, size}` 纯物理描述符（movable），**不做 KMMU 映射**；kernel 经主窗口 `PHYACC_VA` 重链访问 |
| ISA 级资产（gs_complexes / hdstacks） | init 侧 KMMU 映射（BSP 跳转后即用 rsp0 栈，不可动）；kernel 侧 `assets_remap` 重映射 / 精细重映射 |
| arch MMIO 资产（hpet_mmio / gop_framebuffer） | init 侧 KMMU 映射 + kernel 侧重映射（现状保留） |
| 标量资产（xsdt_pbase） | 纯标量穿越，kernel `assets_remap` 落账 `g_xsdt_base` |

**主窗口完全信任**：`phyaddr_window`（`[0, dram_top) → 1GB 对齐高 VA`）在
`exec_env_prepare` 最早落账并 `PhyAddrAccessor::Init`，此后一切 phys→VA 换算
（page_frame_state_mgr 收养、ksymbols/log 重链、ELF 自省、低半段加载）统一走
`PHYACC_VA`，不再依赖 init 留下的 identity 残留。

**hdstacks 降级**：不再是"ISA 硬资产整体链入 TSS"，init 侧只粗映射 + 穿越物理区间；
kernel 侧 `remap_hdstacks` 先 `kspace_vm_table` 圈地整段 VA，再逐处理器逐栈精细映射，
**guard 页保持缺页**（栈溢出立即 #PF）——guard 的"不映射"铁律由 kernel 侧落实。

**低半段实加载**：ap_bootstrap 等低地址段（kld.ld init 区，0x4000 起）在 phase_3a 只存在于
kimg 文件缓冲，从未落到链接物理地址；v3 在 `load_low_half_segments` 中从 kimg 经主窗口
实拷贝到 `p_paddr` + `.bss` 清零 + `enable_low_half_vm_interval` 恒等映射——AP 经
INIT-SIPI-SIPI 实模式启动时 0x4000 内容才真实在位。

---

## 八、重构六：页表生命周期 — 设计了整段丢弃却未执行（黑洞）→ CR3 切换后 BFS 整棵回收

- v2 的设计意图：init 侧把 kmmu 自建页表的分配区间填进 header `kmmu_interval`
  （`kmmu->get_self_alloc_interval()`），专门留一个整段区间**就是为了方便整段丢弃**。
  但 v2 实际执行：kernel 侧 `legacy_mmu_interval` 声明了却从未从 `transfer->kmmu_interval`
  赋值，恒为 `{0,0}`；`interval_pollute/clean` 命中 `seg.size == 0` 直接 no-op——
  设计好的"整段丢弃"从未执行，init KMMU 页表在 CR3 切走后成为死页表，物理页永久泄漏。
- v3：CR3 切换前记录旧根表物理基址（`old_cr3`）；切换后 `bfs_delete_old_pagetable(old_cr3)`
  经主窗口 BFS 遍历旧页表（显式队列，深度不递归），**只回收页表页本身**（PML4/PDPT/PD/PT 表页，
  叶数据页归 FPA 管），逐页 `state_set(_, free)` 翻回账本。PT 层 Present 项全是 4KB 叶数据页，
  绝不入队。

---

## 九、重构七：启动流程 — kernel_start 巨函数 → 三阶段职责切分

### v2：`kernel_start` 一坨

```
very_early_init(transfer) → 焚包 → 输出子系统(GfxPrim/ksym/HPET/textconsole/serial/kout)
→ tsc_regist → Panic → mem_init(账本+FPA+映射+转生) → ACPI/APIC → 调度器 → AP 启动 → create_first_kthread
```

### v3：三阶段（asm 侧依次 call）

```
exec_env_prepare(pkg)   ← 无 kout 摸黑阶段，失败裸停机
    kpoolmemmgr Init → link 信息包 → phymem_segments 拷出 → asset_table create+pour
    → 早期 panic 支撑(phyaddr_window/ksymmanager/HPET) → 输出子系统(log_buffer/gop/gop_info)
    → page_frame_state_mgr::adopt → self_introspection_init

basic_init()            ← 内存主线
    tsc_regist → GlobalKernelStatus → Panic::will_check → mem_init()

kernel_start()          ← 调度器就绪前收尾
    ACPI(g_xsdt_base) → x2apic → 全局调度器数组 → AP 启动 → task_pool → 中断接管 → create_first_kthread
```

三阶段各司其职：**信息包消费与认领**（exec_env_prepare）→ **内存体系建立**（basic_init）→
**多核调度就绪**（kernel_start）。BSP 跳转栈由 `kernel_entry_stack` 改为 GS 复合体内嵌 rsp0，
随 GS 复合体一起由 init 侧就绪。
为什么如此设计，很明显初始化不同阶段的纪律是不同的，一个函数里面塞满，不如分函数分文件隔离更舒爽
---

## 十、逐项对照表

| 项 | v2（纯血） | v3（当前） |
|----|-----------|-----------|
| 传递载体 | `init_to_kernel_header` 一等字段 + VM_ID | `init_to_kernel_header_v2` 骨架 + `asset_entry_t[]` 注册表 |
| 资产身份 | 字段名 / VM_ID 编号约定 | 多arg 字符串锚点 + 路由表（asset_route.h）+ asset_names 单一来源 |
| 生命周期 | 无（一次性字段拷贝） | pour→read→deal→dispose（kernel）；add/read/remove（init） |
| 物理账本 | `all_pages_arr` mem_map 消毒重造（复活）+ kernel 侧 free_segs 派生 | `page_allocator_v2` mem_map 原样穿越 + `page_frame_state_mgr` 收养 |
| 穿越契约 | 绝对区间字段 | 索引式 free_seg_descriptor（纯洁视图下标，无需重定位） |
| 分配器 | basic_allocator → page_allocator（双光标） | basic_allocator → page_allocator_v2（单光标，mem_map 唯一写入口） |
| BCB 初始化 | pure_init 全空闲成年态 + interval_pollute 冻结 | 3-arg 构造逐页写实 order-0 位图（脏叶子允许）→ fold_up 成年 |
| 污染机制 | interval_pollute/clean + dirty_count | 删除 |
| 资产映射 | 纯内存资产也 kmmu->map + kernel 重放 | 纯内存资产只传物理描述符，经主窗口 PHYACC_VA 访问 |
| 内容拷贝 | properties_modify_stage1 转生四资产 | 删除（无转生，资产已在账本正确占位） |
| 页表归还 | kmmu_interval 设计了整段丢弃却从未被消费（黑洞） | bfs_delete_old_pagetable BFS 整棵回收页表页 |
| 启动流程 | kernel_start 巨函数 | exec_env_prepare / basic_init / kernel_start 三阶段 |
| 标量穿越 | arch_specify_offset 硬编码 | xsdt_pbase scalar 资产 |

---

## 十一、删除清单 vs 新增清单

**删除（v2 → v3）**
- `properties_modify_stage1`（转生）
- `interval_pollute` / `interval_clean` + `dirty_count` 污染机制
- `legacy_mmu_interval`（设计用于整段丢弃 kmmu 页表，实际从未被赋值/消费，黑洞）
- kernel 侧 `FPA::Init` 从 free_segs 派生 plan + `memory_crumbs` 碎片记账
- init 侧纯资产 VA 分配与映射（纯资产分支 `va_alloc_up` / per-asset `kmmu->map`）
- `kIMG_self_window` 的 KMMU 映射（kimg 改 movable 纯物理描述符）
- `init_bcb_juvenile`（init 侧 BCB 幼年分配器，被 page_allocator_v2 顶替）
- `kernel_entry_stack` 资产（BSP 跳转改 GS 复合体 rsp0）

**新增（v3）**
- `page_allocator_v2`（mem_map 状态数组 + 单光标 + pages_set 唯一写入口）
- `page_frame_state_mgr`（权威物理账本 + intervals + early_alloc）
- 资产注册表体系：`asset_entry_t` / `asset_route.h` / `asset_names.h` /
  init 侧 `init_asset_registry_t` / kernel 侧 `asset_table_t`
- BCB 收养 + 构造内 `fold_up_from_leaves` 成年（含 order-6 u64 字批量归并；幼年接口遗留测试）
- `bfs_delete_old_pagetable`（老页表 BFS 回收）
- `remap_hdstacks`（guard 页不映射的精细重映射）
- `load_low_half_segments`（低半段实拷贝 + 恒等映射）
- `xsdt_pbase` 标量资产（g_xsdt_base 穿越落账）
- 启动三阶段：`basic_init` 独立文件 / `kernel_start` 恢复实体 / asm 调用链更新

---

## 十二、验证与遗留

### 已验证

- TCG（软件模拟）与实体机均已通过启动全链路（信息包穿越 → 页框收养 → FPA 收养 +
  成年仪式 → 低半段加载 → 调度器就绪）。
- `init.ld` `INIT_BASE` 移至 `0x101000000`：固件更新后 `0x100000000~0x100ffffff` 被固件占用，
  实体机过测必需（记录见 commit `ad02f23`）。

### 遗留（后续方向）

- 业务代码中部分 KURD 仍以空占位（`result_code::FAIL`）返回，等待模块错误树统一编排升格。
- `i8042` `readonly_view` 当前被旁路（ring 直读），机制待回收。
- 成年仪式全量折叠的性能优化空间（当前 O(位图) 一次，超大规模再优化）。
- 资产注册表运行期 CRUD（create/update/remove）尚未全部开放；当前以 boot 认领为主。
