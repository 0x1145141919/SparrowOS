# Init_v3 phase_4 信息包构建迁移草案

> 状态：draft（设计方确认后可覆盖；以代码为准）
> 日期：2026-08-15

---

## 一、背景

`page_allocator_v2` 已部署（phase 2/2.5/3a/3b），`init_bcb_juvenile` 已删除。
phase_4 是 init 侧最后残留：信息包构建仍走 `init_bcb_juvenile::alloc`，
`init_to_kernel_header_v2` 仍填 `bcb_table`/`bcbs_count`——而结构体已在
5434800 改为 `free_segs_count`/`free_segs_descriptors_table`，构建已断。

## 二、部署结论（本次落地）

### 1. 信息包页 = `transfer_package` 语义态

信息包物理页由 `page_allocator_v2::free_ram_explore` 分配后，立即
`pages_set({pkt, pkt_pages<<12}, page_state_t::transfer_package)` 入账穿越。

理由：kernel 收养 mem_map 后即知这些页被占用，且状态语义明确指向
「init.elf 移交给 kernel.elf 的信息包」，与普通持久元数据（kernel_persisit）区分。
`transfer_package=7` 定义于 `memory_base.h` page_state_t。

### 2. 分配器 = page_allocator_v2

替换 `init_bcb_juvenile::alloc(PKT_PAGES, 12, &pkt_err)`：

```
free_ram_explore(PKT_PAGES, 12)  → 勘探并推进 scan_base
pages_set(transfer_package)      → 提交占用（mem_map 唯一写入口）
```

phase_4.5 自裁归还同步迁移：`init_bcb_juvenile::free(p,1)` →
`pages_set({p,0x1000}, page_state_t::free)`——「归还」在状态数组上就是翻状态。

### 3. 穿越契约：bcb_table → free_segs_descriptors_table

`init_to_kernel_header_v2` 一等字段 `bcbs_count`/`bcb_table` 改为
`free_segs_count`/`free_segs_descriptors_table`。

- **源**：`page_allocator_v2::get_free_segs()/get_free_segs_count()`
  （新增 getter，暴露 `mem_map_intervals` 区间描述数组）。
- **数据**：`free_seg_descriptor_t{ in_pure_memview_idx, baseidx_in_memmap }`，
  每个 freeSystemRam 段一个，升序固定，1:1 对应纯洁视图条目。
- **索引式，无需重定位**：
  - `in_pure_memview_idx` → `phymem_segments` 下标（纯洁视图逐字拷贝）
  - `baseidx_in_memmap` → `pages_arr` mem 资产内 mem_map 条目下标
  - 下标在两世界解析一致，kernel 直接据下标重建 free 区间。
- **删除**：原 bcb_table 的 fpa_bitmaps 重定位块（bitmap_region_base_pa 绝对 PA →
  池内偏移 + vbase）不再需要；`g_asset_registry->read("fpa_bitmaps")` 查询一并移除。

### 4. 包布局（8 字节对齐逐段推进）

```
[0, HDR)            init_to_kernel_header_v2
[names_off, +)      name 串
[entries_off, +)    asset_entry_t[]      ← properties_table
[blobs_off, +)      desc blob
[segs_off, +)       phymem_segment[]     ← phymem_segments
[freesegs_off, +)   free_seg_descriptor_t[] ← free_segs_descriptors_table
```

## 三、涉及文件

| 文件 | 改动 |
|------|------|
| `src/include/init/page_allocator_v2.h` / `src/init/page_allocator_v2.cpp` | 新增 `get_free_segs` / `get_free_segs_count`；头注释更新（区间数组现穿越） |
| `src/init/info_fill.cpp` | `build_init_to_kernel_header`：bcb_table → free_segs_descriptors_table，删 fpa 重定位块，dump 同步 |
| `src/init/init_init.cpp` | phase_4 分配器换 page_allocator_v2 + transfer_package；phase_4.5 erase_pages 迁移 |
| `CMakeLists.txt` | 移除 `src/init/init_bcb_juvenile.cpp` |

## 四、遗留（下一步任务，不在本次范围）

kernel.elf 侧消费端仍引用 `bcb_table`/`bcbs_count`/`bcb_desc_v2_t`，整个构建
仍断至 kernel 侧收养迁移（`info_pkg_link.cpp` / `exec_env_prepare.cpp` /
`FreePagesAllocator.cpp` / `mem_init.h` / `bcb_handoff.h` 等）：
- `link_init_to_kernel_header`：bcb_table 重链 → free_segs_descriptors_table 重链
- `exec_env_prepare`：`Inherit_bcbs` → 收养 free_segs + pages_arr + phymem_segments
- `FreePagesAllocator::Init`：BCB 收养路径 → 从 free_segs/pages_arr 重建
