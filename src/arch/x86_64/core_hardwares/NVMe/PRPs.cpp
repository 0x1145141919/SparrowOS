#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/main_phyaddr_access_window.h>
#include <util/kout.h>

// ============================================================
// Zero an MPS-sized PRP List page via PhyAddrAccessor.
// ============================================================
static void prp_list_page_zero(phyaddr_t page_pa, uint32_t mps)
{
    // 主窗口恒等映射直写（1GB 大页），基址只算一次
    volatile uint64_t* page_va = (volatile uint64_t*)PHYACC_VA(page_pa);
    uint32_t nqwords = mps / sizeof(uint64_t);
    for (uint32_t i = 0; i < nqwords; i++) {
        page_va[i] = 0;
    }
}

// ============================================================
// Write a single PRP entry (uint64_t) into a PRP List page.
// ============================================================
static void prp_list_write_entry(phyaddr_t page_pa, uint32_t entry_idx,
                                  uint64_t entry_value)
{
    ((volatile uint64_t*)PHYACC_VA(page_pa))[entry_idx] = entry_value;
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
        return empty_kurd;
    }

    if (page_count == 2) {
        // §4.3.1(d): PRP1 → page 0, PRP2 → page 1 (offset = 0h)
        root_out->prp1 = pbase;
        root_out->prp2 = pbase + mps;
        return empty_kurd;
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

    return empty_kurd;
}

// ============================================================
// prp_template_param_error: 模板接口参数校验失败时的 KURD
// ============================================================
static KURD_t prp_template_param_error(uint16_t reason)
{
    return KURD_t(
        result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::PRP_template,
        level_code::ERROR, err_domain::CORE_MODULE);
}

// ============================================================
// NVMe_Controller::prp_template_build（成员）
//
// 按设备页框（CC.MPS）与单次传输上限定容：
//   max_entries = ceil(max_bytes / MPS)
//   list_pages  = (max_entries<=2) ? 0 : ceil((max_entries-1) / (MPS/8-1))
// FPA 一次性分配 list_pages*MPS 连续页（主窗口 DRAM，PHYACC_VA 硬转清零）。
// max_bytes=0 → 按 max_transfer_bytes（MDTS 折算，MDTS=0 → 2MB）全量。
// ============================================================
KURD_t NVMe_Controller::prp_template_build(PRP_template* tpl, uint32_t max_bytes)
{
    if (tpl == nullptr) {
        return prp_template_param_error(
            DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::tpl_null);
    }
    if (max_bytes == 0) {
        max_bytes = max_transfer_bytes;
    }

    uint32_t mps_shift = this->mps_shift;
    uint64_t mps       = 1ull << mps_shift;
    uint32_t max_entries = (uint32_t)((max_bytes + mps - 1) >> mps_shift);
    if (max_entries == 0) max_entries = 1;

    uint32_t entries_per_page = (uint32_t)(mps / sizeof(uint64_t));
    uint32_t data_per_page    = entries_per_page - 1u;
    uint32_t list_pages = (max_entries <= 2) ? 0
        : ((max_entries - 1) + data_per_page - 1u) / data_per_page;

    *tpl = {};
    tpl->mps_shift        = mps_shift;
    tpl->list_page_count  = list_pages;
    tpl->capacity_entries = 2 + list_pages * data_per_page;

    if (list_pages > 0) {
        KURD_t kurd;
        uint64_t alloc_bytes = (uint64_t)list_pages * mps;
        phyaddr_t pa = FreePagesAllocator::alloc(
            alloc_bytes, {}, page_state_t::kernel_pinned, kurd);
        if (error_kurd(kurd) || pa == FreePagesAllocator::INVALID_ALLOC_BASE) {
            return prp_template_param_error(
                DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::alloc_fail);
        }
        tpl->list_head_pa = pa;

        // 主窗口直映射清零（PHYACC_VA 硬转，无 MMU 开销）
        volatile uint64_t* page_va = (volatile uint64_t*)PHYACC_VA(pa);
        uint32_t nqwords = (uint32_t)(alloc_bytes / sizeof(uint64_t));
        for (uint32_t i = 0; i < nqwords; i++) {
            page_va[i] = 0;
        }
    }

    return empty_kurd;
}

// ============================================================
// NVMe_Controller::prp_template_destroy（静态）
//
// 模板自持全部释放信息（list_head_pa/list_page_count/mps_shift），
// 不依赖 controller 实例，任意上下文可析构。
// ============================================================
KURD_t NVMe_Controller::prp_template_destroy(PRP_template* tpl)
{
    if (tpl == nullptr) {
        return prp_template_param_error(
            DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::tpl_null);
    }
    if (tpl->list_head_pa != 0 && tpl->list_page_count > 0) {
        uint64_t mps = 1ull << tpl->mps_shift;
        FreePagesAllocator::free(tpl->list_head_pa,
                                 (uint64_t)tpl->list_page_count * mps);
    }
    *tpl = {};
    return empty_kurd;
}

// ============================================================
// prp_template_fill：根据 mem_segs 填充模板（自由函数，零分配）
//
// 校验：segs 有效 / 段 MPS 对齐 / 总容量 ≥ bytes / 数据页数 ≤ 模板容量
// 填充：单遍流式拆段（无临时数组）
//   第 1 页 → prp1；第 2 页 → prp2（退化，不用 List 页）
//   ≥3 页   → prp2 = List 指针，List 从数据页 1 起流式写，
//             每页写满 (MPS/8-1) 个 entry 后写链指针换下一页
// ============================================================
KURD_t prp_template_fill(PRP_template* tpl, const mem_segs_t& segs, uint64_t bytes)
{
    if (tpl == nullptr) {
        return prp_template_param_error(
            DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::tpl_null);
    }
    if (segs.count == 0 || segs.entries == nullptr) {
        return prp_template_param_error(
            DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::segs_invalid);
    }
    if (bytes == 0) {
        return empty_kurd;
    }

    uint32_t mps_shift = tpl->mps_shift;
    uint64_t mps       = 1ull << mps_shift;

    // 段对齐 + 总容量校验
    uint64_t seg_capacity = 0;
    for (uint64_t i = 0; i < segs.count; i++) {
        if (segs.entries[i].base & (mps - 1)) {
            return prp_template_param_error(
                DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::seg_not_mps_aligned);
        }
        seg_capacity += segs.entries[i].nuof_4kbpgs * 4096;
    }
    if (bytes > seg_capacity) {
        return prp_template_param_error(
            DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::bytes_exceed_seg_capacity);
    }

    uint32_t pages_needed = (uint32_t)((bytes + mps - 1) >> mps_shift);
    if (pages_needed > tpl->capacity_entries) {
        return prp_template_param_error(
            DEVICES_locs::NVMe_events::PRP_template_results::fail_reasons::entries_exceed_capacity);
    }

    tpl->prp1 = 0;
    tpl->prp2 = 0;
    tpl->used_entries = 0;

    uint32_t data_per_page = (uint32_t)(mps / sizeof(uint64_t)) - 1u;
    uint32_t data_idx      = 0;   // 已产出的数据页数
    uint32_t list_page_idx = 0;   // 当前 List 页下标
    uint32_t list_entry_idx = 0;  // 当前 List 页 entry 游标

    for (uint64_t s = 0; s < segs.count && data_idx < pages_needed; s++) {
        uint64_t base = segs.entries[s].base;
        uint64_t end  = base + segs.entries[s].nuof_4kbpgs * 4096;
        uint64_t pos  = base;

        while (pos < end && data_idx < pages_needed) {
            uint64_t next_mps = (pos + mps) & ~(mps - 1);
            if (next_mps > end || next_mps <= pos) {
                next_mps = end;
            }

            if (data_idx == 0) {
                tpl->prp1 = pos;
            } else if (pages_needed == 2) {
                tpl->prp2 = pos;
            } else {
                // pages_needed >= 3：PRP2 = List 指针，数据页 1 起入 List
                if (data_idx == 1) {
                    tpl->prp2 = tpl->list_head_pa;
                }
                uint64_t list_page_pa = tpl->list_head_pa + (uint64_t)list_page_idx * mps;
                ((volatile uint64_t*)PHYACC_VA(list_page_pa))[list_entry_idx] = pos;
                list_entry_idx++;

                if (list_entry_idx == data_per_page && data_idx + 1 < pages_needed) {
                    // 本页写满且仍有数据页：末 entry 链到下一页 List
                    ((volatile uint64_t*)PHYACC_VA(list_page_pa))[data_per_page] =
                        tpl->list_head_pa + (uint64_t)(list_page_idx + 1) * mps;
                    list_page_idx++;
                    list_entry_idx = 0;
                }
            }

            data_idx++;
            pos = next_mps;
        }
    }

    tpl->used_entries = data_idx;
    return empty_kurd;
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
    return empty_kurd;
}
