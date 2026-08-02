# Init_v3 设计草案

> 状态：draft（敏捷演进中，最终以代码为准，本文件只记录当前方向，随时可覆盖）

---

## 一、设计动机

现行 init_v2 在「BCB 初始完全纯洁 + 最小内存碎片」的约束下，造出了极为恶臭的时序：

1. **同一批纯内存资产被映射两次**——init.elf 在自建瞬态 `kernel_mmu` 页表上给每个资产 `kmmu->map`，kernel.elf 又在全新 `KspacePageTable` 里 `Kspace_phyaddr_direct_map` 重放一遍（`KImage_map_rebuild` + `kimg_affiliate_property_map1`）。
2. **同一份内容被拷贝两次**——`properties_modify_stage1` 对 initramfs/symtable/log/kIMG 做「转生」：`FPA alloc 新页 → paddr_memcpy → 更新 ppn → 重映射 → 释放旧页`。
3. **污染模型兜底**——`interval_pollute/clean` + `dirty_count` 是为掩盖「init 的分配对 kernel FPA 不可见」这一根因，是治标。
4. **黑洞残留**——`legacy_mmu_interval` 从未被赋值（pollute/clean 空段是 no-op）；init 的 KMMU 页表 CR3 切走后成死页表，无归还路径。

## 二、设计意图（设计方原始表述）

1. **BCB 幼年态 / 成年态 + 单向成长**：幼年态只用成年态的 order-0 1bit 位图（1=空闲 0=占用）；成人仪式 = 从 order 0 向上全面折叠到任意空闲叶子兄弟节点不为叶子的状态。刻意设计为幼年→成年单向成长，不得回退。
2. **幼年→成年贯穿 init.elf → kernel.elf 管线**：BCBs（控制器数组）分层看待——init.elf 与 kernel.elf 各用自己的 BCBS 数组；但 **BCB_bitmaps 穿越两个世界**。BCB_bitmaps 是各 BCB 位图集中存放的数据部分，需带一个极简数组描述中间每个位图区间。约定：init.elf 递交的 BCB_bitmaps 中所有对应 BCB 的位图都处于**幼年态**，由 kernel.elf 自行决断每个 BCB 何时成年。
3. **纯内存资产不再映射虚拟地址**：init_v2 给纯内存资产映射 VA 是画蛇添足——本来就有物理地址和一个恒等窗口，只需物理区间描述符，通过窗口自行访问即可。且因 BCB_bitmaps 穿越，无需把 legacy 页表圈禁在固定区间，而是切换到新页表后 BFS 逐个删除（无论成年或幼年态）。

## 三、已确认的决策（设计方拍板）

| # | 分叉点 | 决策 |
|---|--------|------|
| 1 | 叶子位图由谁生成 | init.elf 的内存分配器管线**直接升级**：除开初始链表分配器（basic_allocator）之后，直接初始化幼年 BCB 分配器（取代 `page_allocator`） |
| 2 | BCB plan 由谁计算 | **只能有一个 plan 运行**——init.elf 计算 BCB plan，结果通过传递结构体以数组形式传递；BCB_bitmaps 因已有恒等窗口，init.elf 阶段**无需为其准备 MMU** |
| 3 | 成年仪式触发时机 | **全量折叠**（mem_init 单线程阶段一次折叠全部 BCB）；若后续全量折叠太慢再优化 |
| 4 | 地址敏感资产访问方式 | GS 复合体 / hw_stacks 是 **ISA 级硬资产**，由 init.elf 映射好（BSP 跳转内核后即用 rsp0 栈）；FPA_bitmap 虽是重要元数据但是**纯内存资产**，只传物理区间信息；HPET + GOP 这两个 x86 架构相关非纯内存资产由 init.elf 准备好 + kernel.elf 重新映射 |

## 四、新协议：init_to_kernel_header v3

新增两段穿越数据：

```
[BCB plan 段]    BCB_desc[] 数组  ← 唯一 plan 源（init.elf 产出）
                 { base, max_order, bitmap_byte_offset }
[BCB_bitmaps 段] 连续位图区        ← 每 BCB 3·2^N bits（成年布局全量预留）
                 叶子区(order-0 1bit)由 init.elf 按分配动作写实
                 内部节点区清零(NONEXIST)，成年仪式时 kernel 填
```

资产按语义分三类处理：

| 资产 | 处理方式 |
|------|----------|
| 纯内存资产：FPA_bitmaps、log_buffer、symtable_file、initramfs_file、pages_arr、kIMG 瞬态映像 | 只传 `{pa, size}` 物理描述符，不带 VA |
| ISA 级资产：GS 复合体、hw_stacks | 仍由 init.elf 映射，VA 穿越（BSP 跳转后即用 rsp0 栈，不可动） |
| arch MMIO 资产：HPET、GOP | init.elf 准备 + kernel.elf 重映射（现状保留） |

## 五、init.elf 变更

1. **分配管线升级**：`basic_allocator`（链表自举，保留）→ **幼年 BCB 分配器**（直接取代 `page_allocator`）。分配即翻叶子位图位。
2. **BCB plan 唯一源**：init.elf 从 `get_pure_memory_view` 空闲段计算 plan（2 的幂切分 + crumbs 分流 + BEST_FIT），写出描述数组。
3. **FPA_bitmaps 区**：从 basic_allocator 预分配（UEFI 恒等映射下按物理地址直接读写，无需 KMMU 映射）。
4. **纯资产不再 `kmmu->map`**：log/symtable/initramfs/pages_arr/kIMG 只记物理描述符；GS/hw_stacks/HPET/GOP 映射逻辑原样保留。
5. **删除**：`va_alloc_up` 纯资产生分支、per-asset `kmmu->map`、`kIMG_self_window` 的 KMMU 映射。

## 六、kernel.elf 变更

1. **FPA::Init 改「收养」路径**：接收 init 的描述数组 + 叶子位图，重建 BCBS 控制器（锁/统计/偏好），**不再从 free_segs 派生 plan**（保证「只有一个 plan」不变量）。pollute 阶段整体删除。
2. **activate 后立即 ACTIVE**：位图精确，无需 dirty_count 保护。
3. **成年仪式**：mem_init 单线程段全量折叠（`fold_up_from_leaves`，自底向上填内部节点，O(位图) 一次；慢再优化）。
4. **访问方式**：CR3 切换前经 `PhyAddrAccessor` 恒等窗口读 ELF/资产；切换后纯资产按需 `Kspace_phyaddr_direct_map` / `Kspace_pinterval_alloc_and_map` 映射（VA 由 kernel 自选，如 kIMG 自省窗口）；GS/hw_stacks/HPET/GOP 走 `kimg_affiliate_property_map1` 重映射（现状）。
5. **properties_modify_stage1 整体删除**：无转生，资产已在位图正确占位。
6. **legacy 页表归还**：CR3 切换后按 `pgallocator` 的 `[base, top)` 线性翻位归还（非 BFS——线性分配器保证整段只含页表页）。

## 七、删除清单

- `properties_modify_stage1`
- `interval_pollute` / `interval_clean` 及 `dirty_count` 机制
- `legacy_mmu_interval`（从未赋值的黑洞）
- kernel 侧 `FPA::Init` 的 plan 派生路径
- init 侧纯资产 VA 分配与映射（`va_alloc_up` 相关、per-asset `kmmu->map`）
- `kIMG_self_window` 的 KMMU 映射

## 八、推进顺序（动态决策，话不说死）

1. **先改 BCB**：幼年/成年态 + `fold_up` 成年仪式 + FPA「收养」路径。压测（`bcb_replay` / USER_MODE 测试）通过后再动下一步。
2. **再上 init.elf**：此时对照真实代码动态决策传递结构体怎么调整（描述数组、位图区布局以实际代码为准）。
3. **最后 kernel.elf 适配**：kernel 消费 init.elf 实际传过来的东西。
4. 资产切分（删转生、纯资产只传物理描述符、legacy 页表归还）视实际进度插入，不预设死顺序。

## 九、遗留待确认（到对应阶段再拍板）

1. `pages_arr` 是否仍穿越？（倾向保留——all_pages_arr 的 `page_state_t` 语义无法从纯 free/used 位图还原，但以实际代码为准）
2. crumbs（order<10 碎片，当前无消费方）继续产出还是干脆不传？
3. 成年仪式在 kernel 侧的精确执行点（FPA adopt 后、CR3 切换前？）
