#include "abi/boot.h"
#include "abi/src_loc.h"
#include "init/init_asset_registry.h"
#include "init/page_allocator_v2.h"
#include "init/util/kout.h"
#include "memory/memory_base.h"

// ============================================================================
// 辅助: 调试打印
// ============================================================================
static const char* memtype_str(PHY_MEM_TYPE t) {
    switch(t) {
        case EFI_RESERVED_MEMORY_TYPE: return "EFI_RESERVED";
        case EFI_LOADER_CODE:          return "EFI_LDR_CODE";
        case EFI_LOADER_DATA:          return "EFI_LDR_DATA";
        case EFI_BOOT_SERVICES_CODE:   return "EFI_BS_CODE";
        case EFI_BOOT_SERVICES_DATA:   return "EFI_BS_DATA";
        case EFI_RUNTIME_SERVICES_CODE:return "EFI_RT_CODE";
        case EFI_RUNTIME_SERVICES_DATA:return "EFI_RT_DATA";
        case freeSystemRam:            return "free";
        case EFI_ACPI_RECLAIM_MEMORY:  return "ACPI_RECLM";
        case EFI_ACPI_MEMORY_NVS:      return "ACPI_NVS";
        case EFI_MEMORY_MAPPED_IO:     return "MMIO";
        case OS_KERNEL_DATA:           return "KERN_DATA";
        case OS_KERNEL_CODE:           return "KERN_CODE";
        case OS_ALLOCATABLE_MEMORY:    return "ALLOCATABLE";
        case OS_PGTB_SEGS:             return "PGTABLE";
        case OS_RESERVED_MEMORY:       return "RESERVED";
        default:                       return "OTHER";
    }
}

static void dump_header_v2(const init_to_kernel_header_v2* h, phyaddr_t pkt_base) {
    bsp_kout <<HEX<< kendl
             << "=== init_to_kernel_header_v2 @ phys 0x" << pkt_base << " ===" << kendl;
    bsp_kout << "  magic=0x" << h->magic
             << "  self_pages=" << h->self_pages_count << kendl;
    bsp_kout << "  phymem_segments=" << h->phymem_segment_count
             << " @off 0x" << h->phymem_segments << kendl;
    bsp_kout << "  properties=" << h->properties_count
             << " @off 0x" << h->properties_table << kendl;
    bsp_kout << "  free_segs=" << h->free_segs_count
             << " @off 0x" << h->free_segs_descriptors_table << kendl;
    bsp_kout << "  logical_processor_count=" << h->logical_processor_count << kendl;

    // phymem_segments
    if (h->phymem_segment_count) {
        const phymem_segment* map = (const phymem_segment*)(pkt_base + h->phymem_segments);
        bsp_kout << "  -- phymem_segments (" << h->phymem_segment_count << ") --" << kendl;
        for (uint64_t i = 0; i < h->phymem_segment_count; i++)
            bsp_kout << "    [" << i << "] 0x" << map[i].start
                     << " +0x" << map[i].size
                     << " " << memtype_str(map[i].type) << kendl;
    }

    // properties（name/data 存包内偏移，这里按包基址重链打印）
    if (h->properties_count) {
        const asset_entry_t* props = (const asset_entry_t*)(pkt_base + h->properties_table);
        bsp_kout << "  -- properties (" << h->properties_count << ") --" << kendl;
        for (uint64_t i = 0; i < h->properties_count; i++)
            bsp_kout << "    [" << i << "] '" << (const char*)(pkt_base + (uint64_t)props[i].name)
                     << "' data@off 0x" << (uint64_t)props[i].data << kendl;
    }

    // free_segs（free_seg_descriptor_t 解包：索引式，无需重定位——
    //     in_pure_memview_idx → phymem_segments 下标，baseidx_in_memmap → pages_arr 下标）
    if (h->free_segs_count) {
        const free_seg_descriptor_t* descs = (const free_seg_descriptor_t*)(pkt_base + h->free_segs_descriptors_table);
        bsp_kout << "  -- free_segs_descriptors_table (" << h->free_segs_count << ") --" << kendl;
        for (uint64_t i = 0; i < h->free_segs_count; i++) {
            bsp_kout << "    [" << i << "] pure_memview_idx=" << descs[i].in_pure_memview_idx
                     << " baseidx_in_memmap=" << descs[i].baseidx_in_memmap << kendl;
        }
    }

    bsp_kout << "========================================" << kendl << kendl;
}

// ============================================================================
// 构建 init_to_kernel_header_v2 — 偏移式 + 注册表 + free_segs 描述
// ============================================================================
//
// v2 契约（见 abi/boot.h init_to_kernel_header_v2）：
//   一等字段只剩 phymem_segments / properties_table / free_segs_descriptors_table
//   三个 offset，其余全部进 properties_table（资产注册表序列化）。pages_arr /
//   kmmu_interval / arch_specify / pass_through / loaded_VM_intervals 等 v1 一等字段
//   全部移除：
//   - pages_arr：职能由 mem_map 状态数组资产（pages_arr mem）接替
//   - kmmu_interval / arch_specify：资产树已覆盖（kmmu 树 / 各 mem 资产）
//
// 包布局（8 字节对齐逐段推进）：
//   [0, HDR)                    init_to_kernel_header_v2
//   [names_off, +)              name 串（每资产含 '\0'）
//   [entries_off, +)            asset_entry_t[]      ← properties_table
//   [blobs_off, +)              desc blob（每资产 data 拷贝入包）
//   [segs_off, +)               phymem_segment[]     ← phymem_segments
//   [freesegs_off, +)           free_seg_descriptor_t[] ← free_segs_descriptors_table
//
// 一级重链：name/data 在包内存"包基址相对偏移"（info_offset_t 语义），
// kernel 端用包基址自加还原可访问线性地址。
//
// free_segs_descriptors_table 是索引式描述符（无需重定位）：
//   in_pure_memview_idx → phymem_segments 下标（纯洁视图逐字拷贝）
//   baseidx_in_memmap   → pages_arr mem 资产内 mem_map 条目下标
//   下标在两世界解析一致，kernel 直接据下标重建 free 区间。
//
// 输入:
//   pkt_pbase — 信息包物理基址（page_allocator_v2 分配，已 pages_set transfer_package）
//   pkt_pages — 信息包总页数
//   header    — BootInfoHeader（UEFI 传递，取 logical_processor_count）
//   seg_view  — phymem_segment 视图（pure view）
//   seg_count — 视图条目数
//
// 输出: pkt_pbase 处填充 v2 header + 各 payload 段；失败返回 0
//
phyaddr_t build_init_to_kernel_header(
    phyaddr_t                pkt_pbase,
    uint64_t                 pkt_pages,
    BootInfoHeader*          header,
    phymem_segment*          seg_view,
    uint64_t                 seg_count)
{
    uint8_t* base = reinterpret_cast<uint8_t*>(pkt_pbase);

    const uint64_t props_count = g_asset_registry ? g_asset_registry->size() : 0;
    const uint64_t free_segs_count = page_allocator_v2::get_free_segs_count();
    free_seg_descriptor_t* free_segs = page_allocator_v2::get_free_segs();

    const uint64_t hdr_sz = sizeof(init_to_kernel_header_v2);
    const uint64_t seg_sz = seg_count * sizeof(phymem_segment);
    const uint64_t freeseg_sz = free_segs_count * sizeof(free_seg_descriptor_t);
    const uint64_t ent_sz = props_count * sizeof(asset_entry_t);

    // ---- 第一遍：name 区 / blob 区 累计，逐资产记 name_off + blob_sz ----
    struct prop_plan_t { uint64_t name_off; uint64_t blob_sz; };
    prop_plan_t* plan = nullptr;
    if (props_count) plan = new prop_plan_t[props_count];

    uint64_t names_cursor = align_up(hdr_sz, 8);
    uint64_t blobs_total  = 0;
    {
        uint64_t i = 0;
        if (g_asset_registry) {
            for (auto it = g_asset_registry->begin(); it != g_asset_registry->end(); ++it, ++i) {
                const uint64_t bsz = asset_desc_size(*it);
                if (!it->name || !it->data || bsz == 0) {
                    bsp_kout << "[BUILD_HEADER] FATAL: bad asset '"
                             << (it->name ? it->name : "(null)") << "'" << kendl;
                    if (plan) delete[] plan;
                    return SRC_LOC();
                }
                const uint64_t name_off = align_up(names_cursor, 8);
                names_cursor = name_off + strlen_in_kernel(it->name) + 1;
                plan[i] = { name_off, bsz };
                blobs_total += align_up(bsz, 8);
            }
        }
    }

    const uint64_t entries_off = align_up(names_cursor, 8);
    const uint64_t blobs_off   = align_up(entries_off + ent_sz, 8);
    const uint64_t segs_off    = align_up(blobs_off + blobs_total, 8);
    const uint64_t freesegs_off = align_up(segs_off + seg_sz, 8);
    const uint64_t total       = freesegs_off + freeseg_sz;

    const uint64_t allocated = pkt_pages * 4096;
    if (total > allocated) {
        bsp_kout << "[BUILD_HEADER] FATAL: pkt too small: need 0x"
                 << HEX << total << " but have 0x" << allocated << DEC << kendl;
        if (plan) delete[] plan;
        return 0;
    }

    // ---- 填充 header ----
    init_to_kernel_header_v2* h = reinterpret_cast<init_to_kernel_header_v2*>(base);
    h->magic                   = 0x494E494B524E4C48ULL; // "INIKRNLH"
    h->self_pages_count        = pkt_pages;
    h->phymem_segment_count    = seg_count;
    h->phymem_segments         = segs_off;
    h->properties_count        = props_count;
    h->properties_table        = entries_off;
    h->free_segs_count         = free_segs_count;
    h->free_segs_descriptors_table = freesegs_off;
    h->logical_processor_count = header->logical_processor_count;

    // ---- phymem_segments ----
    if (seg_sz && seg_view)
        ksystemramcpy(seg_view, base + segs_off, seg_sz);

    // ---- free_segs_descriptors_table：索引式描述符，逐字拷贝，无需重定位 ----
    //     in_pure_memview_idx → phymem_segments 下标（纯洁视图逐字拷贝）
    //     baseidx_in_memmap   → pages_arr mem 资产内 mem_map 条目下标
    //     下标在两世界解析一致，kernel 直接据下标重建 free 区间。
    if (freeseg_sz && free_segs)
        ksystemramcpy(free_segs, base + freesegs_off, freeseg_sz);

    // ---- properties：name 串 + entries + desc blob（name/data 存包内偏移） ----
    if (props_count && g_asset_registry) {
        uint64_t i = 0;
        uint64_t blob_cursor = blobs_off;
        asset_entry_t* dst_ent = reinterpret_cast<asset_entry_t*>(base + entries_off);
        for (auto it = g_asset_registry->begin(); it != g_asset_registry->end(); ++it, ++i) {
            const uint64_t name_off = plan[i].name_off;
            const uint64_t blob_off = blob_cursor;
            const uint64_t bsz      = plan[i].blob_sz;

            ksystemramcpy(it->name, base + name_off, strlen_in_kernel(it->name) + 1);
            ksystemramcpy(it->data, base + blob_off, bsz);

            dst_ent[i].name = reinterpret_cast<char*>(name_off);
            dst_ent[i].data = reinterpret_cast<void*>(blob_off);

            blob_cursor += align_up(bsz, 8);
        }
    }
    if (plan) delete[] plan;

    dump_header_v2(h, pkt_pbase);
    return pkt_pbase;
}
