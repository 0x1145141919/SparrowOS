#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>

// ============================================================
// Zero an MPS-sized PRP List page via PhyAddrAccessor.
// ============================================================
static void prp_list_page_zero(phyaddr_t page_pa, uint32_t mps)
{
    uint32_t nqwords = mps / sizeof(uint64_t);
    for (uint32_t i = 0; i < nqwords; i++) {
        PhyAddrAccessor::writeu64(page_pa + i * sizeof(uint64_t), 0);
    }
}

// ============================================================
// Write a single PRP entry (uint64_t) into a PRP List page.
// ============================================================
static void prp_list_write_entry(phyaddr_t page_pa, uint32_t entry_idx,
                                  uint64_t entry_value)
{
    phyaddr_t offset = page_pa + (uint64_t)entry_idx * sizeof(uint64_t);
    PhyAddrAccessor::writeu64(offset, entry_value);
}

// ============================================================
// 计算 segment 中按 MPS 边界拆分出的总页面数（双 pass 用）
// ============================================================
static uint64_t count_mps_pages(const mem_segs_t& segs, uint32_t mps)
{
    uint64_t count = 0;
    for (uint64_t s = 0; s < segs.count; s++) {
        phyaddr_t base     = segs.entries[s].base;
        uint64_t seg_bytes = segs.entries[s].nuof_4kbpgs * 4096;
        phyaddr_t end      = base + seg_bytes;

        phyaddr_t pos = base;
        while (pos < end) {
            phyaddr_t next_mps = (pos + mps) & ~((uint64_t)mps - 1);
            if (next_mps > end || next_mps <= pos) {
                next_mps = end;
            }
            count++;
            pos = next_mps;
        }
    }
    return count;
}

// ============================================================
// 填充 page_addrs 数组：将 segment 按 MPS 边界拆分
// ============================================================
static void fill_mps_page_addrs(const mem_segs_t& segs, uint32_t mps,
                                 uint64_t* page_addrs)
{
    uint64_t idx = 0;
    for (uint64_t s = 0; s < segs.count; s++) {
        phyaddr_t base     = segs.entries[s].base;
        uint64_t seg_bytes = segs.entries[s].nuof_4kbpgs * 4096;
        phyaddr_t end      = base + seg_bytes;

        phyaddr_t pos = base;
        while (pos < end) {
            phyaddr_t next_mps = (pos + mps) & ~((uint64_t)mps - 1);
            if (next_mps > end || next_mps <= pos) {
                next_mps = end;
            }
            page_addrs[idx++] = pos;
            pos = next_mps;
        }
    }
}

// ============================================================
// build_PRP_root：从连续的物理页构建 PRP
//
// §4.3.1 关键约束：
//   (a) PRP List 中的 PRP entry offset 必须为 0h（页对齐）
//   (b) 需多个 PRP List 页面时，末 entry 指向下一页 PRP List（可 daisy-chain）
//   (c) PRP List 紧凑排列，从 entry 0 开始
//   (d) PRP2 若有 offset 则控制器应报 PRP Offset Invalid
//
// 假设 pbase 是 MPS 对齐的连续地址。
// root_out 由调用方提供存储，用于 destroy_PRP_root。
// ============================================================
KURD_t build_PRP_root(phyaddr_t pbase, uint32_t page_count,
                       uint32_t mps_shift, prp_root_t* root_out, KURD_t& kurd)
{
    if (root_out == nullptr) {
        return kurd;
    }

    root_out->prp1           = 0;
    root_out->prp2           = 0;
    root_out->list_head_pa   = 0;
    root_out->list_page_count = 0;
    root_out->page_count     = page_count;

    uint32_t mps = 1u << mps_shift;
    uint32_t entries_per_page = mps / sizeof(uint64_t);
    // §4.3.1(b): 每 PRP List 页面最后 1 entry 保留给链指针
    uint32_t data_per_page = entries_per_page - 1u;

    if (page_count == 1) {
        // §4.3.1(d): 仅 PRP1，PRP2 = 0
        root_out->prp1 = pbase;
        root_out->prp2 = 0;
        return KURD_t(
            result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::submit_command,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    if (page_count == 2) {
        // §4.3.1(d): PRP1 → page 0, PRP2 → page 1 (offset = 0h)
        root_out->prp1 = pbase;
        root_out->prp2 = pbase + mps;
        return KURD_t(
            result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::submit_command,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    // ≥3 pages: need PRP List
    root_out->prp1 = pbase;

    uint32_t remaining   = page_count - 1;            // pages after PRP1
    uint32_t list_pages  = (remaining + data_per_page - 1u) / data_per_page;

    phyaddr_t list_pa = FreePagesAllocator::alloc(
        list_pages * mps,
        {},
        page_state_t::kernel_pinned,
        kurd);
    if (error_kurd(kurd) || list_pa == FreePagesAllocator::INVALID_ALLOC_BASE) {
        return kurd;
    }

    // Zero all PRP List pages via PhyAddrAccessor (§4.3.1(a): offset = 0h)
    for (uint32_t lp = 0; lp < list_pages; lp++) {
        prp_list_page_zero(list_pa + (uint64_t)lp * mps, mps);
    }

    root_out->list_head_pa   = list_pa;
    root_out->list_page_count = list_pages;
    root_out->prp2           = list_pa;   // PRP2 → first PRP List page (offset = 0h)

    // Fill PRP List with chaining per §4.3.1(b)
    uint32_t data_idx = 0;  // index into data pages (page_count total, skip page 0)
    for (uint32_t lp = 0; lp < list_pages; lp++) {
        phyaddr_t page_pa = list_pa + (uint64_t)lp * mps;
        bool     is_last  = (lp == list_pages - 1);

        uint32_t n_entries = is_last
            ? (remaining - data_idx)
            : data_per_page;

        for (uint32_t e = 0; e < n_entries; e++) {
            uint64_t data_pa = pbase + (uint64_t)(data_idx + 1) * mps;
            prp_list_write_entry(page_pa, e, data_pa);
            data_idx++;
        }

        if (!is_last) {
            // §4.3.1(b): last entry of non-terminal page = chain to next list page
            phyaddr_t next_pa = list_pa + (uint64_t)(lp + 1) * mps;
            prp_list_write_entry(page_pa, entries_per_page - 1u, next_pa);
        }
    }

    return KURD_t(
        result_code::SUCCESS, 0,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::submit_command,
        level_code::INFO, err_domain::CORE_MODULE);
}

// ============================================================
// build_PRP_root_advance：从非连续段构建 PRP（支持 MEM SEGS）
//
// 两遍扫描：
//   1. count_mps_pages → new uint64_t[count]
//   2. fill_mps_page_addrs → 构建 PRP List
//
// §4.3.1 约束同上，增加 multi-segment 支持。
// root_out 由调用方提供存储，用于 destroy_PRP_root。
// ============================================================
KURD_t build_PRP_root_advance(mem_segs_t& segs,
                               uint32_t mps_shift,
                               prp_root_t* root_out, KURD_t& kurd)
{
    if (root_out == nullptr) {
        return kurd;
    }

    root_out->prp1           = 0;
    root_out->prp2           = 0;
    root_out->list_head_pa   = 0;
    root_out->list_page_count = 0;
    root_out->page_count      = 0;

    if (segs.count == 0 || segs.entries == nullptr) {
        return kurd;
    }

    uint32_t mps = 1u << mps_shift;
    uint32_t entries_per_page = mps / sizeof(uint64_t);
    uint32_t data_per_page = entries_per_page - 1u;

    // Pass 1: count total MPS-aligned pages across all segments
    uint64_t total_pages_64 = count_mps_pages(segs, mps);
    if (total_pages_64 == 0) {
        return kurd;
    }
    uint32_t total_pages = (uint32_t)total_pages_64;

    // Allocate temporary array for page addresses
    uint64_t* page_addrs = new uint64_t[total_pages];

    // Pass 2: fill array
    fill_mps_page_addrs(segs, mps, page_addrs);

    root_out->page_count = total_pages;
    root_out->prp1       = page_addrs[0];

    if (total_pages == 1) {
        root_out->prp2 = 0;
        delete[] page_addrs;
        return KURD_t(
            result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::submit_command,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    if (total_pages == 2) {
        root_out->prp2 = page_addrs[1];
        delete[] page_addrs;
        return KURD_t(
            result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::submit_command,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    // ≥3 pages: need PRP List
    uint32_t remaining  = total_pages - 1;
    uint32_t list_pages = (remaining + data_per_page - 1u) / data_per_page;

    phyaddr_t list_pa = FreePagesAllocator::alloc(
        list_pages * mps,
        {},
        page_state_t::kernel_pinned,
        kurd);
    if (error_kurd(kurd) || list_pa == FreePagesAllocator::INVALID_ALLOC_BASE) {
        delete[] page_addrs;
        return kurd;
    }

    // Zero all PRP List pages
    for (uint32_t lp = 0; lp < list_pages; lp++) {
        prp_list_page_zero(list_pa + (uint64_t)lp * mps, mps);
    }

    root_out->list_head_pa   = list_pa;
    root_out->list_page_count = list_pages;
    root_out->prp2           = list_pa;

    // Fill PRP List with chaining
    uint32_t data_idx = 0;
    for (uint32_t lp = 0; lp < list_pages; lp++) {
        phyaddr_t page_pa = list_pa + (uint64_t)lp * mps;
        bool     is_last  = (lp == list_pages - 1);

        uint32_t n_entries = is_last
            ? (remaining - data_idx)
            : data_per_page;

        for (uint32_t e = 0; e < n_entries; e++) {
            prp_list_write_entry(page_pa, e, page_addrs[data_idx + 1]);
            data_idx++;
        }

        if (!is_last) {
            phyaddr_t next_pa = list_pa + (uint64_t)(lp + 1) * mps;
            prp_list_write_entry(page_pa, entries_per_page - 1u, next_pa);
        }
    }

    delete[] page_addrs;

    return KURD_t(
        result_code::SUCCESS, 0,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::submit_command,
        level_code::INFO, err_domain::CORE_MODULE);
}

// ============================================================
// destroy_PRP_root：释放 PRP List 页面
//
// PRP List 由 list_pages 个 MPS 页面组成（由 build_PRP_root* 分配），
// 依次释放每个 MPS 页面。
// ============================================================
KURD_t destroy_PRP_root(const prp_root_t& root,
                         uint32_t mps_shift, KURD_t& kurd)
{
    uint32_t mps = 1u << mps_shift;
    if (root.list_head_pa != 0 && root.list_page_count > 0) {
        for (uint32_t i = 0; i < root.list_page_count; i++) {
            phyaddr_t list_page_pa = root.list_head_pa + (uint64_t)i * mps;
            FreePagesAllocator::free(list_page_pa, mps);
        }
    }
    return KURD_t(
        result_code::SUCCESS, 0,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::submit_command,
        level_code::INFO, err_domain::CORE_MODULE);
}
