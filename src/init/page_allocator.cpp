#include "init/page_allocator.h"
#include "init/pages_alloc.h"
#include "init/init_linker_symbols.h"
#include "init/util/kout.h"
#include "util/OS_utils.h"

// ============================================================================
// 静态成员定义
// ============================================================================
page*     page_allocator::mem_map                  = nullptr;
uint64_t  page_allocator::mem_map_page_count        = 0;
phyaddr_t page_allocator::mem_map_pbase             = 0;
uint64_t  page_allocator::mem_map_bytes             = 0;

page_allocator::phyinterval_t* page_allocator::mem_map_intervals       = nullptr;
uint64_t                       page_allocator::mem_map_intervals_count = 0;
uint64_t                       page_allocator::free_pages               = 0;

phyaddr_t page_allocator::scan_top_base    = 0;
phyaddr_t page_allocator::scan_down_base   = 0;
phyaddr_t page_allocator::dram_top_addr    = 0;

// ============================================================================
// init — 正式初始化
// ============================================================================
int page_allocator::init() {
    // 1. 获取 basic_allocator 的纯净内存视图
    uint64_t view_count = 0;
    phymem_segment* view = basic_allocator::get_pure_memory_view(&view_count);
    if (!view || view_count == 0) {
        bsp_kout << "[page_allocator] ERROR: get_pure_memory_view failed" << kendl;
        return -1;
    }

    // 2. 只对 freeSystemRam 进行注册
    uint64_t total_pages     = 0;
    uint64_t free_iv_count   = 0;
    phyaddr_t free_min_paddr = ~0ULL;
    phyaddr_t free_max_paddr = 0;

    for (uint64_t i = 0; i < view_count; i++) {
        if (view[i].type != PHY_MEM_TYPE::freeSystemRam) continue;
        uint64_t seg_pages = view[i].size >> 12;
        total_pages += seg_pages;
        free_iv_count++;
        if (view[i].start < free_min_paddr) free_min_paddr = view[i].start;
        uint64_t end = view[i].start + view[i].size;
        if (end > free_max_paddr) free_max_paddr = end;
    }

    bsp_kout << "[page_allocator] DEBUG: view_count=" << view_count
             << " free_iv=" << free_iv_count
             << " free_pages=" << total_pages << kendl;

    if (total_pages == 0 || free_iv_count == 0) {
        bsp_kout << "[page_allocator] ERROR: no free memory" << kendl;
        return -1;
    }

    mem_map_page_count = total_pages;
    mem_map_pbase      = free_min_paddr;
    mem_map_bytes      = mem_map_page_count * sizeof(page);
    mem_map_intervals_count = free_iv_count;

    bsp_kout << "[page_allocator] free: [0x" << free_min_paddr
             << " , 0x" << free_max_paddr << ")"
             << " pages=" << mem_map_page_count
             << " intervals=" << mem_map_intervals_count << kendl;

    // 3. 分配 page* mem_map
    uint64_t mem_map_pages = align_up(mem_map_bytes, 4096) >> 12;
    phyaddr_t mem_map_phys = basic_allocator::pages_alloc(mem_map_pages, 12);
    if (mem_map_phys == 0) {
        bsp_kout << "[page_allocator] ERROR: cannot allocate mem_map ("
                 << mem_map_pages << " pages)" << kendl;
        return -2;
    }
    mem_map = reinterpret_cast<page*>(static_cast<uintptr_t>(mem_map_phys));
    bsp_kout << "[page_allocator] mem_map at phys=0x" << mem_map_phys
             << " (" << mem_map_pages << " pages, " << mem_map_bytes << " bytes)" << kendl;

    // 4. 分配 phyinterval_t 数组
    mem_map_intervals = new phyinterval_t[mem_map_intervals_count];
    if (!mem_map_intervals) { return -3; }

    // 5. 填充 phyinterval_t + page.state (仅 freeSystemRam)
    uint64_t page_idx    = 0;
    uint64_t free_iv_idx = 0;

    for (uint64_t i = 0; i < view_count; i++) {
        const auto& seg = view[i];
        if (seg.type != PHY_MEM_TYPE::freeSystemRam) continue;

        uint64_t seg_pages = seg.size >> 12;
        mem_map_intervals[free_iv_idx].base               = seg.start;
        mem_map_intervals[free_iv_idx].numof4kbpgs        = seg_pages;
        mem_map_intervals[free_iv_idx].baseidx_in_memmap  = page_idx;

        for (uint64_t p = 0; p < seg_pages; p++) {
            phyaddr_t paddr = seg.start + (p << 12);
            page_state_t st = page_state_t::free;
            if (paddr < 0x100000ULL)
                st = page_state_t::reserved;
            mem_map[page_idx + p].state = st;
        }
        free_pages += seg_pages;
        page_idx   += seg_pages;
        free_iv_idx++;

        bsp_kout << "  [iv " << (free_iv_idx-1) << "] 0x" << seg.start
                 << " +0x" << seg.size
                 << " baseidx=" << mem_map_intervals[free_iv_idx-1].baseidx_in_memmap << kendl;
    }

    // 6. 初始化扫描光标
    {
        const auto& last_iv = mem_map_intervals[mem_map_intervals_count - 1];
        scan_top_base = last_iv.base + (last_iv.numof4kbpgs << 12);

        const auto& first_iv = mem_map_intervals[0];
        scan_down_base = (first_iv.base < 0x100000ULL) ? 0x100000ULL : first_iv.base;
    }

    // 8. 记录 DRAM 上界
    dram_top_addr = free_max_paddr;

    bsp_kout << "[page_allocator] scan_top_base=0x" << scan_top_base
             << " scan_down_base=0x" << scan_down_base
             << " dram_top=0x" << dram_top_addr << kendl;

    // 7. 自引用保护
    // 7a. 标记 mem_map 自身
    mem_interval mm_iv = { .start = mem_map_phys, .size = mem_map_pages << 12 };
    pages_set(mm_iv, page_state_t::reserved);

    // 7b. 标记 mem_map_intervals 数组
    uint64_t iv_bytes = mem_map_intervals_count * sizeof(phyinterval_t);
    uint64_t iv_pages = align_up(iv_bytes, 4096) >> 12;
    phyaddr_t iv_phys = reinterpret_cast<uintptr_t>(mem_map_intervals);
    mem_interval iv_iv = { .start = iv_phys, .size = iv_pages << 12 };
    pages_set(iv_iv, page_state_t::reserved);

    // 7c. 标记 init.elf 映像本身
    phyaddr_t init_img_start = reinterpret_cast<phyaddr_t>(&__init_text_start);
    phyaddr_t init_img_end   = reinterpret_cast<phyaddr_t>(&__init_heap_end);
    uint64_t init_img_size   = init_img_end - init_img_start;
    if (init_img_size > 0) {
        mem_interval init_iv = {
            .start = init_img_start,
            .size  = align_up(init_img_size, 4096)
        };
        pages_set(init_iv, page_state_t::reserved);
        bsp_kout << "[page_allocator] init.elf self-tag: 0x" << init_img_start
                 << " +0x" << init_iv.size << kendl;
    }

    // 重新统计 free_pages（自标记后各页面均可能被标记为 reserved）
    free_pages = 0;
    for (uint64_t i = 0; i < mem_map_page_count; i++) {
        if (mem_map[i].state == page_state_t::free)
            free_pages++;
    }

    bsp_kout << "[page_allocator] init done: "
             << free_pages << " free pages" << kendl;
    return 0;
}

// ============================================================================
// get_interval_by_addr — 通过物理地址定位区间描述符
// ============================================================================
page_allocator::phyinterval_t* page_allocator::get_interval_by_addr(phyaddr_t addr) {
    if (!mem_map_intervals) return nullptr;
    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        auto& iv = mem_map_intervals[i];
        if (addr >= iv.base && addr < iv.base + (iv.numof4kbpgs << 12))
            return &mem_map_intervals[i];
    }
    return nullptr;
}

// ============================================================================
// interval_top_to_bottom_ff_scan — 区间内从高地址向下首次适应扫描
//
// 返回被找到区间的物理基址（页面对齐）；若未找到返回 0。
// ============================================================================
phyaddr_t page_allocator::interval_top_to_bottom_ff_scan(
    phyinterval_t* iv, uint64_t page_count, uint8_t align_log2)
{
    uint64_t align_mask = (1ULL << align_log2) - 1;
    int64_t top_idx  = (int64_t)(iv->baseidx_in_memmap + iv->numof4kbpgs) - 1;
    int64_t base_idx = (int64_t)iv->baseidx_in_memmap;
    int64_t idx = top_idx;
    auto idx_to_top_addr = [iv](uint64_t idx) ->uint64_t{
        return iv->base+((idx-iv->baseidx_in_memmap+1)<<12);
    };
    auto phyaddr_to_idx = [iv](phyaddr_t paddr) ->uint64_t{
        return (uint64_t)(iv->baseidx_in_memmap+(((paddr - iv->base) >> 12)-1));
    };
    while (idx >= base_idx) {
        if (mem_map[idx].state != page_state_t::free) { idx--; continue; }

        phyaddr_t paddr = idx_to_top_addr(idx);

        // 对齐检查
        if (paddr & align_mask) {
            phyaddr_t aligned = align_down(paddr, 1<<align_log2);
            idx=phyaddr_to_idx(aligned);
            continue;
        }

        // 区间内剩余页数检查：从 idx 向上到区间末尾的页数
        uint64_t remaining = idx - base_idx+1;
        if (remaining < page_count) { 
            idx -= (int64_t)remaining;
             continue;
        }

        // 验证连续 page_count 页全 free
        bool ok = true;
        for (uint64_t j = 0; j < page_count; j++) {
            if (mem_map[(uint64_t)idx - j].state != page_state_t::free) {
                idx += (int64_t)j;
                ok = false;
                break;
            }
        }
        if (!ok) { idx--; continue; }

        return idx_to_top_addr(idx);
    }
    return 0;
}

// ============================================================================
// interval_bottom_to_top_ff_scan — 区间内从低地址向上首次适应扫描
//
// 返回被找到区间的物理基址（页面对齐）；若未找到返回 0。
// ============================================================================
phyaddr_t page_allocator::interval_bottom_to_top_ff_scan(
    phyinterval_t* iv, uint64_t page_count, uint8_t align_log2)
{
    uint64_t align_mask = (1ULL << align_log2) - 1;
    uint64_t top_excl = iv->baseidx_in_memmap + iv->numof4kbpgs;

    uint64_t idx = iv->baseidx_in_memmap;
    while (idx < top_excl) {
        if (mem_map[idx].state != page_state_t::free) { idx++; continue; }

        phyaddr_t paddr = iv->base + ((idx - iv->baseidx_in_memmap) << 12);
        auto idx_to_paddr = [&](uint64_t idx) -> phyaddr_t {
            return iv->base + ((idx - iv->baseidx_in_memmap) << 12);
        };
        auto paddr_to_idx = [&](phyaddr_t paddr) -> uint64_t {
            return iv->baseidx_in_memmap+((paddr - iv->base) >> 12);
        };
        // 对齐检查
        if (paddr & align_mask) {
            phyaddr_t aligned = align_up(paddr, align_mask+1);
            idx=paddr_to_idx(aligned);
            continue;
        }

        // 区间内剩余页数检查
        uint64_t remaining = top_excl - idx;
        if (remaining < page_count) break; // 本区间不可能够

        // 验证连续 page_count 页全 free
        bool ok = true;
        for (uint64_t j = 0; j < page_count; j++) {
            if (mem_map[idx + j].state != page_state_t::free) {
                idx += (j + 1);
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        return idx_to_paddr(idx);
    }
    return 0;
}

// ============================================================================
// available_meminterval_probe — 瞬态端：从 scan_top_base 向下扫描
//
// 扫描顺序：从包含 scan_top_base 的区间开始，逐区间下降。
// 成功后 advance scan_top_base 至找到的物理基址。
// ============================================================================
phyaddr_t page_allocator::available_meminterval_probe(
    uint64_t page_count, uint8_t align_log2)
{
    if (page_count == 0 || !mem_map_intervals) return 0;

    for (int64_t i = (int64_t)mem_map_intervals_count - 1; i >= 0; i--) {
        auto& iv = mem_map_intervals[i];
        phyaddr_t iv_start = iv.base;

        // 区间完全在光标之上 → 已被瞬态消耗，跳过
        if (iv_start >= scan_top_base) continue;

        // 光标落在此区间内 → 限制扫描范围不侵入光标之上
        // 区间完全在光标之下 → 全区间扫描
        phyaddr_t result = interval_top_to_bottom_ff_scan(&iv, page_count, align_log2);
        if (result) {
            scan_top_base = result;   // 光标推进至找到的基址
            bsp_kout<<"found top at 0x"<<HEX<<result<<" base at 0x"<<HEX<<(result-(page_count<<12))<<DEC<<kendl;
            return result;
        }
    }
    return 0;
}

// ============================================================================
// available_meminterval_probe_keep — 保持端：从 scan_down_base 向上扫描
//
// 扫描顺序：从包含 scan_down_base 的区间开始，逐区间上升。
// 成功后 advance scan_down_base 至找到区间末尾（page_count 页之后）。
// ============================================================================
phyaddr_t page_allocator::available_meminterval_probe_keep(
    uint64_t page_count, uint8_t align_log2)
{
    if (page_count == 0 || !mem_map_intervals) return 0;

    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        auto& iv = mem_map_intervals[i];
        phyaddr_t iv_end = iv.base + (iv.numof4kbpgs << 12);

        // 区间完全在光标之下 → 已被保持端消耗，跳过
        if (iv_end <= scan_down_base) continue;

        phyaddr_t result = interval_bottom_to_top_ff_scan(&iv, page_count, align_log2);
        if (result) {
            
            scan_down_base = result + (page_count << 12);  // 光标推进至区间末尾
            bsp_kout<<"found base at 0x"<<HEX<<result<<" end at 0x"<<HEX<<scan_down_base<<DEC<<kendl;
            return result;
        }
    }
    return 0;
}

// ============================================================================
// interval_set — 在指定区间描述符的内存页框中设置状态
//
// 通过 get_interval_by_addr 找到完全包住目标区间的描述符。
// 若目标区间跨区间则报错（区间未完全落入单个 phyinterval_t 子集）。
// ============================================================================
int page_allocator::interval_set(mem_interval interval, page_state_t state) {
    if (interval.size == 0) return -1;
    if ((interval.start & 0xFFF) != 0 || (interval.size & 0xFFF) != 0) return -1;

    phyinterval_t* iv = get_interval_by_addr(interval.start);
    if (!iv) return -1;

    phyaddr_t iv_end = iv->base + (iv->numof4kbpgs << 12);
    phyaddr_t req_end = interval.start + interval.size;

    // 检查：目标区间必须完全落在单个 phyinterval_t 内
    if (req_end > iv_end) return -1;

    uint64_t start_idx = iv->baseidx_in_memmap
                         + ((interval.start - iv->base) >> 12);
    uint64_t page_count = interval.size >> 12;

    for (uint64_t j = 0; j < page_count; j++) {
        page_state_t old = mem_map[start_idx + j].state;
        mem_map[start_idx + j].state = state;
        if (old == page_state_t::free && state != page_state_t::free) free_pages--;
        else if (old != page_state_t::free && state == page_state_t::free) free_pages++;
    }
    return 0;
}

// ============================================================================
// pages_set — 设置物理区间内存类型（public 入口）
// ============================================================================
int page_allocator::pages_set(mem_interval interval, page_state_t state) {
    return interval_set(interval, state);
}

// ============================================================================
// 查询接口
// ============================================================================
uint64_t page_allocator::free_page_count()   { return free_pages; }
uint64_t page_allocator::total_page_count()  { return mem_map_page_count; }
const page* page_allocator::get_mem_map()    { return mem_map; }
phyaddr_t page_allocator::get_mem_map_pbase(){ return mem_map_pbase; }
phyaddr_t page_allocator::dram_top()          { return dram_top_addr; }

// ============================================================================
// relinquish_mem_map — 自裁接口
// ============================================================================
void page_allocator::relinquish_mem_map(phyaddr_t* out_pbase, uint64_t* out_pcount) {
    if (out_pbase)  *out_pbase  = mem_map_pbase;
    if (out_pcount) *out_pcount = mem_map_page_count;

    bsp_kout << "[page_allocator] relinquish: pbase=0x" << mem_map_pbase
             << " pages=" << mem_map_page_count
             << " bytes=" << mem_map_bytes << kendl;

    // 清零 mem_map 再交出
    if (mem_map) {
        ksetmem_8((void*)(uint64_t)mem_map_pbase, 0, mem_map_bytes);
    }

    // 撕毁所有内部状态
    mem_map                  = nullptr;
    mem_map_page_count       = 0;
    mem_map_pbase            = 0;
    mem_map_bytes            = 0;
    mem_map_intervals        = nullptr;
    mem_map_intervals_count  = 0;
    free_pages               = 0;
    scan_top_base            = 0;
    scan_down_base           = 0;
    dram_top_addr            = 0;
}
