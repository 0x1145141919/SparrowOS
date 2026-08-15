#include "init/page_allocator_v2.h"
#include "init/pages_alloc.h"           // basic_allocator：纯洁视图 / mem_map 缓冲自举
#include "init/init_linker_symbols.h"    // __init_text_start / __init_heap_end（init 镜像自标记）
#include "util/OS_utils.h"               // align_up / ksetmem_8
#include "init/util/kout.h"              // bsp_kout（init.elf 侧输出）
#include <new>

// ════════════════════════════════════════════════════════════════
// page_allocator_v2 实现
//
// 核心不变量：
//   - 纯洁视图（basic_allocator 的 pure_mem_view）是唯一物理内存描述表：一次生成、
//     升序固定、Init 后不可变。free_seg_descriptor_t.in_pure_memview_idx 就是它的下标，
//     kernel 侧 phymem_segments 是它的逐字拷贝 → 下标两世界解析一致。
//   - mem_map（page[]）是唯一状态账本：free/非-free 由 page_state_t 表达，
//     pages_set 是唯一写入口；free_ram_explore 只勘探不改状态。
//   - 勘探成功即推进 scan_base（单调，不回落）；调用方必须随后 pages_set 提交，
//     否则该区域被跳过且穿越账本错误。
// ════════════════════════════════════════════════════════════════

// ---------- 静态数据成员定义（纯静态类：全局唯一账本） ----------
page*                page_allocator_v2::mem_map               = nullptr;
uint64_t             page_allocator_v2::mem_map_page_count    = 0;
phyaddr_t            page_allocator_v2::mem_map_pbase         = 0;
uint64_t             page_allocator_v2::mem_map_bytes         = 0;
free_seg_descriptor_t* page_allocator_v2::mem_map_intervals   = nullptr;
uint64_t             page_allocator_v2::mem_map_intervals_count = 0;
uint64_t             page_allocator_v2::free_pages            = 0;
phyaddr_t            page_allocator_v2::dram_top_addr         = 0;
phyaddr_t            page_allocator_v2::scan_base             = 0;

namespace {
// 纯洁视图缓存（basic_allocator 的静态快照，Init 后不可变，不持有所有权）
phymem_segment* g_pure_view       = nullptr;
uint64_t        g_pure_view_count = 0;

// 由 free_seg_descriptor_t 解析出 [base, pages)。
// in_pure_memview_idx 指向纯洁视图中对应的 freeSystemRam 段——唯一性来源，
// base/长度不再自携带，消除双份拷贝漂移。
void resolve_seg(const free_seg_descriptor_t* iv, phyaddr_t& base, uint64_t& pages) {
    const phymem_segment& seg = g_pure_view[iv->in_pure_memview_idx];
    base  = seg.start;
    pages = seg.size >> 12;
}
} // namespace

// ================================================================
// init — 建立 mem_map（逻辑复刻历史 page_allocator::init）
// ================================================================
int page_allocator_v2::init() {
    // 1. 纯洁视图（唯一物理内存描述表）
    uint64_t view_count = 0;
    phymem_segment* view = basic_allocator::get_pure_memory_view(&view_count);
    if (!view || view_count == 0) return -1;
    g_pure_view       = view;
    g_pure_view_count = view_count;

    // 2. 仅统计 freeSystemRam 段（mem_map 只覆盖可分配物理内存）
    uint64_t total_pages  = 0;
    uint64_t free_iv_count = 0;
    phyaddr_t free_max    = 0;
    for (uint64_t i = 0; i < view_count; i++) {
        if (view[i].type != PHY_MEM_TYPE::freeSystemRam) continue;
        total_pages += view[i].size >> 12;
        free_iv_count++;
        const phyaddr_t end = view[i].start + view[i].size;
        if (end > free_max) free_max = end;
    }
    if (total_pages == 0 || free_iv_count == 0) return -2;

    mem_map_page_count     = total_pages;
    mem_map_bytes          = total_pages * sizeof(page);
    mem_map_intervals_count = free_iv_count;

    // 3. 挖 mem_map 缓冲（4KB 对齐；物理内存经 UEFI 恒等映射直接访问）
    const uint64_t buf_pages = align_up(mem_map_bytes, 0x1000) >> 12;
    const phyaddr_t buf_phys = basic_allocator::pages_alloc(buf_pages << 12, 12);
    if (buf_phys == ~0ull || buf_phys == 0) return -3;
    mem_map_pbase = buf_phys;
    mem_map       = reinterpret_cast<page*>(static_cast<uintptr_t>(buf_phys));

    // 4. 区间描述数组（每 freeSystemRam 段一个，索引式引用纯洁视图）
    mem_map_intervals = new free_seg_descriptor_t[free_iv_count];
    if (!mem_map_intervals) return -4;

    // 5. 填区间 + page.state（free；低 1MB 段内 reserved）
    uint64_t page_idx = 0;
    uint64_t iv_idx   = 0;
    for (uint64_t i = 0; i < view_count; i++) {
        if (view[i].type != PHY_MEM_TYPE::freeSystemRam) continue;
        free_seg_descriptor_t& d = mem_map_intervals[iv_idx];
        d.in_pure_memview_idx = i;
        d.baseidx_in_memmap   = page_idx;

        const uint64_t pages = view[i].size >> 12;
        for (uint64_t p = 0; p < pages; p++) {
            const phyaddr_t paddr = view[i].start + (p << 12);
            // 低 1MB：实模式 IVT/BDA/EBDA/VGA ROM/BIOS，预留
            mem_map[page_idx + p].state =
                (paddr < 0x100000ULL) ? page_state_t::reserved : page_state_t::free;
        }
        page_idx += pages;
        iv_idx++;
    }

    // 6. 单光标初值：首个区间基址（≥1MB）
    {
        phyaddr_t base; uint64_t pages;
        resolve_seg(&mem_map_intervals[0], base, pages);
        scan_base = (base < 0x100000ULL) ? 0x100000ULL : base;
    }

    // 7. dram_top（freeSystemRam 物理上界）
    dram_top_addr = free_max;

    // 8. 自引用保护：mem_map 缓冲 / 区间数组 / init 镜像。
    //    任一失败 = 该区域跨越内存空洞，账本无法表达 → 致命，返回负值。
    // 注：堆上对象（new[] 区间数组）与链接符号起点不保证 4KB 对齐，
    //        故统一向下/向上取整到页边界再标记（多标的页也归 init 所有，安全）。
    // 语义态：mem_map 缓冲 = pages_arr 资产 → kernel_persisit（内核持久元数据）；
    //         区间数组/init 镜像是 init 临时财产（不穿越，镜像 4.5 归还 free）→ init_tmp_property。
    if (pages_set({buf_phys, buf_pages << 12}, page_state_t::kernel_persisit) != 0) return -5;
    {
        const uint64_t iv_bytes = free_iv_count * sizeof(free_seg_descriptor_t);
        const phyaddr_t iv_phys = reinterpret_cast<uintptr_t>(mem_map_intervals);
        const phyaddr_t iv_lo   = align_down(iv_phys, 0x1000);
        const phyaddr_t iv_hi   = align_up(iv_phys + iv_bytes, 0x1000);
        if (pages_set({iv_lo, iv_hi - iv_lo}, page_state_t::init_tmp_property) != 0) return -5;
    }
    {
        const phyaddr_t img_lo = align_down((uint64_t)&__init_text_start, 0x1000);
        const phyaddr_t img_hi = align_up((uint64_t)&__init_heap_end, 0x1000);
        if (pages_set({img_lo, img_hi - img_lo}, page_state_t::init_tmp_property) != 0) return -5;
    }

    // 9. 重算 free_pages（自引用保护后）
    free_pages = 0;
    for (uint64_t i = 0; i < mem_map_page_count; i++)
        if (mem_map[i].state == page_state_t::free) free_pages++;

    bsp_kout << "[page_allocator_v2] init done: intervals=" << (uint64_t)free_iv_count
             << " total_pages=" << (uint64_t)mem_map_page_count
             << " free=" << (uint64_t)free_pages
             << " mem_map@0x" << HEX << (uint64_t)mem_map_pbase
             << " dram_top=0x" << (uint64_t)dram_top_addr << DEC << kendl;
    return 0;
}

// ================================================================
// free_ram_explore — 勘探（非消费）：找到满足的连续空闲段，推进 scan_base
// ================================================================
phyaddr_t page_allocator_v2::free_ram_explore(uint64_t page_count, uint8_t align_log2) {
    if (page_count == 0 || !mem_map_intervals || mem_map_intervals_count == 0) return 0;
    if (align_log2 < 12) align_log2 = 12;

    // 跨区间自下而上 first-fit：跳过完全位于光标之下的区间
    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        free_seg_descriptor_t& iv = mem_map_intervals[i];
        phyaddr_t base; uint64_t pages;
        resolve_seg(&iv, base, pages);
        if (base + (pages << 12) <= scan_base) continue;

        const phyaddr_t result = interval_bottom_to_top_ff_scan(&iv, page_count, align_log2);
        if (result) {
            // 勘探成功即推进单光标（单调，不回落）
            scan_base = result + (page_count << 12);
            bsp_kout << "[page_allocator_v2] explore: base=0x" << HEX << (uint64_t)result
                     << " end=0x" << (uint64_t)scan_base
                     << " pages=" << DEC << (uint64_t)page_count << kendl;
            return result;
        }
    }
    return 0;
}

// ================================================================
// interval_bottom_to_top_ff_scan — 单区间内自下而上 first-fit
// ================================================================
phyaddr_t page_allocator_v2::interval_bottom_to_top_ff_scan(
    free_seg_descriptor_t* iv, uint64_t page_count, uint8_t align_log2)
{
    phyaddr_t base; uint64_t pages;
    resolve_seg(iv, base, pages);
    const uint64_t align_mask = (1ull << align_log2) - 1;
    const uint64_t top_excl   = iv->baseidx_in_memmap + pages;
    uint64_t idx = iv->baseidx_in_memmap;

    while (idx < top_excl) {
        if (mem_map[idx].state != page_state_t::free) { idx++; continue; }

        const phyaddr_t paddr = base + ((idx - iv->baseidx_in_memmap) << 12);
        // 对齐前跳：候选起点必须满足物理对齐
        if (paddr & align_mask) {
            const phyaddr_t aligned = (paddr + align_mask) & ~align_mask;
            idx = iv->baseidx_in_memmap + ((aligned - base) >> 12);
            continue;
        }

        if (top_excl - idx < page_count) break;   // 本区间剩余不足，放弃

        // 连续 page_count 页全 free 校验
        bool ok = true;
        for (uint64_t j = 0; j < page_count; j++) {
            if (mem_map[idx + j].state != page_state_t::free) {
                idx += (j + 1);
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        return base + ((idx - iv->baseidx_in_memmap) << 12);
    }
    return 0;
}

// ================================================================
// pages_set — 状态写入（mem_map 唯一写入口），跨区间遍历
// ================================================================
int page_allocator_v2::pages_set(mem_interval interval, page_state_t state) {
    if (interval.size == 0) return -1;
    if ((interval.start & 0xFFF) != 0 || (interval.size & 0xFFF) != 0) return -1;
    if (!mem_map_intervals || mem_map_intervals_count == 0) return -1;

    const phyaddr_t start = interval.start;
    const phyaddr_t end   = start + interval.size;
    if (end <= start) return -1;   // 溢出

    // 遍历相交区间；覆盖不足（空洞/越界）= 账本无法表达 → -1
    uint64_t covered = 0;
    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        free_seg_descriptor_t& iv = mem_map_intervals[i];
        phyaddr_t base; uint64_t pages;
        resolve_seg(&iv, base, pages);
        const phyaddr_t iv_end = base + (pages << 12);
        if (end <= base || start >= iv_end) continue;   // 不相交

        const phyaddr_t seg_lo    = (start > base) ? start : base;
        const phyaddr_t seg_hi    = (end < iv_end) ? end : iv_end;
        const uint64_t  start_idx = iv.baseidx_in_memmap + ((seg_lo - base) >> 12);
        const uint64_t  count     = (seg_hi - seg_lo) >> 12;
        if (interval_set(&iv, start_idx, count, state) != 0) return -1;
        covered += count << 12;
    }
    if (covered != interval.size) return -1;
    return 0;
}

// ================================================================
// interval_set — 单区间内 [start_idx, +page_count) 条目置状态 + free_pages 记账
// ================================================================
int page_allocator_v2::interval_set(free_seg_descriptor_t* iv, uint64_t start_idx,
                                    uint64_t page_count, page_state_t state)
{
    phyaddr_t base; uint64_t pages;
    resolve_seg(iv, base, pages);
    const uint64_t iv_start = iv->baseidx_in_memmap;
    const uint64_t iv_end   = iv_start + pages;
    if (start_idx < iv_start) return -1;
    if (start_idx + page_count > iv_end) return -1;

    for (uint64_t j = 0; j < page_count; j++) {
        const page_state_t old = mem_map[start_idx + j].state;
        mem_map[start_idx + j].state = state;
        if (old == page_state_t::free && state != page_state_t::free) free_pages--;
        else if (old != page_state_t::free && state == page_state_t::free) free_pages++;
    }
    return 0;
}

// ================================================================
// get_interval_by_addr — 物理地址定位所属区间（线性扫描；区间数少）
// ================================================================
free_seg_descriptor_t* page_allocator_v2::get_interval_by_addr(phyaddr_t addr) {
    if (!mem_map_intervals || mem_map_intervals_count == 0) return nullptr;
    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        free_seg_descriptor_t& iv = mem_map_intervals[i];
        phyaddr_t base; uint64_t pages;
        resolve_seg(&iv, base, pages);
        if (addr >= base && addr < base + (pages << 12)) return &iv;
    }
    return nullptr;
}

// ================================================================
// 查询
// ================================================================
uint64_t page_allocator_v2::free_page_count()  { return free_pages; }
uint64_t page_allocator_v2::total_page_count() { return mem_map_page_count; }
phyaddr_t page_allocator_v2::dram_top()        { return dram_top_addr; }
phyaddr_t page_allocator_v2::get_mem_map_pbase() { return mem_map_pbase; }
free_seg_descriptor_t* page_allocator_v2::get_free_segs() { return mem_map_intervals; }
uint64_t               page_allocator_v2::get_free_segs_count() { return mem_map_intervals_count; }
