#include "init/init_bcb_juvenile.h"
#ifdef KERNEL_MODE
#include "init/pages_alloc.h"   // basic_allocator：位图池内部挖取（生产路径）
#endif
#include <new>

// ════════════════════════════════════════════════════════════════
// init_bcb_juvenile — init.elf 阶段幼年 BCB 分配器（纯静态类，独立可测单元）
//
// 显式参数。plan_and_setup 的输入全部来自 bcb_juvenile_init_config
// （内存视图 + strategy 链条 + 池来源）。位图池两种来源：
//   POOL_SOURCE_BASIC_ALLOCATOR → 生产路径：plan_and_setup 内部经
//                                  basic_allocator 挖池（KERNEL_MODE 构建）
//   POOL_SOURCE_CALLER          → 调用方预置 region_pbase_：
//     region_pbase_ == 0   → plan-only：只算 plan（bcbs_/descs_），
//                            不写位图；region_size_ 输出所需池字节数。
//     region_pbase_ != 0   → full：在调用方提供的池上铺全空闲叶子 +
//                            池自保护（池页在所属 BCB 叶子置占用）。
// ════════════════════════════════════════════════════════════════

namespace {

constexpr uint8_t  MIN_BCB_ORDER = 10;      // 低于此阶进 crumbs（丢弃）
constexpr uint64_t PAGE_SIZE      = 4096;

// 平台无关零填充（freestanding 安全，不依赖 libc）
void zero_bytes(void* p, uint64_t n) {
    uint8_t* b = static_cast<uint8_t*>(p);
    while (n--) *b++ = 0;
}

// 幂 2 对齐上取整（a 须为 2 的幂）
uint64_t align_up_2pow(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

struct plan_entry {
    phyaddr_t base;
    uint8_t   order;
};

// 分桶：把一段 freeSystemRam 切成最大对齐的 2 幂块（base 升序）。
// 返回块数；out 非空时逐块写入。与 FPA::Init 的候选生成一致：
// order_max 随 base 推进重新计算，低对齐段头部小块自然落入 crumbs。
uint64_t bucket_segment(phyaddr_t base, uint64_t size, plan_entry* out) {
    const phyaddr_t end = base + (size & ~(PAGE_SIZE - 1));   // 截到页对齐
    uint64_t n = 0;
    while (base < end) {
        uint8_t order = 0;
        if (base != 0) {
            const uint32_t tz = __builtin_ctzll(base);
            order = (tz > 12) ? static_cast<uint8_t>(tz - 12) : 0;
        }
        uint64_t top = base + (1ull << (order + 12));
        while (top > end) {
            if (order == 0) break;
            --order;
            top = base + (1ull << (order + 12));
        }
        if (top > end) break;                 // 残余不足 1 页，放弃
        if (out) {
            out[n].base  = base;
            out[n].order = order;
        }
        ++n;
        base = top;
    }
    return n;
}

} // namespace

// ── 静态数据成员定义（纯静态类：全局唯一账本） ──
bcb_state*  init_bcb_juvenile::bcbs_          = nullptr;
uint64_t    init_bcb_juvenile::bcb_count_     = 0;
phyaddr_t   init_bcb_juvenile::region_pbase_  = 0;
uint64_t    init_bcb_juvenile::region_size_   = 0;
uint64_t    init_bcb_juvenile::scan_hint_bcb_ = 0;
bcb_desc_v2_t* init_bcb_juvenile::descs_      = nullptr;

// ================================================================
// plan_and_setup — 算 plan → 铺叶子（全空闲）→ 活
// ================================================================
loc_code_t init_bcb_juvenile::plan_and_setup(bcb_juvenile_init_config* cfg)
{
    // 重入清理（plan-only → full 二次调用时释放上次内部数组）
    if (descs_) { delete[] descs_; descs_ = nullptr; }
    if (bcbs_)  { delete[] bcbs_;  bcbs_  = nullptr; }
    bcb_count_     = 0;
    scan_hint_bcb_ = 0;

    auto fail_cleanup = [&]() -> loc_code_t {
        if (descs_) { delete[] descs_; descs_ = nullptr; }
        if (bcbs_)  { delete[] bcbs_;  bcbs_  = nullptr; }
        bcb_count_ = 0;
        return SRC_LOC();
    };

    if (!cfg || !cfg->segs || cfg->segs_count == 0) return SRC_LOC();

    // ---- 1. 收集 freeSystemRam 段 + 总空闲页 ----
    uint64_t total_free_pages = 0;
    for (uint64_t i = 0; i < cfg->segs_count; ++i) {
        if (cfg->segs[i].type != freeSystemRam) continue;
        total_free_pages += cfg->segs[i].size >> 12;
    }
    if (total_free_pages == 0) return SRC_LOC();

    // ---- 2. 分桶：先统计候选总数，再填充 ----
    uint64_t candidate_count = 0;
    for (uint64_t i = 0; i < cfg->segs_count; ++i) {
        if (cfg->segs[i].type != freeSystemRam) continue;
        candidate_count += bucket_segment(cfg->segs[i].start, cfg->segs[i].size, nullptr);
    }
    if (candidate_count == 0) return SRC_LOC();

    plan_entry* cand = new plan_entry[candidate_count];
    if (!cand) return SRC_LOC();
    uint64_t c = 0;
    for (uint64_t i = 0; i < cfg->segs_count; ++i) {
        if (cfg->segs[i].type != freeSystemRam) continue;
        c += bucket_segment(cfg->segs[i].start, cfg->segs[i].size, cand + c);
    }

    // ---- 3. crumbs 分流：order >= MIN_BCB_ORDER 才留 ----
    plan_entry* sel = new plan_entry[candidate_count];
    if (!sel) { delete[] cand; return SRC_LOC(); }
    uint64_t sel_count = 0;
    for (uint64_t i = 0; i < candidate_count; ++i) {
        if (cand[i].order >= MIN_BCB_ORDER)
            sel[sel_count++] = cand[i];
    }
    delete[] cand;

    // ---- 4. strategy 变换（预留参数位） ----
    uint8_t fit_order = MIN_BCB_ORDER;
    if (cfg->strategy == bcb_juvenile_init_config::INIT_STRATEGY_MATCH_THREAD) {
        uint64_t cpu = cfg->logical_processor_count;
        if (cpu == 0) cpu = 1;
        uint64_t coeff = (cfg->thread_coefficient >= 1) ? cfg->thread_coefficient : 1;
        uint64_t per_thread_bytes = (total_free_pages << 12) / (cpu * coeff);
        fit_order = 0;
        if (per_thread_bytes >= (1ull << 12))
            fit_order = static_cast<uint8_t>((63 - __builtin_clzll(per_thread_bytes)) - 12);
        if (fit_order < MIN_BCB_ORDER) fit_order = MIN_BCB_ORDER;
    }

    // 上界：每个候选 1 块；MATCH_THREAD 时大块切成 2^(order-fit) 块
    uint64_t plan_cap = 0;
    for (uint64_t i = 0; i < sel_count; ++i) {
        plan_cap += (sel[i].order > fit_order) ? (1ull << (sel[i].order - fit_order)) : 1;
    }
    if (plan_cap == 0) { delete[] sel; return SRC_LOC(); }

    plan_entry* plan = new plan_entry[plan_cap];
    if (!plan) { delete[] sel; return SRC_LOC(); }
    uint64_t plan_count = 0;

    if (cfg->strategy == bcb_juvenile_init_config::INIT_STRATEGY_MATCH_THREAD) {
        for (uint64_t i = 0; i < sel_count; ++i) {
            if (sel[i].order <= fit_order) {
                plan[plan_count++] = sel[i];
            } else {
                const uint64_t split       = 1ull << (sel[i].order - fit_order);
                const uint64_t split_size  = 1ull << (fit_order + 12);
                for (uint64_t k = 0; k < split; ++k) {
                    plan[plan_count++] = {
                        static_cast<phyaddr_t>(sel[i].base + k * split_size), fit_order
                    };
                }
            }
        }
    } else {   // BEST_ALIGN_FIT：贪心结果直接采纳
        for (uint64_t i = 0; i < sel_count; ++i) plan[plan_count++] = sel[i];
    }
    delete[] sel;

    if (plan_count == 0) { delete[] plan; return SRC_LOC(); }

    // ---- 5. 按 base 升序排序（防御性，插入排序；桶天然近序） ----
    for (uint64_t i = 1; i < plan_count; ++i) {
        const plan_entry key = plan[i];
        uint64_t j = i;
        while (j > 0 && plan[j - 1].base > key.base) {
            plan[j] = plan[j - 1];
            --j;
        }
        plan[j] = key;
    }

    // ---- 6. 位图池预算：3bit × 总空闲页（现行算法），上取整到页 ----
    region_size_ = align_up_2pow((total_free_pages * 3 + 7) >> 3, PAGE_SIZE);
    if (region_size_ == 0) { delete[] plan; return SRC_LOC(); }

    // ---- 6.5 池来源：BASIC_ALLOCATOR 生产路径内部挖池 ----
    //     池经 basic_allocator 挖取后 pages_set 标记（防 basic_allocator 复用），
    //     并在 step 8 池自保护钉入所属 BCB 叶子（防 BCB 自分配）。一次调用即 full。
    if (cfg->pool == bcb_juvenile_init_config::POOL_SOURCE_BASIC_ALLOCATOR) {
#ifdef KERNEL_MODE
        phyaddr_t pool = basic_allocator::pages_alloc(region_size_, 12);
        if (pool == ~0ull) pool = 0;
        if (pool == 0) {
            region_pbase_ = 0;
            delete[] plan;
            return fail_cleanup();
        }
        basic_allocator::pages_set({pool, region_size_}, PHY_MEM_TYPE::OS_KERNEL_DATA);
        region_pbase_ = pool;
#else
        // 用户态构建（测试）无 basic_allocator：内部挖池不可用
        delete[] plan;
        return fail_cleanup();
#endif
    }

    // ---- 7. 建立 bcbs_ / descs_（池内 8B 对齐逐 BCB 切片） ----
    bcbs_  = new bcb_state[plan_count];
    descs_ = new bcb_desc_v2_t[plan_count];
    if (!bcbs_ || !descs_) {
        delete[] plan;
        return fail_cleanup();
    }
    bcb_count_ = plan_count;

    uint64_t cursor = region_pbase_;
    for (uint64_t i = 0; i < plan_count; ++i) {
        const uint8_t  ord  = plan[i].order;
        const uint64_t need = bcb_region_bytes(ord);          // (3<<N)>>3，N≥10 时恒为 8 的倍数
        uint64_t slice = cursor;
        if (slice & 7ull) slice = (slice + 7ull) & ~7ull;
        if (slice + need > region_pbase_ + region_size_) {    // 池容量校验
            delete[] plan;
            return fail_cleanup();
        }

        bcbs_[i].base          = plan[i].base;
        bcbs_[i].max_order     = ord;
        bcbs_[i].region_va     = slice;                       // 恒等映射：虚拟 == 物理
        bcbs_[i].free_leaves   = 1ull << ord;
        bcbs_[i].scan_hint_off_ = 0;

        // v2 描述打包：bcb_district_descriptor = [0:5]order | [12:63]base_pa[12:63]
        // base 页对齐（低 12bit 恒 0），order 限 6 bit（N ≤ 63，实际远小于）
        descs_[i].bcb_district_descriptor = (plan[i].base & ~0xFFFull) |
                                            (static_cast<uint64_t>(ord) & 0x3Full);
        descs_[i].bitmap_region_base_pa   = slice;

        cursor = slice + need;
    }
    delete[] plan;

    // ---- 8. 铺位图（仅当池已提供，full 模式） ----
    if (region_pbase_ != 0) {
        // 8a. 整池清零：内部节点区 = 0 (NODE_NONEXIST)，叶子 = 0 (占用)
        zero_bytes(reinterpret_cast<void*>(static_cast<uintptr_t>(region_pbase_)), region_size_);//用mem_set

        // 8b. 全叶子置空闲（1）
        for (uint64_t i = 0; i < bcb_count_; ++i) {
            const uint64_t leaf_cnt = 1ull << bcbs_[i].max_order;
            for (uint64_t o = 0; o < leaf_cnt; ++o)
                leaf_set(i, o, true);
        }

        // 8c. 池自保护：池所占页在所属 BCB 叶子置占用（防自分配）。
        //     池落在被丢弃的 crumbs 区时无 BCB 覆盖，天然安全。
        const uint64_t pool_pages = region_size_ >> 12;
        for (uint64_t p = 0; p < pool_pages; ++p) {
            const phyaddr_t a  = region_pbase_ + (p << 12);
            const int64_t   bi = bcb_for_addr(a);
            if (bi < 0) continue;
            const uint64_t off = (a - bcbs_[bi].base) >> 12;
            if (off >= (1ull << bcbs_[bi].max_order)) continue;
            leaf_set(static_cast<uint64_t>(bi), off, false);
            --bcbs_[bi].free_leaves;
        }
    }
    return 0;
}

// ================================================================
// mark_used — 排他性标记：区间覆盖的叶子全部置占用
// ================================================================
loc_code_t init_bcb_juvenile::mark_used(phyaddr_t base, uint64_t byte_size)
{
    if (!bcbs_ || bcb_count_ == 0 || region_pbase_ == 0 || byte_size == 0) return SRC_LOC();

    // 页对齐展开（base 向下、end 向上），容忍非页对齐输入
    const phyaddr_t lo = base & ~(PAGE_SIZE - 1);
    const phyaddr_t hi = (base + byte_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (hi <= lo) return 0;

    int64_t bi = bcb_for_addr(lo);
    if (bi < 0) bi = 0;
    for (; (uint64_t)bi < bcb_count_; ++bi) {
        bcb_state& b = bcbs_[bi];
        const uint64_t span     = 1ull << (b.max_order + 12);
        const phyaddr_t bcb_end = b.base + span;
        if (lo >= bcb_end) continue;
        if (hi <= b.base) break;                       // base 升序，越过即停

        const phyaddr_t seg_lo = (lo > b.base) ? lo : b.base;
        const phyaddr_t seg_hi = (hi < bcb_end) ? hi : bcb_end;
        const uint64_t  off_lo = (seg_lo - b.base) >> 12;
        const uint64_t  off_hi = (seg_hi - b.base) >> 12;
        for (uint64_t o = off_lo; o < off_hi; ++o) {
            if (leaf_test(static_cast<uint64_t>(bi), o)) {
                leaf_set(static_cast<uint64_t>(bi), o, false);
                --b.free_leaves;
            }
        }
    }
    return 0;
}

// ================================================================
// 叶子位图访问（布局常量来自 abi/bcb_handoff.h，位偏移 = 2^(N+1) + off）
// ================================================================
bool init_bcb_juvenile::leaf_test(uint64_t bcb_idx, uint64_t off)
{
    const uint64_t bit = bcb_leaf_bit_offset(bcbs_[bcb_idx].max_order) + off;
    const uint64_t* bm = reinterpret_cast<const uint64_t*>(bcbs_[bcb_idx].region_va);
    return (bm[bit >> 6] >> (bit & 63)) & 1;
}

void init_bcb_juvenile::leaf_set(uint64_t bcb_idx, uint64_t off, bool free)
{
    const uint64_t bit = bcb_leaf_bit_offset(bcbs_[bcb_idx].max_order) + off;
    uint64_t* bm = reinterpret_cast<uint64_t*>(bcbs_[bcb_idx].region_va);
    if (free) bm[bit >> 6] |=  (1ull << (bit & 63));
    else      bm[bit >> 6] &= ~(1ull << (bit & 63));
}

// ================================================================
// bcb_for_addr — 二分找 base <= addr 的最后一个 BCB；无则 -1
// ================================================================
int64_t init_bcb_juvenile::bcb_for_addr(phyaddr_t addr)
{
    if (!bcbs_ || bcb_count_ == 0) return -1;
    uint64_t lo = 0, hi = bcb_count_;
    while (lo < hi) {
        const uint64_t mid = lo + ((hi - lo) >> 1);
        if (bcbs_[mid].base <= addr) lo = mid + 1;
        else                         hi = mid;
    }
    return (lo == 0) ? -1 : static_cast<int64_t>(lo - 1);
}

// ================================================================
// scan_aligned_run — 跨 BCB 自下而上 first-fit，物理对齐
// ================================================================
phyaddr_t init_bcb_juvenile::scan_aligned_run(uint64_t count, uint8_t align_log2, loc_code_t* err)
{
    if (count == 0 || !bcbs_ || bcb_count_ == 0 || region_pbase_ == 0) {
        if (err) *err = SRC_LOC();
        return 0;
    }
    if (align_log2 < 12) align_log2 = 12;
    const uint64_t align_mask = (1ull << align_log2) - 1;
    const uint64_t step       = 1ull << (align_log2 - 12);   // 叶子偏移必须为 step 的倍数

    for (uint64_t w = 0; w < bcb_count_; ++w) {
        const uint64_t idx = (scan_hint_bcb_ + w) % bcb_count_;
        bcb_state& b = bcbs_[idx];

        // 物理对齐：本 BCB 基址不满足时整体跳过
        if (b.base & align_mask) continue;

        const uint64_t leaf_cnt = 1ull << b.max_order;
        uint64_t run = 0, run_start = 0;
        for (uint64_t o = 0; o < leaf_cnt; ++o) {
            if (run == 0 && (o % step) != 0) continue;       // run 起点必须对齐
            if (leaf_test(idx, o)) {
                if (run == 0) run_start = o;
                ++run;
                if (run == count) {
                    for (uint64_t k = 0; k < count; ++k) leaf_set(idx, run_start + k, false);
                    b.free_leaves  -= count;
                    b.scan_hint_off_ = run_start + count;
                    scan_hint_bcb_   = idx;
                    if (err) *err = 0;
                    return b.base + (run_start << 12);
                }
            } else {
                run = 0;
            }
        }
    }
    if (err) *err = SRC_LOC();
    return 0;
}

// ================================================================
// 公开接口
// ================================================================
phyaddr_t init_bcb_juvenile::alloc(uint64_t count, uint8_t align_log2, loc_code_t* err)
{
    return scan_aligned_run(count, align_log2, err);
}

loc_code_t init_bcb_juvenile::free(phyaddr_t base, uint64_t count)
{
    if (count == 0 || !bcbs_ || bcb_count_ == 0 || region_pbase_ == 0) return SRC_LOC();
    const int64_t bi = bcb_for_addr(base);
    if (bi < 0) return SRC_LOC();
    bcb_state& b = bcbs_[bi];
    const uint64_t span = 1ull << (b.max_order + 12);
    if (base < b.base || base + (count << 12) > b.base + span) return SRC_LOC();

    const uint64_t off0 = (base - b.base) >> 12;
    for (uint64_t i = 0; i < count; ++i)
        if (leaf_test(static_cast<uint64_t>(bi), off0 + i)) return SRC_LOC();  // 重复释放拒绝
    for (uint64_t i = 0; i < count; ++i)
        leaf_set(static_cast<uint64_t>(bi), off0 + i, true);
    b.free_leaves += count;
    return 0;
}

uint64_t init_bcb_juvenile::free_page_count()
{
    uint64_t s = 0;
    for (uint64_t i = 0; i < bcb_count_; ++i) s += bcbs_[i].free_leaves;
    return s;
}

uint64_t init_bcb_juvenile::total_page_count()
{
    uint64_t s = 0;
    for (uint64_t i = 0; i < bcb_count_; ++i) s += 1ull << bcbs_[i].max_order;
    return s;
}

const bcb_desc_v2_t* init_bcb_juvenile::get_descs() { return descs_; }
uint64_t          init_bcb_juvenile::get_desc_count() { return bcb_count_; }

phyaddr_t init_bcb_juvenile::get_region_pbase() { return region_pbase_; }
uint64_t  init_bcb_juvenile::get_region_size()  { return region_size_; }
