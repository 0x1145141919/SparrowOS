#include "abi/boot.h"
#include "init/init_asset_registry.h"
#include "init/init_bcb_juvenile.h"
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
    bsp_kout << "  bcbs=" << h->bcbs_count
             << " @off 0x" << h->bcb_table << kendl;
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

    // bcbs（bcb_district_descriptor 解包：order [0:5] / base [12:63]；
    //      bitmap 区在包内已是 fpa_bitmaps 池内偏移）
    if (h->bcbs_count) {
        const bcb_desc_v2_t* descs = (const bcb_desc_v2_t*)(pkt_base + h->bcb_table);
        bsp_kout << "  -- bcb_table (" << h->bcbs_count << ") --" << kendl;
        for (uint64_t i = 0; i < h->bcbs_count; i++) {
            const uint64_t dd = descs[i].bcb_district_descriptor;
            const uint8_t  order  = static_cast<uint8_t>(dd & 0x3F);
            const uint64_t base_pa = dd & ~static_cast<uint64_t>(0xFFF);
            bsp_kout << "    [" << i << "] order=" << (uint32_t)order
                     << " base=0x" << base_pa
                     << " bitmap_off=0x" << descs[i].bitmap_region_base_pa << kendl;
        }
    }

    bsp_kout << "========================================" << kendl << kendl;
}

// ============================================================================
// 构建 init_to_kernel_header_v2 — 偏移式 + 注册表 + BCB 描述
// ============================================================================
//
// v2 契约（见 abi/boot.h init_to_kernel_header_v2）：
//   一等字段只剩 phymem_segments / properties_table / bcb_table 三个 offset，
//   其余全部进 properties_table（资产注册表序列化）。pages_arr / kmmu_interval /
//   arch_specify / pass_through / loaded_VM_intervals 等 v1 一等字段全部移除：
//   - pages_arr：彻底废除，职能由 bcb_table（跨世界位图）接替
//   - kmmu_interval / arch_specify：资产树已覆盖（kmmu 树 / 各 mem 资产）
//
// 包布局（8 字节对齐逐段推进）：
//   [0, HDR)                    init_to_kernel_header_v2
//   [names_off, +)              name 串（每资产含 '\0'）
//   [entries_off, +)            asset_entry_t[]      ← properties_table
//   [blobs_off, +)              desc blob（每资产 data 拷贝入包）
//   [segs_off, +)               phymem_segment[]     ← phymem_segments
//   [bcbs_off, +)               bcb_desc_v2_t[]      ← bcb_table
//
// 一级重链：name/data 在包内存"包基址相对偏移"（info_offset_t 语义），
// kernel 端用包基址自加还原可访问线性地址。
//
// 输入:
//   pkt_pbase — 信息包物理基址（init_bcb_juvenile::alloc，位图已置占用）
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
    const uint64_t bcbs_count  = init_bcb_juvenile::get_desc_count();
    const bcb_desc_v2_t* bcbs  = init_bcb_juvenile::get_descs();

    const uint64_t hdr_sz = sizeof(init_to_kernel_header_v2);
    const uint64_t seg_sz = seg_count * sizeof(phymem_segment);
    const uint64_t bcb_sz = bcbs_count * sizeof(bcb_desc_v2_t);
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
    const uint64_t bcbs_off    = align_up(segs_off + seg_sz, 8);
    const uint64_t total       = bcbs_off + bcb_sz;

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
    h->bcbs_count              = bcbs_count;
    h->bcb_table               = bcbs_off;
    h->logical_processor_count = header->logical_processor_count;

    // ---- phymem_segments ----
    if (seg_sz && seg_view)
        ksystemramcpy(seg_view, base + segs_off, seg_sz);

    // ---- bcb_table：bitmap 区地址按 fpa_bitmaps 资产重定位（绝对 PA → 池内偏移）----
    // init 侧（init_bcb_juvenile）填的是绝对物理基址——UEFI 恒等映射下 bitmap_region_base_pa
    // 即 init.elf 世界的访问地址；kernel 接手后恒等映射不复存在，须经 fpa_bitmaps 资产
    // （位图池的 kernel VA 区间）重定位：bitmap_kernel_va = fpa_bitmaps.vbase() + offset。
    // 故此处把包内 desc 改写为相对位图池基址的偏移（同时校验落在池内）。
    if (bcb_sz && bcbs) {
        const asset_entry_t* fpa = g_asset_registry ? g_asset_registry->read("fpa_bitmaps") : nullptr;
        if (!fpa || !fpa->data) {
            bsp_kout << "[BUILD_HEADER] FATAL: fpa_bitmaps asset missing for bcb_table" << kendl;
            if (plan) delete[] plan;
            return 0;
        }
        const uint64_t pool_pbase = ((const vm_interval*)fpa->data)->pbase();
        bcb_desc_v2_t* dst = reinterpret_cast<bcb_desc_v2_t*>(base + bcbs_off);
        for (uint64_t i = 0; i < bcbs_count; i++) {
            if (bcbs[i].bitmap_region_base_pa < pool_pbase) {
                bsp_kout << "[BUILD_HEADER] FATAL: bcb bitmap outside fpa pool" << kendl;
                if (plan) delete[] plan;
                return 0;
            }
            dst[i] = bcbs[i];
            dst[i].bitmap_region_base_pa -= pool_pbase;   // → fpa_bitmaps 池内偏移
            dst[i].bitmap_region_base_pa += ((const vm_interval*)fpa->data)->vbase();//换成线性地址
        }
    }

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
