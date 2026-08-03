// ════════════════════════════════════════════════════════════════
// init_bcb_juvenile_test — plan_and_setup 切分效果测试
//
// 以 test_set/*.json（EFI 内存视图）为输入，验证：
//   1. 分桶（切分效果）：BCB 数量、order 分布、span 汇总
//   2. 不变量：descs 按 base 升序、互不重叠、全部落在 freeSystemRam 段内
//   3. 位图预算：region_size_ = 3bit × 总空闲页
//   4. full 模式：铺全空闲叶子 + free_page_count 守恒
//   5. 池自保护：池页在所属 BCB 叶子置占用，alloc 自动跳过
//
// 编译: cd src/tests && make -f Makefile init_bcb_juvenile_test
// 运行: ./init_bcb_juvenile_test [test_set 目录]
// ════════════════════════════════════════════════════════════════

#include "init/init_bcb_juvenile.h"
#include "uutils/json_prasers.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL [L%d]: %s\n", __LINE__, msg); g_fail++; } \
} while(0)

static const char* kCases[] = { "mem.json", "phy_instance_1.json", "phy_instance_2.json" };

// ================================================================
// 工具：统计 freeSystemRam 段
// ================================================================
static uint64_t count_free_pages(const phymem_segment* segs, uint64_t n) {
    uint64_t pages = 0;
    for (uint64_t i = 0; i < n; ++i)
        if (segs[i].type == freeSystemRam) pages += segs[i].size >> 12;
    return pages;
}

static const phymem_segment* find_free_seg(const phymem_segment* segs, uint64_t n,
                                           phyaddr_t base) {
    for (uint64_t i = 0; i < n; ++i) {
        if (segs[i].type != freeSystemRam) continue;
        if (base >= segs[i].start && base < segs[i].start + segs[i].size) return &segs[i];
    }
    return nullptr;
}

// ================================================================
// 打印切分效果 + 校验不变量
// ================================================================
static void dump_and_validate(const phymem_segment* segs, uint64_t seg_cnt,
                              uint64_t total_free_pages, const char* tag) {
    const bcb_desc_t* d = init_bcb_juvenile::get_descs();
    const uint64_t n    = init_bcb_juvenile::get_desc_count();
    const uint64_t mg   = init_bcb_juvenile::total_page_count();

    printf("  [%s] BCB=%lu managed=%lu/%lu pages  pool_need=0x%lx (%lu KB)\n",
           tag, (unsigned long)n, (unsigned long)mg, (unsigned long)total_free_pages,
           (unsigned long)init_bcb_juvenile::region_size_, (unsigned long)(init_bcb_juvenile::region_size_ >> 10));

    // order 分布（切分效果）
    uint8_t order_hist[64] = {0};
    uint64_t span_sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        if (d[i].order < 64) order_hist[d[i].order]++;
        span_sum += 1ull << (d[i].order + 12);
    }
    for (int o = 0; o < 64; ++o)
        if (order_hist[o])
            printf("     order %2d: %3u BCB  (span %lu MB/BCB)\n", o, order_hist[o],
                   (unsigned long)((1ull << (o + 12)) >> 20));
    printf("     total span=%lu MB\n", (unsigned long)(span_sum >> 20));

    // ---- 不变量 ----
    CHECK(n > 0, "desc count must be > 0");
    CHECK(mg <= total_free_pages, "managed pages must not exceed total free");
    CHECK(init_bcb_juvenile::free_page_count() <= mg, "free_page_count must not exceed managed");
    if (n == 0) return;

    // 升序 + 不重叠 + 落在 free 段内
    uint64_t prev_end = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t span = 1ull << (d[i].order + 12);
        if (i > 0) CHECK(d[i].managed_base_phyaddr_base >= prev_end, "descs must not overlap");
        const phymem_segment* s = find_free_seg(segs, seg_cnt, d[i].managed_base_phyaddr_base);
        CHECK(s != nullptr, "desc base must lie in a freeSystemRam segment");
        if (s) CHECK(d[i].managed_base_phyaddr_base + span <= s->start + s->size,
                     "desc span must fit inside its free segment");
        prev_end = d[i].managed_base_phyaddr_base + span;
    }
    // 位图预算：3bit × 总空闲页（现行算法）
    uint64_t expect_pool = ((total_free_pages * 3 + 7) >> 3);
    expect_pool = (expect_pool + 4095) & ~4095ull;
    CHECK(init_bcb_juvenile::region_size_ == expect_pool, "pool budget must equal 3bit x total free pages");
}

// ================================================================
// 用例：单 JSON × 双策略
// ================================================================
static void run_case(const char* dir, const char* name, uint64_t cpu) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    uint64_t seg_cnt = 0;
    phymem_segment* segs = load_and_parse_memory_map(path, seg_cnt);
    if (!segs) {
        fprintf(stderr, "  skip %s (cannot parse)\n", path);
        return;
    }
    const uint64_t free_pages = count_free_pages(segs, seg_cnt);
    printf("== %s: segs=%lu free_pages=%lu\n", name, (unsigned long)seg_cnt,
           (unsigned long)free_pages);

    // BEST_ALIGN_FIT（plan-only）
    {
        bcb_juvenile_init_config cfg = bcb_juvenile_init_config::BEST_FIT();
        cfg.segs = segs; cfg.segs_count = seg_cnt; cfg.logical_processor_count = cpu;
        CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "BEST_FIT plan_and_setup failed");
        dump_and_validate(segs, seg_cnt, free_pages, "BEST_FIT");
    }
    // MATCH_THREAD（plan-only）
    {
        bcb_juvenile_init_config cfg = bcb_juvenile_init_config::DEFAULT_THREAD();
        cfg.segs = segs; cfg.segs_count = seg_cnt; cfg.logical_processor_count = cpu;
        CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "MATCH_THREAD plan_and_setup failed");
        dump_and_validate(segs, seg_cnt, free_pages, "MATCH_THREAD");
    }
    delete[] segs;
}

// ================================================================
// full 模式：malloc 池 → 铺叶子 → alloc/free 端到端
// ================================================================
static void run_full_mode(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    uint64_t seg_cnt = 0;
    phymem_segment* segs = load_and_parse_memory_map(path, seg_cnt);
    if (!segs) return;

    bcb_juvenile_init_config cfg = bcb_juvenile_init_config::BEST_FIT();
    cfg.segs = segs; cfg.segs_count = seg_cnt; cfg.logical_processor_count = 4;

    // 阶段 A：plan-only 求预算（纯静态类共享状态，先显式清零池指针）
    init_bcb_juvenile::region_pbase_ = 0;
    CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "full: plan-only failed");
    const uint64_t need = init_bcb_juvenile::region_size_;
    const uint64_t mg   = init_bcb_juvenile::total_page_count();
    void* pool = std::malloc(need);
    CHECK(pool != nullptr, "full: pool malloc failed");
    if (!pool) { delete[] segs; return; }
    std::memset(pool, 0xAA, need);   // 预置毒值，验证铺叶子确实清零覆盖

    // 阶段 B：full 重建（池为宿主地址，不在视图内 → 无自保护翻转）
    init_bcb_juvenile::region_pbase_ = (phyaddr_t)(uintptr_t)pool;
    CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "full: plan_and_setup failed");
    CHECK(init_bcb_juvenile::total_page_count() == mg, "full: managed pages stable across phases");
    CHECK(init_bcb_juvenile::free_page_count() == mg, "full: all leaves must be free (pool outside view)");

    // 叶子全 1：直接读池内位图
    {
        const bcb_desc_t* d = init_bcb_juvenile::get_descs();
        for (uint64_t i = 0; i < init_bcb_juvenile::get_desc_count(); ++i) {
            const uint64_t* bm = (const uint64_t*)(uintptr_t)d[i].bitmap_region_base_pa;
            const uint64_t leaf_cnt = 1ull << d[i].order;
            uint64_t bad = 0;
            for (uint64_t o = 0; o < leaf_cnt; ++o) {
                const uint64_t bit = bcb_leaf_bit_offset(d[i].order) + o;
                if (!((bm[bit >> 6] >> (bit & 63)) & 1)) { bad++; }
            }
            CHECK(bad == 0, "full: a leaf is not free after setup");
        }
    }

    // alloc/free 端到端（首个 BCB 的第 0 叶 → base）
    if (init_bcb_juvenile::get_desc_count() > 0) {
        loc_code_t err = 0;
        phyaddr_t a = init_bcb_juvenile::alloc(1, 12, &err);
        CHECK(err == 0 && a != 0, "full: alloc(1) failed");
        CHECK(init_bcb_juvenile::free_page_count() == mg - 1, "full: alloc must consume 1 leaf");
        CHECK(init_bcb_juvenile::free(a, 1) == 0, "full: free failed");
        CHECK(init_bcb_juvenile::free_page_count() == mg, "full: free must restore leaf");
        CHECK(init_bcb_juvenile::free(a, 1) != 0, "full: double-free must be rejected");
    }
    std::free(pool);
    delete[] segs;
    printf("== %s: full-mode OK (managed=%lu)\n", name, (unsigned long)mg);
}

// ================================================================
// 池自保护：池落在视图内时，池页叶子置占用，alloc 跳过
// ================================================================
static void run_pool_self_protection() {
    printf("== pool self-protection (synthetic view)\n");
    void* raw = nullptr;
    if (posix_memalign(&raw, 4u << 20, (16ull << 20) + (4ull << 20)) != 0) {
        CHECK(false, "self-protection: memalign failed");
        return;
    }
    const phyaddr_t pool_base = (phyaddr_t)(uintptr_t)raw;   // 4MB 对齐
    phymem_segment seg;
    seg.start = pool_base;                  // 池起点即视图段起点
    seg.size  = 16ull << 20;                // 16MB free 段
    seg.type  = freeSystemRam;

    bcb_juvenile_init_config cfg = bcb_juvenile_init_config::BEST_FIT();
    cfg.segs = &seg; cfg.segs_count = 1; cfg.logical_processor_count = 4;
    init_bcb_juvenile::region_pbase_ = pool_base;
    CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "self-protection: plan_and_setup failed");

    // 16MB = 4096 页，4MB 对齐段 → 4 个 order-10 BCB
    CHECK(init_bcb_juvenile::total_page_count() == 4096, "self-protection: managed != 4096");
    // 池预算 = align_up(3*4096/8, 4096) = 4096 字节 = 1 页，落在 BCB0 的 offset 0
    CHECK(init_bcb_juvenile::free_page_count() == 4095, "self-protection: pool page must be occupied");

    loc_code_t err = 0;
    phyaddr_t a = init_bcb_juvenile::alloc(1, 12, &err);
    CHECK(err == 0 && a == pool_base + 4096, "self-protection: alloc must skip pool page");
    CHECK(init_bcb_juvenile::free(a, 1) == 0, "self-protection: free failed");
    free(raw);
    printf("  OK\n");
}

// ================================================================
// alloc/free 对齐测试：合成视图强制多种对齐路径
//
// 视图 [0x100000, 0x1C00000) 27MB → 分桶出 4 个 BCB（全部 ≥4MB 对齐）：
//   {0x400000, o10}  4MB     base 4MB 对齐
//   {0x800000, o11}  8MB     base 8MB 对齐
//   {0x1000000, o11} 16MB    base 16MB 对齐（唯一 16MB 槽位）
//   {0x1800000, o10} 24MB    base 8MB 对齐（余量只够 4MB 块）
// 推论：align_log2≤22 恒可满足；23 跳过 BCB0；24 只有 0x1000000；
//       25 无 BCB 满足 → 必失败。
// ================================================================
static void run_alignment_test() {
    printf("== alloc/free alignment test (synthetic view)\n");
    const phyaddr_t VB = 0x100000ull;
    const uint64_t  VS = 27ull << 20;
    phymem_segment seg = { VB, VS, freeSystemRam };
    bcb_juvenile_init_config cfg = bcb_juvenile_init_config::BEST_FIT();
    cfg.segs = &seg; cfg.segs_count = 1; cfg.logical_processor_count = 4;

    // 阶段 A：plan-only 求预算（纯静态类共享状态，先显式清零池指针）
    init_bcb_juvenile::region_pbase_ = 0;
    CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "align: plan-only failed");
    const uint64_t mg = init_bcb_juvenile::total_page_count();
    void* pool = std::malloc(init_bcb_juvenile::region_size_);
    CHECK(pool != nullptr, "align: pool malloc failed");
    if (!pool) return;
    init_bcb_juvenile::region_pbase_ = (phyaddr_t)(uintptr_t)pool;
    CHECK(init_bcb_juvenile::plan_and_setup(&cfg) == 0, "align: full setup failed");
    CHECK(init_bcb_juvenile::free_page_count() == mg, "align: all leaves free before ops");
    printf("  view %lu MB -> BCB=%lu managed=%lu\n",
           (unsigned long)(VS >> 20), (unsigned long)init_bcb_juvenile::get_desc_count(), (unsigned long)mg);

    auto aligned_ok = [](phyaddr_t a, uint8_t al) -> bool {
        return a != 0 && (a & ((1ull << al) - 1)) == 0;
    };

    // ---- 1. 常规对齐 12..22：任意 BCB 基址均可满足 ----
    {
        const uint8_t als[] = { 12, 13, 14, 16, 21, 22 };
        for (uint8_t al : als) {
            loc_code_t err = 0;
            phyaddr_t a = init_bcb_juvenile::alloc(1, al, &err);
            CHECK(aligned_ok(a, al), "align: must succeed and be aligned");
            if (a) printf("  alloc(align=%2u) = 0x%lx\n", al, (unsigned long)a);
            CHECK(init_bcb_juvenile::free(a, 1) == 0, "align: free failed");
            CHECK(init_bcb_juvenile::free_page_count() == mg, "align: count restored");
        }
    }

    // ---- 2. 8MB：必须跳过 4MB 对齐的 BCB0，落在 0x800000 ----
    {
        loc_code_t err = 0;
        phyaddr_t a = init_bcb_juvenile::alloc(1, 23, &err);
        CHECK(a == 0x800000ull, "align23: first 8MB-aligned BCB is 0x800000 (skip BCB0)");
        CHECK(init_bcb_juvenile::free(a, 1) == 0, "align23: free failed");
    }

    // ---- 3. 16MB：唯一槽位 0x1000000；占→再占失败→还→可再占 ----
    {
        loc_code_t err = 0;
        phyaddr_t a = init_bcb_juvenile::alloc(1, 24, &err);
        CHECK(a == 0x1000000ull, "align24: only 16MB slot is 0x1000000");
        loc_code_t err2 = 0;
        phyaddr_t b = init_bcb_juvenile::alloc(1, 24, &err2);
        CHECK(b == 0, "align24: second 16MB alloc must fail (only one slot)");
        CHECK(init_bcb_juvenile::free(a, 1) == 0, "align24: free failed");
        phyaddr_t c = init_bcb_juvenile::alloc(1, 24, &err2);
        CHECK(c == 0x1000000ull, "align24: slot reusable after free");
        CHECK(init_bcb_juvenile::free(c, 1) == 0, "align24: free2 failed");
    }

    // ---- 4. 32MB：无 BCB 基址满足，必失败 ----
    {
        loc_code_t err = 0;
        phyaddr_t a = init_bcb_juvenile::alloc(1, 25, &err);
        CHECK(a == 0 && err != 0, "align25: no 32MB-aligned BCB, must fail");
        printf("  alloc(align=25) -> FAIL as expected\n");
    }

    // ---- 5. 多页 + 对齐：alloc(64, 21) = 64 连续页且 2MB 对齐 ----
    {
        loc_code_t err = 0;
        phyaddr_t a = init_bcb_juvenile::alloc(64, 21, &err);
        CHECK(aligned_ok(a, 21), "align21x64: 2MB aligned");
        CHECK(init_bcb_juvenile::free_page_count() == mg - 64, "align21x64: consumed 64 pages");
        CHECK(init_bcb_juvenile::free(a, 64) == 0, "align21x64: free failed");
        CHECK(init_bcb_juvenile::free_page_count() == mg, "align21x64: restored");
        if (a) printf("  alloc(64, align=21) = 0x%lx\n", (unsigned long)a);
    }

    // ---- 6. 耗尽：单页 first-fit 填满 mg 页，再分配必失败，归还全恢复 ----
    {
        std::vector<phyaddr_t> addrs;
        bool full = false;
        while (addrs.size() <= mg) {
            loc_code_t err = 0;
            phyaddr_t a = init_bcb_juvenile::alloc(1, 12, &err);
            if (a == 0) { full = true; break; }
            addrs.push_back(a);
        }
        CHECK(full, "exhaust: alloc must eventually fail when full");
        CHECK(addrs.size() == mg, "exhaust: exactly mg pages consumed");
        CHECK(init_bcb_juvenile::free_page_count() == 0, "exhaust: no free pages left");
        for (size_t i = 0; i < addrs.size(); ++i)
            CHECK((addrs[i] & 0xFFF) == 0, "exhaust: all page aligned");
        for (size_t i = 0; i < addrs.size(); ++i)
            CHECK(init_bcb_juvenile::free(addrs[i], 1) == 0, "exhaust: free failed");
        CHECK(init_bcb_juvenile::free_page_count() == mg, "exhaust: fully restored");
        printf("  exhaust: %lu pages alloc/free round-trip OK\n", (unsigned long)addrs.size());
    }

    std::free(pool);
    printf("  OK\n");
}

// ================================================================
// main
// ================================================================
int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : "test_set";
    // 兜底：从 src/tests/ 运行时，仓库根是 ../../
    if (argc <= 1) {
        char probe[512];
        snprintf(probe, sizeof(probe), "%s/%s", dir, kCases[0]);
        FILE* f = fopen(probe, "rb");
        if (f) { fclose(f); }
        else { dir = "../../test_set"; }
    }
    const uint64_t cpus[] = { 4, 8 };

    printf("=== init_bcb_juvenile plan_and_setup test ===\n");
    for (size_t c = 0; c < sizeof(kCases) / sizeof(kCases[0]); ++c) {
        for (size_t k = 0; k < sizeof(cpus) / sizeof(cpus[0]); ++k) {
            printf("--- cpu=%lu ---\n", (unsigned long)cpus[k]);
            run_case(dir, kCases[c], cpus[k]);
        }
    }

    printf("--- full mode (pool) ---\n");
    run_full_mode(dir, kCases[0]);

    printf("--- self protection ---\n");
    run_pool_self_protection();

    printf("--- alloc/free alignment ---\n");
    run_alignment_test();

    printf("\n=== Complete: %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
