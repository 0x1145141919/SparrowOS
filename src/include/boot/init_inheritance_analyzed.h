#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "abi/asset_route.h"
#include "abi/bcb_handoff.h"

// ════════════════════════════════════════════════════════════════
// init_to_kernel_header_analyzed — v2 信息包"指针式视图"
//
// 与 init_to_kernel_header_v2（偏移式，abi/boot.h）一一对应：
//   偏移式包（info_fill.cpp 产出）三个 offset + properties 内 name/data 全存
//   "包基址相对偏移"；analyzed 是链接后的结果——各字段重链为指向包内真实位置
//   的线性地址，供 kernel 侧直接消费。
//
//   链接动作（info_pkg_link.cpp link_init_to_kernel_header）：
//     phymem_segments   = base + offset
//     properties_table  = base + offset，且每条 name/data 再按 base 自加
//     bcb_table         = base + offset（条目内 bitmap_region_base_pa 已是
//                         fpa_bitmaps 线性地址，info_fill 已重定位，不重链）
//
// 生命周期：视图指向包内，包焚毁（ksetmem_8）前有效；pour 深拷贝后即可焚。
// ════════════════════════════════════════════════════════════════
struct init_to_kernel_header_analyzed{
    uint64_t magic;
    uint64_t self_pages_count;
    uint64_t phymem_segment_count;
    phymem_segment* phymem_segments;     // 包内 phymem_segment[]（重链后指针）
    uint64_t properties_count;
    asset_entry_t* properties_table;     // 包内 asset_entry_t[]（name/data 已重链）
    uint64_t free_segs_count;
    free_seg_descriptor_t* free_segs_descriptors_table;            // 包内 bcb_desc_v2_t[]（注意：v2，非 bcb_desc_t）
    uint32_t logical_processor_count;
};
