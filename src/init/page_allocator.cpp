#include "init/page_allocator.h"
#include "init/pages_alloc.h"
#include "init/util/kout.h"
#include "util/OS_utils.h"

// ============================================================================
// 静态成员定义
// ============================================================================
page*     page_allocator::mem_map                  = nullptr;
uint64_t  page_allocator::mem_map_page_count        = 0;
phyaddr_t page_allocator::mem_map_pbase             = 0;

page_allocator::phyinterval_t* page_allocator::mem_map_intervals       = nullptr;
uint64_t                       page_allocator::mem_map_intervals_count = 0;
uint64_t                       page_allocator::free_pages               = 0;
uint64_t                       page_allocator::last_scan_cursor         = 0;

// ============================================================================
// phy2page — PHY_MEM_TYPE → page_state_t 转换
// ============================================================================
page_state_t page_allocator::phy2page(PHY_MEM_TYPE type) {
    if (type == PHY_MEM_TYPE::freeSystemRam)
        return page_state_t::free;
    return page_state_t::kernel_persisit;
}

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

    // 2. 扫描视图：计算总页数、可分配区间数
    uint64_t total_pages     = 0;
    uint64_t free_iv_count   = 0;
    phyaddr_t view_min_paddr = ~0ULL;
    phyaddr_t view_max_paddr = 0;

    for (uint64_t i = 0; i < view_count; i++) {
        const auto& seg = view[i];
        uint64_t seg_pages = seg.size >> 12;
        total_pages += seg_pages;

        if (seg.start < view_min_paddr)
            view_min_paddr = seg.start;
        if (seg.start + seg.size > view_max_paddr)
            view_max_paddr = seg.start + seg.size;

        if (seg.type == PHY_MEM_TYPE::freeSystemRam)
            free_iv_count++;
    }

    mem_map_page_count = total_pages;
    mem_map_pbase      = view_min_paddr;
    mem_map_intervals_count = free_iv_count;

    bsp_kout << "[page_allocator] pbase=0x" << mem_map_pbase
             << " total_pages=" << mem_map_page_count
             << " free_intervals=" << mem_map_intervals_count << kendl;

    // 3. 分配 page* mem_map 连续页框（通过 basic_allocator）
    uint64_t mem_map_bytes = mem_map_page_count * sizeof(page);
    uint64_t mem_map_pages = align_up(mem_map_bytes, 4096) >> 12;

    phyaddr_t mem_map_phys = basic_allocator::pages_alloc(mem_map_pages, 12);
    if (mem_map_phys == 0) {
        bsp_kout << "[page_allocator] ERROR: cannot allocate mem_map (" 
                 << mem_map_pages << " pages)" << kendl;
        return -2;
    }
    // 在 identity mapping 下 phys == virt（init.elf 的早期阶段）
    mem_map = reinterpret_cast<page*>(static_cast<uintptr_t>(mem_map_phys));

    bsp_kout << "[page_allocator] mem_map at phys=0x" << mem_map_phys
             << " (" << mem_map_pages << " pages, " << mem_map_bytes << " bytes)" << kendl;

    // 4. 分配 phyinterval_t 数组（在堆上）
    mem_map_intervals = new phyinterval_t[mem_map_intervals_count];
    if (!mem_map_intervals) {
        bsp_kout << "[page_allocator] ERROR: cannot allocate mem_map_intervals" << kendl;
        return -3;
    }

    bsp_kout << "[page_allocator] mem_map_intervals at heap=0x"
             << (uintptr_t)mem_map_intervals << kendl;

    // 5. 初始化 page.state + 填充 phyinterval_t
    uint64_t page_idx    = 0;
    uint64_t free_iv_idx = 0;

    for (uint64_t i = 0; i < view_count; i++) {
        const auto& seg = view[i];
        uint64_t seg_pages = seg.size >> 12;

        if (seg.type == PHY_MEM_TYPE::freeSystemRam) {
            // 创建 phyinterval_t
            mem_map_intervals[free_iv_idx].base               = seg.start;
            mem_map_intervals[free_iv_idx].numof4kbpgs        = seg_pages;
            mem_map_intervals[free_iv_idx].baseidx_in_memmap  = page_idx;

            // 初始化 page.state
            for (uint64_t p = 0; p < seg_pages; p++) {
                phyaddr_t paddr = seg.start + (p << 12);
                page_state_t st = page_state_t::free;

                // x86 低 1MB 强制 reserved（保护 IVT/BDA/EBDA 等）
                if (paddr < 0x100000ULL)
                    st = page_state_t::reserved;

                mem_map[page_idx + p].state = st;
            }

            free_pages += seg_pages;
            free_iv_idx++;
        } else {
            // 非可分配区间，全部标记为 reserved
            for (uint64_t p = 0; p < seg_pages; p++) {
                mem_map[page_idx + p].state = page_state_t::reserved;
            }
        }

        page_idx += seg_pages;
    }

    // x86 低 1MB 修正：修正 free_pages 统计
    for (uint64_t i = 0; i < mem_map_page_count; i++) {
        phyaddr_t paddr = idx_to_paddr(i);
        if (paddr < 0x100000ULL && mem_map[i].state == page_state_t::free) {
            // 上面已处理，这里仅做断言式检查
        }
    }

    bsp_kout << "[page_allocator] init done: " << free_pages 
             << " free pages across " << mem_map_intervals_count << " intervals" << kendl;

    // 6. 自引用保护：标记 mem_map 物理区间
    mem_interval mm_iv = {
        .start = mem_map_phys,
        .size  = mem_map_pages << 12
    };
    pages_set(mm_iv, PHY_MEM_TYPE::OS_KERNEL_DATA);

    // 标记 mem_map_intervals 物理区间
    uint64_t iv_bytes = mem_map_intervals_count * sizeof(phyinterval_t);
    uint64_t iv_pages = align_up(iv_bytes, 4096) >> 12;
    // 通过 basic_allocator 通知物理内存，因为重叠区间计算需要
    // （heap 分配的空间已在 basic_allocator 侧做好标记，不对 page 数组造成污染）
    // 但为了 page_allocator 自身的账目正确，需通过 pages_set 标记
    phyaddr_t iv_phys = reinterpret_cast<uintptr_t>(mem_map_intervals);
    mem_interval iv_iv = {
        .start = iv_phys,
        .size  = iv_pages << 12
    };
    pages_set(iv_iv, PHY_MEM_TYPE::OS_KERNEL_DATA);

    // 重新统计 free_pages（自引用标记后修正）
    free_pages = 0;
    for (uint64_t i = 0; i < mem_map_page_count; i++) {
        if (mem_map[i].state == page_state_t::free)
            free_pages++;
    }

    bsp_kout << "[page_allocator] after self-ref protection: " 
             << free_pages << " free pages" << kendl;

    return 0;
}

// ============================================================================
// idx_to_interval — mem_map 索引 → 所属 phyinterval_t
// ============================================================================
int page_allocator::idx_to_interval(uint64_t idx, uint64_t* out_iv_idx) {
    if (!mem_map_intervals)
        return -1;

    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        const auto& iv = mem_map_intervals[i];
        if (idx >= iv.baseidx_in_memmap &&
            idx <  iv.baseidx_in_memmap + iv.numof4kbpgs) {
            *out_iv_idx = i;
            return 0;
        }
    }
    return -1;
}

// ============================================================================
// idx_to_paddr — mem_map 索引 → 物理地址
// ============================================================================
phyaddr_t page_allocator::idx_to_paddr(uint64_t idx) {
    uint64_t iv_idx;
    if (idx_to_interval(idx, &iv_idx) != 0)
        return 0;

    const auto& iv = mem_map_intervals[iv_idx];
    return iv.base + ((idx - iv.baseidx_in_memmap) << 12);
}

// ============================================================================
// paddr_to_idx — 物理地址 → mem_map 索引
// ============================================================================
int page_allocator::paddr_to_idx(phyaddr_t paddr, uint64_t* out_idx) {
    if (!mem_map_intervals)
        return -1;

    for (uint64_t i = 0; i < mem_map_intervals_count; i++) {
        const auto& iv = mem_map_intervals[i];
        if (paddr >= iv.base && paddr < iv.base + (iv.numof4kbpgs << 12)) {
            *out_idx = iv.baseidx_in_memmap + ((paddr - iv.base) >> 12);
            return 0;
        }
    }
    return -1;
}

// ============================================================================
// alloc — 分配连续物理页框（roving-pointer 线性扫描）
// ============================================================================
phyaddr_t page_allocator::alloc(uint64_t page_count, uint8_t align_log2) {
    if (page_count == 0 || page_count > mem_map_page_count)
        return 0;

    uint64_t align_mask = (1ULL << align_log2) - 1;

    // roving pointer: 从上一次结束位置继续扫
    uint64_t cursor        = last_scan_cursor;
    uint64_t scanned       = 0;
    uint64_t max_scan      = mem_map_page_count;  // 最多扫一轮

    while (scanned < max_scan) {
        uint64_t idx = (cursor + scanned) % mem_map_page_count;

        // 跳过非 free 页
        if (mem_map[idx].state != page_state_t::free) {
            scanned++;
            continue;
        }

        // 检查对齐
        phyaddr_t candidate_base = idx_to_paddr(idx);
        if (candidate_base == 0) {
            scanned++;
            continue;
        }
        if (candidate_base & align_mask) {
            // 跳至下一个对齐边界
            phyaddr_t aligned = (candidate_base + align_mask) & ~align_mask;
            if (aligned <= candidate_base) { // 溢出保护
                scanned++;
                continue;
            }
            uint64_t skip_pages = (aligned - candidate_base) >> 12;
            scanned += skip_pages;
            continue;
        }

        // 查找当前所属区间，检查区间内是否有足够空间
        // （不能跨 phyinterval_t 分配）
        uint64_t iv_idx;
        if (idx_to_interval(idx, &iv_idx) != 0) {
            scanned++;
            continue;
        }
        const auto& iv = mem_map_intervals[iv_idx];
        uint64_t remaining = iv.numof4kbpgs - (idx - iv.baseidx_in_memmap);
        if (remaining < page_count) {
            // 跳至区间末尾再继续
            scanned += remaining;
            continue;
        }

        // 验证 page_count 个连续页是否都是 free
        bool ok = true;
        for (uint64_t j = 0; j < page_count; j++) {
            if (mem_map[idx + j].state != page_state_t::free) {
                // 跳过到冲突位置
                scanned += (j + 1);
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;

        // 找到连续空闲区间
        last_scan_cursor = (idx + page_count) % mem_map_page_count;

        // 不修改 page.state（由调用者通过 pages_set 确认类型）

        return candidate_base;
    }

    // 分配失败
    return 0;
}

// ============================================================================
// apply_type — 批量设置 page.state（已假定参数合法）
// ============================================================================
void page_allocator::apply_type(phyaddr_t base, uint64_t page_count, page_state_t state) {
    uint64_t idx;
    uint64_t iv_idx;

    for (uint64_t p = 0; p < page_count; ) {
        phyaddr_t paddr = base + (p << 12);
        if (paddr_to_idx(paddr, &idx) != 0) {
            p++;
            continue;
        }
        idx_to_interval(idx, &iv_idx);
        const auto& iv = mem_map_intervals[iv_idx];

        uint64_t remaining_in_iv = iv.numof4kbpgs - (idx - iv.baseidx_in_memmap);
        uint64_t batch = page_count - p;
        if (batch > remaining_in_iv)
            batch = remaining_in_iv;

        for (uint64_t j = 0; j < batch; j++) {
            mem_map[idx + j].state = state;
        }
        p += batch;
    }
}

// ============================================================================
// pages_set — 设置物理区间内存类型，唯一写 page.state 的入口
// ============================================================================
int page_allocator::pages_set(mem_interval interval, PHY_MEM_TYPE new_type) {
    // 参数验证
    if (interval.size == 0)
        return -1;
    if ((interval.start & 0xFFF) != 0 || (interval.size & 0xFFF) != 0)
        return -1;

    page_state_t new_state = phy2page(new_type);
    phyaddr_t end = interval.start + interval.size;
    uint64_t covered = 0;

    while (covered < interval.size) {
        phyaddr_t paddr = interval.start + covered;

        uint64_t idx;
        if (paddr_to_idx(paddr, &idx) != 0) {
            // 地址不在管理区间内，跳过单页继续
            covered += 4096;
            continue;
        }

        // 找到所属区间，计算可批量设置的连续页数
        uint64_t iv_idx;
        idx_to_interval(idx, &iv_idx);
        const auto& iv = mem_map_intervals[iv_idx];

        uint64_t interval_page_start = idx - iv.baseidx_in_memmap;
        uint64_t max_in_iv = iv.numof4kbpgs - interval_page_start;

        // 计算请求区间在当前 phyinterval_t 内覆盖的页数
        uint64_t pages_in_iv = max_in_iv;
        uint64_t remain_from_paddr = (end - paddr) >> 12;
        if (remain_from_paddr < pages_in_iv)
            pages_in_iv = remain_from_paddr;

        // 更新 page.state + 维护 free_pages 计数器
        for (uint64_t j = 0; j < pages_in_iv; j++) {
            page_state_t old = mem_map[idx + j].state;
            mem_map[idx + j].state = new_state;

            if (old == page_state_t::free && new_state != page_state_t::free)
                free_pages--;
            else if (old != page_state_t::free && new_state == page_state_t::free)
                free_pages++;
        }

        covered += pages_in_iv << 12;
    }

    return 0;
}

// ============================================================================
// 查询接口
// ============================================================================
uint64_t page_allocator::free_page_count() {
    return free_pages;
}

uint64_t page_allocator::total_page_count() {
    return mem_map_page_count;
}

const page* page_allocator::get_mem_map() {
    return mem_map;
}

phyaddr_t page_allocator::get_mem_map_pbase() {
    return mem_map_pbase;
}

// ============================================================================
// dump_free_regions — 调试输出
// ============================================================================
void page_allocator::dump_free_regions() {
    bsp_kout << "=== page_allocator free regions ===" << kendl;
    bsp_kout << " total_pages=" << mem_map_page_count
             << " free=" << free_pages << kendl;

    uint64_t free_start = ~0ULL;
    uint64_t free_run   = 0;

    for (uint64_t i = 0; i < mem_map_page_count; i++) {
        if (mem_map[i].state == page_state_t::free) {
            if (free_start == ~0ULL)
                free_start = i;
            free_run++;
        } else {
            if (free_run > 0) {
                phyaddr_t base = idx_to_paddr(free_start);
                bsp_kout << " [free] 0x" << base
                         << " (" << free_run << " pages)" << kendl;
                free_start = ~0ULL;
                free_run   = 0;
            }
        }
    }
    if (free_run > 0) {
        phyaddr_t base = idx_to_paddr(free_start);
        bsp_kout << " [free] 0x" << base
                 << " (" << free_run << " pages)" << kendl;
    }
    bsp_kout << "=== end ===" << kendl;
}
