#include "boot/info_pkg_link.h"

// v2 信息包 magic（与 info_fill.cpp 同一常量）
constexpr uint64_t INFO_PKG_V2_MAGIC = 0x494E494B524E4C48ULL; // "INIKRNLH"

// 偏移是否落在包内：[off, off+len) ⊆ [0, pkt_bytes)
static inline bool offset_in_pkt(uint64_t off, uint64_t len, uint64_t pkt_bytes) {
    return off <= pkt_bytes && len <= pkt_bytes - off;
}

init_to_kernel_header_analyzed* link_init_to_kernel_header(init_to_kernel_header_v2* pkg) {
    if (!pkg) return nullptr;
    if (pkg->magic != INFO_PKG_V2_MAGIC) return nullptr;

    uint8_t* const base = reinterpret_cast<uint8_t*>(pkg);
    const uint64_t pkt_bytes = pkg->self_pages_count * 4096;

    // ---- phymem_segments：offset → 包内指针（原地改写）----
    if (!offset_in_pkt(pkg->phymem_segments,
                       pkg->phymem_segment_count * sizeof(phymem_segment), pkt_bytes))
        return nullptr;
    pkg->phymem_segments = reinterpret_cast<uint64_t>(base + pkg->phymem_segments);

    // ---- free_segs_descriptors_table（索引式：in_pure_memview_idx → phymem_segments
    //       下标 / baseidx_in_memmap → pages_arr 下标，条目内无需重链）----
    if (!offset_in_pkt(pkg->free_segs_descriptors_table,
                       pkg->free_segs_count * sizeof(free_seg_descriptor_t), pkt_bytes))
        return nullptr;
    pkg->free_segs_descriptors_table = reinterpret_cast<uint64_t>(base + pkg->free_segs_descriptors_table);

    // ---- properties_table：数组指针 + 每条 name/data 一级重链 ----
    if (!offset_in_pkt(pkg->properties_table,
                       pkg->properties_count * sizeof(asset_entry_t), pkt_bytes))
        return nullptr;
    uint64_t props_off = pkg->properties_table;
    pkg->properties_table = reinterpret_cast<uint64_t>(base + props_off);

    asset_entry_t* props = reinterpret_cast<asset_entry_t*>(pkg->properties_table);
    for (uint64_t i = 0; i < pkg->properties_count; ++i) {
        const uint64_t name_off = reinterpret_cast<uint64_t>(props[i].name);
        const uint64_t data_off = reinterpret_cast<uint64_t>(props[i].data);
        if (!offset_in_pkt(name_off, 1, pkt_bytes) ||
            !offset_in_pkt(data_off, 1, pkt_bytes))
            return nullptr;
        props[i].name = reinterpret_cast<char*>(base + name_off);
        props[i].data = reinterpret_cast<void*>(base + data_off);
    }

    return reinterpret_cast<init_to_kernel_header_analyzed*>(pkg);
}
