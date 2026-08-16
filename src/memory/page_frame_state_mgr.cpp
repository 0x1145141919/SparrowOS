#include "memory/page_frame_state_mgr.h"
#include "memory/kpoolmemmgr.h"   // 全局 operator new[]/delete[]（-fno-exceptions 下需显式声明）
#include "memory/main_phyaddr_access_window.h"   // PHYACC_VA：movable 纯物理描述符 → 窗口 VA

// ════════════════════════════════════════════════════════════════
// page_frame_state_mgr 实现
//
// 核心不变量（与头文件契约一致）：
//   - pages_arr（mem_map）是唯一状态账本：free/非-free 由 page_state_t 表达，
//     adopt 收养即冻结——不覆写 init 已写好的每页状态。
//   - intervals[] 是有序区间表（adopt 从 free_segs_descriptors + phymem_segments
//     解析构建），同时服务 phyaddr → mem_map 索引二分定位 与 可分配区域边界。
//   - 空闲 run 不持有：state_set / early_alloc 直接扫 mem_map（权威账本即真理）。
//   - scan_base_idx（每区间单调光标）：early_alloc 自下而上 first-fit，成功后
//     推进到分配末尾，不回落。光标以下被翻回 free 的页不再被 early_alloc 利用
//     （初始阶段分配单调，可接受）；FPA 收养时取 [scan_base_idx, end) 空闲段。
//
// 错误约定（开发期）：SRC_LOC() 应急盖戳，不编排 KURD 错误树。
// ════════════════════════════════════════════════════════════════

// ---------- 静态数据成员定义（纯静态类：全局唯一账本） ----------
page*    page_frame_state_mgr::pages_arr             = nullptr;
uint64_t page_frame_state_mgr::pages_arr_entry_count = 0;
bool     page_frame_state_mgr::early_alloc_closed    = false;
page_frame_state_mgr::interval_t* page_frame_state_mgr::intervals      = nullptr;
uint64_t page_frame_state_mgr::intervals_count                        = 0;

// ================================================================
// adopt — 收养 init 穿越的 pages_arr 账本 + free_segs 描述符
// ================================================================
loc_code_t page_frame_state_mgr::adopt(const movable_file_entry_t* pages_arr_file,
                                       const free_seg_descriptor_t* free_segs,
                                       uint64_t free_segs_count)
{
    if (!pages_arr_file || !free_segs || free_segs_count == 0) return SRC_LOC();
    if (!phymem_segments || phymem_segments_count == 0) return SRC_LOC();

    // pages_arr 已改 movable（纯物理描述符，phase_3b 不再 KMMU 映射）：
    // 经主窗口把物理基址重链成可访问 VA 作为 mem_map 线性基址。
    // 前提：调用方（exec_env_prepare）已 PhyAddrAccessor::Init（main_window_vbase 就绪）。
    const phyaddr_t mem_map_pbase = pages_arr_file->base_ppn << 12;
    pages_arr = reinterpret_cast<page*>(PHYACC_VA(mem_map_pbase));
    pages_arr_entry_count = pages_arr_file->size / sizeof(page);
    if (!pages_arr || pages_arr_entry_count == 0) return SRC_LOC();

    interval_t* new_intervals = new interval_t[free_segs_count];
    if (!new_intervals) return SRC_LOC();

    // 有序填充：free_segs 升序（init 已排好），baseidx_in_memmap 单调递增。
    // 防御性校验：in_pure_memview_idx 落在 phymem_segments 内；区间不越出账本。
    uint64_t prev_end_idx = 0;
    for (uint64_t i = 0; i < free_segs_count; i++) {
        const uint64_t iv_idx = free_segs[i].in_pure_memview_idx;
        if (iv_idx >= phymem_segments_count) {
            delete[] new_intervals;
            return SRC_LOC();
        }
        const phymem_segment& seg = phymem_segments[iv_idx];
        const uint64_t        pg  = seg.size >> 12;
        if (pg == 0 || free_segs[i].baseidx_in_memmap < prev_end_idx) {
            delete[] new_intervals;
            return SRC_LOC();
        }
        if (free_segs[i].baseidx_in_memmap + pg > pages_arr_entry_count) {
            delete[] new_intervals;
            return SRC_LOC();
        }
        interval_t& it = new_intervals[i];
        it.base              = seg.start;
        it.numof4kbpgs       = pg;
        it.baseidx_in_memmap = free_segs[i].baseidx_in_memmap;
        it.scan_base_idx     = it.baseidx_in_memmap;   // 初始时期光标：区间首
        prev_end_idx         = it.baseidx_in_memmap + pg;
    }

    // 全部校验通过后才提交（失败路径不留半成品状态）。
    intervals      = new_intervals;
    intervals_count = free_segs_count;
    early_alloc_closed = false;
    return 0;
}

// ================================================================
// state_set — 内存区间 state_set（mem_map 唯一写入口，跨区间遍历）
// ================================================================
loc_code_t page_frame_state_mgr::state_set(phyaddr_t phybase, uint64_t page_count,
                                           page_state_t state)
{
    if (page_count == 0) return 0;
    if ((phybase & 0xFFF) != 0) return SRC_LOC();
    if (!pages_arr || !intervals || intervals_count == 0) return SRC_LOC();

    const phyaddr_t start = phybase;
    const phyaddr_t end   = start + (page_count << 12);
    if (end <= start) return SRC_LOC();   // 溢出

    // 遍历相交区间；覆盖不足（空洞/越界）= 账本无法表达 → 失败
    uint64_t covered = 0;
    for (uint64_t i = 0; i < intervals_count; i++) {
        interval_t& iv = intervals[i];
        const phyaddr_t iv_end = iv.base + (iv.numof4kbpgs << 12);
        if (end <= iv.base || start >= iv_end) continue;   // 不相交

        const phyaddr_t seg_lo    = (start > iv.base) ? start : iv.base;
        const phyaddr_t seg_hi    = (end < iv_end) ? end : iv_end;
        const uint64_t  start_idx = iv.baseidx_in_memmap + ((seg_lo - iv.base) >> 12);
        const uint64_t  count     = (seg_hi - seg_lo) >> 12;
        if (start_idx + count > pages_arr_entry_count) return SRC_LOC();

        for (uint64_t j = 0; j < count; j++)
            pages_arr[start_idx + j].state = state;
        covered += count << 12;
    }
    if (covered != (page_count << 12)) return SRC_LOC();
    return 0;
}

// ================================================================
// state_query — 单个页框状态查询
// ================================================================
loc_code_t page_frame_state_mgr::state_query(phyaddr_t phyaddr, page_state_t* out_state)
{
    if (!out_state) return SRC_LOC();
    interval_t* iv = locate_interval(phyaddr);
    if (!iv) return SRC_LOC();

    const uint64_t idx = iv->baseidx_in_memmap + ((phyaddr - iv->base) >> 12);
    if (idx >= pages_arr_entry_count) return SRC_LOC();
    *out_state = pages_arr[idx].state;
    return 0;
}

// ================================================================
// kind_check — 区间页框种类校验：全部页框在 pages_arr 内被找到且状态 == expected
// ================================================================
bool page_frame_state_mgr::kind_check(phyaddr_t phybase, uint64_t page_count,
                                      page_state_t expected)
{
    if (page_count == 0) return true;
    if ((phybase & 0xFFF) != 0) return false;
    if (!pages_arr || !intervals || intervals_count == 0) return false;

    const phyaddr_t end = phybase + (page_count << 12);
    if (end <= phybase) return false;   // 溢出

    uint64_t checked = 0;
    for (uint64_t i = 0; i < intervals_count; i++) {
        interval_t& iv = intervals[i];
        const phyaddr_t iv_end = iv.base + (iv.numof4kbpgs << 12);
        if (end <= iv.base || phybase >= iv_end) continue;   // 不相交

        const phyaddr_t seg_lo    = (phybase > iv.base) ? phybase : iv.base;
        const phyaddr_t seg_hi    = (end < iv_end) ? end : iv_end;
        const uint64_t  start_idx = iv.baseidx_in_memmap + ((seg_lo - iv.base) >> 12);
        const uint64_t  count     = (seg_hi - seg_lo) >> 12;
        if (start_idx + count > pages_arr_entry_count) return false;

        for (uint64_t j = 0; j < count; j++)
            if (pages_arr[start_idx + j].state != expected) return false;
        checked += count << 12;
    }
    return checked == (page_count << 12);
}

// ================================================================
// idx_base_* — 纯 pages_arr 索引接口（刻意不做跨 interval 检查，只做越界）
// ================================================================
loc_code_t page_frame_state_mgr::idx_base_state_set(uint64_t base_idx, uint64_t page_count,
                                                    page_state_t state)
{
    if (page_count == 0) return 0;
    // 减法式越界：避免 base_idx + page_count 加法溢出后误判通过（窗口访问无 guard 页，
    // 越界会静默写坏相邻物理内存，越界检查是唯一防线）。
    if (!pages_arr || base_idx > pages_arr_entry_count) return SRC_LOC();
    if (page_count > pages_arr_entry_count - base_idx) return SRC_LOC();
    for (uint64_t j = 0; j < page_count; j++)
        pages_arr[base_idx + j].state = state;
    return 0;
}

loc_code_t page_frame_state_mgr::idx_base_state_query(uint64_t base_idx,
                                                      page_state_t* out_state)
{
    if (!out_state) return SRC_LOC();
    if (!pages_arr || base_idx >= pages_arr_entry_count) return SRC_LOC();
    *out_state = pages_arr[base_idx].state;
    return 0;
}

bool page_frame_state_mgr::idx_base_kind_check(uint64_t base_idx, uint64_t page_count,
                                               page_state_t expected)
{
    if (page_count == 0) return true;
    // 减法式越界（同上）：窗口访问无 guard 页，越界读会静默读到相邻物理内存。
    if (!pages_arr || base_idx > pages_arr_entry_count) return false;
    if (page_count > pages_arr_entry_count - base_idx) return false;
    for (uint64_t j = 0; j < page_count; j++)
        if (pages_arr[base_idx + j].state != expected) return false;
    return true;
}

// ================================================================
// phyaddr_to_page_idx / page_idx_to_phyaddr — 地址 / pages_arr 索引互转
// ================================================================
loc_code_t page_frame_state_mgr::phyaddr_to_page_idx(phyaddr_t phyaddr, uint64_t* out_idx)
{
    if (!out_idx) return SRC_LOC();
    interval_t* iv = locate_interval(phyaddr);
    if (!iv) return SRC_LOC();   // 不在任何 freeSystemRam 区间内

    const uint64_t idx = iv->baseidx_in_memmap + ((phyaddr - iv->base) >> 12);
    if (idx >= pages_arr_entry_count) return SRC_LOC();
    *out_idx = idx;
    return 0;
}

loc_code_t page_frame_state_mgr::page_idx_to_phyaddr(uint64_t idx, phyaddr_t* out_phyaddr)
{
    if (!out_phyaddr) return SRC_LOC();
    if (!intervals || intervals_count == 0 || idx >= pages_arr_entry_count) return SRC_LOC();

    // baseidx_in_memmap 单调升序，二分定位包含 idx 的区间
    uint64_t lo = 0;
    uint64_t hi = intervals_count;
    while (lo < hi) {
        const uint64_t mid = lo + ((hi - lo) >> 1);
        const interval_t& iv = intervals[mid];
        const uint64_t iv_end = iv.baseidx_in_memmap + iv.numof4kbpgs;
        if (idx < iv.baseidx_in_memmap) { hi = mid; continue; }
        if (idx >= iv_end) { lo = mid + 1; continue; }
        *out_phyaddr = iv.base + ((idx - iv.baseidx_in_memmap) << 12);
        return 0;
    }
    return SRC_LOC();   // 理论上不可达（idx < entry_count 必属某区间）
}

// ================================================================
// early_alloc — 初始时期分配：每区间单光标自下而上 first-fit
// ================================================================
phyaddr_t page_frame_state_mgr::early_alloc(uint64_t page_count, uint8_t align_log2,
                                            page_state_t state)
{
    if (early_alloc_closed) return 0;   // 关闭后调用 = 调用违规
    if (page_count == 0) return 0;
    if (align_log2 < 12) align_log2 = 12;
    if (!pages_arr || !intervals || intervals_count == 0) return 0;

    const uint64_t align_mask = (1ull << align_log2) - 1;

    for (uint64_t i = 0; i < intervals_count; i++) {
        interval_t& iv = intervals[i];
        const uint64_t iv_end_idx = iv.baseidx_in_memmap + iv.numof4kbpgs;
        uint64_t idx = (iv.scan_base_idx >= iv.baseidx_in_memmap)
                           ? iv.scan_base_idx : iv.baseidx_in_memmap;
        if (idx >= iv_end_idx || iv_end_idx - idx < page_count) continue;   // 本区间剩余不足

        while (idx < iv_end_idx) {
            if (pages_arr[idx].state != page_state_t::free) { idx++; continue; }

            const phyaddr_t paddr = iv.base + ((idx - iv.baseidx_in_memmap) << 12);
            // 对齐前跳：候选起点必须满足物理对齐
            if (paddr & align_mask) {
                const phyaddr_t aligned = (paddr + align_mask) & ~align_mask;
                idx = iv.baseidx_in_memmap + ((aligned - iv.base) >> 12);
                continue;
            }

            if (iv_end_idx - idx < page_count) break;   // 剩余不足，本区间放弃

            // 连续 page_count 页全 free 校验
            bool ok = true;
            for (uint64_t j = 0; j < page_count; j++) {
                if (pages_arr[idx + j].state != page_state_t::free) {
                    idx += (j + 1);
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;

            // 命中：分配 + 就地写账本 + 推进单区间光标（单调，不回落）
            if (state_set(paddr, page_count, state) != 0) return 0;   // 理论不可达
            iv.scan_base_idx = idx + page_count;
            return paddr;
        }
    }
    return 0;
}

// ================================================================
// early_alloc_close / early_alloc_open — 关闭控制位
// ================================================================
void page_frame_state_mgr::early_alloc_close() { early_alloc_closed = true; }
bool page_frame_state_mgr::early_alloc_open()  { return !early_alloc_closed; }

// ================================================================
// intervals_count_get / intervals_snapshot — 内部 interval 数组暴露
// ================================================================
uint64_t page_frame_state_mgr::intervals_count_get()
{
    return intervals_count;
}

// 堆上分配全部 interval 的副本（摘除 scan_base_idx 字段）：
//   不按状态/光标过滤——FPA 新初始化拿到全量区间边界后自行分桶，
//   从脏 order-0 页向上折叠；副本归调用方所有（delete[]）。
page_frame_state_mgr::interval_desc_t* page_frame_state_mgr::intervals_snapshot(uint64_t* out_count)
{
    if (!out_count) return nullptr;
    *out_count = 0;
    if (!intervals || intervals_count == 0) return nullptr;

    interval_desc_t* snap = new interval_desc_t[intervals_count];
    if (!snap) return nullptr;

    for (uint64_t i = 0; i < intervals_count; i++) {
        interval_desc_t& s = snap[i];
        s.base              = intervals[i].base;
        s.numof4kbpgs       = intervals[i].numof4kbpgs;
        s.baseidx_in_memmap = intervals[i].baseidx_in_memmap;
    }
    *out_count = intervals_count;
    return snap;
}

// ================================================================
// locate_interval — phyaddr 定位所属区间（二分；区间数少也可线性）
// ================================================================
page_frame_state_mgr::interval_t* page_frame_state_mgr::locate_interval(phyaddr_t base)
{
    if (!intervals || intervals_count == 0) return nullptr;
    uint64_t lo = 0;
    uint64_t hi = intervals_count;
    while (lo < hi) {
        const uint64_t mid = lo + ((hi - lo) >> 1);
        const interval_t& iv = intervals[mid];
        if (base < iv.base) {
            hi = mid;
            continue;
        }
        if (base - iv.base >= (iv.numof4kbpgs << 12)) {
            lo = mid + 1;
            continue;
        }
        return &intervals[mid];
    }
    return nullptr;
}
