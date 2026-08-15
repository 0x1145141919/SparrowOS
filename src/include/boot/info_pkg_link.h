#pragma once
#include "abi/boot.h"
#include "boot/init_inheritance_analyzed.h"
#include "abi/src_loc.h"

// ════════════════════════════════════════════════════════════════
// info_pkg_link — v2 信息包链接（偏移式 → 指针式 analyzed 视图）
//
// 逆向 info_fill.cpp 的一级重链：v2 包内 phymem_segments / properties_table /
// free_segs_descriptors_table 存"包基址相对偏移"（info_offset_t 语义），
// properties 内每条 asset_entry_t 的 name/data 同样如此。链接 = 按包基址自加
// 还原为可访问线性地址。
//
// 原地链接（单参数）：
//   init_to_kernel_header_v2 与 init_to_kernel_header_analyzed 字段一一对应、
//   布局完全同构——链接直接把包内三个 offset 字段 + properties 内 name/data
//   偏移**原地改写为真实指针**，然后 reinterpret_cast 为 analyzed 视角返回。
//   链接后 pkg 即不再保持偏移式语义（一次性，焚包前完成）。
//
// 语义边界：
//   - 只做地址重算，不拷贝、不分配、不校验数据内容（magic/偏移落在包内除外）
//   - free_seg_descriptor_t 是索引式描述符（in_pure_memview_idx → phymem_segments
//     下标 / baseidx_in_memmap → pages_arr 下标），条目内无需重链
//   - 视图指向包内，包焚毁（ksetmem_8）前有效；pour 深拷贝后即可焚
// ════════════════════════════════════════════════════════════════

// 原地链接 v2 信息包。
//   pkg — 信息包在 kernel 侧可访问的线性基址（exec_env_prepare 的 arg0），非 const
//         （会被原地改写：offset → 指针）
// 校验：magic 合法 + 三个 offset 均在包内（offset+长度 ≤ self_pages_count*4096）。
// 失败返回 nullptr；成功返回 reinterpret_cast<init_to_kernel_header_analyzed*>(pkg)，
// 各字段可直接消费。
init_to_kernel_header_analyzed* link_init_to_kernel_header(init_to_kernel_header_v2* pkg);
