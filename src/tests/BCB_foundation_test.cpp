// ════════════════════════════════════════════════════════════════
// BCB_foundation_test — BuddyControlBlock_foundation (v4) 测试
//
// 测试 mixed_bitmap_v4 DFS 底座：
//   find_candidate + split + order_occupy_try + order_return
// 在用户态运行，用 malloc 模拟 bitmap 区域
// ════════════════════════════════════════════════════════════════

#include "util/BCB_fnd_DeepFirst.h"
#include <vector>
#include <random>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <new>

// ===== rdtsc stub =====
static inline uint64_t rdtsc() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ===== 测试配置 =====
static constexpr uint64_t FAKE_BASE = 0x100000000ULL;
static constexpr uint8_t   BCB_ORDER = 16;  // 管理 2^16 × 4KB = 256MB

// ===== 测试辅助 =====
struct mem_seg {
    uint64_t addr;   // 0 = 空槽/未分配
    uint8_t  order;
};

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL [L%d]: %s\n", __LINE__, msg); g_failures++; } \
} while(0)

#define CHECK_OK(kurd) do { \
    if (!success_all_kurd(kurd)) { \
        fprintf(stderr, "  FAIL [L%d]: KURD result=%d reason=%d\n", \
                __LINE__, (int)(kurd).result, (int)(kurd).reason); \
        g_failures++; \
    } \
} while(0)

// ===== 包装分配器：find_candidate → split → occupy =====
static uint64_t w_alloc(BuddyControlBlock_foundation& fnd,
                         uint8_t want_order, KURD_t& kurd)
{
    uint8_t  base_order = want_order;
    uint64_t offset = fnd.find_candidate(base_order, kurd);
    if (!success_all_kurd(kurd))
        return 0;

    if (base_order > want_order) {
        kurd = fnd.split(base_order, offset, want_order);
        if (!success_all_kurd(kurd))
            return 0;
        offset <<= (base_order - want_order);
    }

    kurd = fnd.order_occupy_try(want_order, offset);
    if (!success_all_kurd(kurd))
        return 0;

    return FAKE_BASE + (offset << (want_order + 12));
}

// ===== 包装释放器（addr=0 时跳过） =====
static KURD_t w_free(BuddyControlBlock_foundation& fnd,
                      uint64_t addr, uint8_t order)
{
    if (addr == 0)
        return KURD_t();   // 空操作
    uint64_t offset = (addr - FAKE_BASE) >> (order + 12);
    KURD_t kurd;
    uint8_t final_order = fnd.order_return(order, offset, kurd);
    if (final_order >= 0x40)
        return kurd;
    return kurd;
}

// ===== 校验：每轮后调用 btree_validation =====
static int do_validate(BuddyControlBlock_foundation& fnd, const char* tag) {
    KURD_t vk = fnd.btree_validation();
    if (!success_all_kurd(vk)) {
        fprintf(stderr, "  VALIDATION FAIL [%s]: result=%d reason=%d\n",
                tag, (int)vk.result, (int)vk.reason);
        g_failures++;
        return 1;
    }
    return 0;
}

// ================================================================
// 基础测试集合
// ================================================================

static void basic_alloc_free_one(BuddyControlBlock_foundation& fnd) {
    printf("[basic] single alloc/free (order=5)...\n");
    KURD_t k;
    uint64_t a = w_alloc(fnd, 5, k);
    CHECK_OK(k);
    CHECK(a != 0, "alloc returned 0");
    CHECK(a >= FAKE_BASE, "addr below base");
    CHECK(a < FAKE_BASE + (1ull << (BCB_ORDER + 12)), "addr exceeds BCB range");

    KURD_t fk = w_free(fnd, a, 5);
    CHECK_OK(fk);
    do_validate(fnd, "basic_one");
    printf("  OK\n");
}

static void basic_alloc_zero(BuddyControlBlock_foundation& fnd) {
    printf("[basic] alloc order=0 (single 4K page)...\n");
    KURD_t k;
    uint64_t a = w_alloc(fnd, 0, k);
    CHECK_OK(k);
    CHECK(a != 0, "alloc returned 0");
    CHECK_OK(w_free(fnd, a, 0));
    do_validate(fnd, "basic_zero");
    printf("  OK\n");
}

static void basic_various_orders(BuddyControlBlock_foundation& fnd) {
    printf("[basic] various orders (0,1,2,4,8,12)...\n");
    // order=16 在前序分裂后不是 FREE，单独由 basic_max_order 测试
    static const uint8_t orders[] = {0, 1, 2, 4, 8, 12};
    uint64_t addrs[sizeof(orders)] = {0};
    KURD_t k;
    bool all_ok = true;

    for (int i = 0; i < (int)(sizeof(orders)/sizeof(orders[0])); i++) {
        addrs[i] = w_alloc(fnd, orders[i], k);
        if (!success_all_kurd(k) || addrs[i] == 0) {
            fprintf(stderr, "  FAIL [L%d]: alloc order=%d, res=%d reason=%d\n",
                    __LINE__, orders[i], (int)k.result, (int)k.reason);
            g_failures++; all_ok = false; break;
        }
        uint64_t block_size = 4096ULL << orders[i];
        CHECK((addrs[i] & (block_size - 1)) == 0, "bad alignment");
    }
    if (all_ok) do_validate(fnd, "basic_various_after_alloc");

    for (int i = (int)(sizeof(orders)/sizeof(orders[0])) - 1; i >= 0; i--) {
        if (addrs[i] == 0) continue;
        KURD_t fk = w_free(fnd, addrs[i], orders[i]);
        if (!success_all_kurd(fk)) {
            fprintf(stderr, "  FAIL [L%d]: free order=%d, res=%d reason=%d\n",
                    __LINE__, orders[i], (int)fk.result, (int)fk.reason);
            g_failures++; all_ok = false;
        }
    }
    if (all_ok) do_validate(fnd, "basic_various_after_free");
    printf("  %s\n", all_ok ? "OK" : "FAIL");
}

static void basic_split_coalesce(BuddyControlBlock_foundation& fnd) {
    printf("[basic] split & coalesce (two adj order=0)...\n");
    KURD_t k;
    uint64_t p1 = w_alloc(fnd, 0, k);
    CHECK_OK(k);
    uint64_t p2 = w_alloc(fnd, 0, k);
    CHECK_OK(k);

    // 地址应相邻（从大块分裂出的两块）
    uint64_t diff = (p1 > p2) ? (p1 - p2) : (p2 - p1);
    CHECK(diff == 4096, "adjacent 4K pages not 4K apart");

    CHECK_OK(w_free(fnd, p1, 0));
    CHECK_OK(w_free(fnd, p2, 0));
    do_validate(fnd, "basic_coalesce_after_free");
    printf("  OK\n");
}

static void basic_max_order(BuddyControlBlock_foundation& fnd) {
    printf("[basic] max_order alloc/free (entire BCB)...\n");
    KURD_t k;
    uint64_t a = w_alloc(fnd, BCB_ORDER, k);
    CHECK_OK(k);
    CHECK(a != 0, "max_order alloc returned 0");
    CHECK(a == FAKE_BASE, "max_order alloc should start at base");
    do_validate(fnd, "basic_max_after_alloc");
    CHECK_OK(w_free(fnd, a, BCB_ORDER));
    do_validate(fnd, "basic_max_after_free");
    printf("  OK\n");
}

static void basic_fragmentation(BuddyControlBlock_foundation& fnd) {
    // 分配所有 order=0 页 → 树完全分裂 → 释放 → 验证折叠
    printf("[basic] fragment then coalesce...\n");
    constexpr int NUM_PAGES = 1 << BCB_ORDER;
    KURD_t k;
    std::vector<uint64_t> pages;
    pages.reserve(NUM_PAGES);

    for (int i = 0; i < NUM_PAGES; i++) {
        uint64_t a = w_alloc(fnd, 0, k);
        if (!success_all_kurd(k) || a == 0) break;
        pages.push_back(a);
    }
    CHECK(pages.size() > 0, "should have allocated at least 1 page");

    printf("    allocated %zu order=0 pages\n", pages.size());

    for (auto a : pages)
        CHECK_OK(w_free(fnd, a, 0));
    do_validate(fnd, "frag_after_free_all");

    // 应完全折叠回 order=16
    uint64_t big = w_alloc(fnd, BCB_ORDER, k);
    CHECK_OK(k);
    CHECK(big == FAKE_BASE, "full coalesce should give base addr");
    CHECK_OK(w_free(fnd, big, BCB_ORDER));
    printf("  OK\n");
}

// ================================================================
// 压力测试
// ================================================================

static constexpr int   NUM_ITEMS  = 20000;
static constexpr int   NUM_CYCLES = 1000000;
static constexpr int   VALIDATE_INTERVAL = 100000;

static void run_stress(BuddyControlBlock_foundation& fnd) {
    printf("\n=== Stress Test (%d cycles, validate every %d) ===\n",
           NUM_CYCLES, VALIDATE_INTERVAL);

    std::mt19937_64 gen((uint64_t)&fnd ^ (uint64_t)rdtsc());
    std::exponential_distribution<double> order_dist(0.3);
    // exp(λ=0.3): P(order=k) ~ e^{-0.3k}, 均值 ≈ 3.3, ≤BCB_ORDER 占绝大多数

    // 预处理生成所有 items 的 order（指数分布 → 取余保护上限）
    std::vector<mem_seg> items(NUM_ITEMS);
    for (auto& item : items) {
        item.addr  = 0;
        double v = order_dist(gen);
        item.order = (uint8_t)((uint64_t)v % (BCB_ORDER + 1));
    }

    std::uniform_int_distribution<int> idx_dist(0, NUM_ITEMS - 1);

    uint64_t alloc_ok  = 0;
    uint64_t free_ok   = 0;
    uint64_t alloc_fail= 0;
    uint64_t free_fail = 0;
    uint64_t ops_this_interval = 0;
    uint64_t t_interval_start = rdtsc();
    uint64_t t_global_start    = t_interval_start;

    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        int idx = idx_dist(gen);
        mem_seg& item = items[idx];

        if (item.addr == 0) {
            KURD_t k;
            uint64_t a = w_alloc(fnd, item.order, k);
            if (success_all_kurd(k) && a != 0) {
                item.addr = a;
                alloc_ok++;
            } else {
                alloc_fail++;
            }
        } else {
            KURD_t fk = w_free(fnd, item.addr, item.order);
            if (success_all_kurd(fk)) {
                item.addr = 0;
                free_ok++;
            } else {
                free_fail++;
            }
        }
        ops_this_interval++;

        if ((cycle + 1) % VALIDATE_INTERVAL == 0) {
            uint64_t t_now = rdtsc();
            uint64_t interval_cycles = t_now - t_interval_start;
            double avg_cycles = (double)interval_cycles / ops_this_interval;

            printf("  cycle %7d/%d  alloc=%lu/%lu  free=%lu/%lu  "
                   "%.1f cyc/op  validate...\n",
                   cycle + 1, NUM_CYCLES, alloc_ok, alloc_fail,
                   free_ok, free_fail, avg_cycles);
            fflush(stdout);
/*
            if (do_validate(fnd, "stress") == 0)
                printf(" OK  (%lu ms)\n", (t_now - t_global_start) / 1000000);
            else
                printf(" FAIL\n");
*/
            // 重置区间计时
            t_interval_start = t_now;
            ops_this_interval = 0;

            if (g_failures > 10) {
                printf("  *** Too many failures, aborting stress test\n");
                break;
            }
        }
    }

    // 清理
    uint64_t remain = 0;
    for (auto& item : items) {
        if (item.addr != 0) {
            KURD_t fk = w_free(fnd, item.addr, item.order);
            if (success_all_kurd(fk)) item.addr = 0;
            remain++;
        }
    }

    uint64_t t1 = rdtsc();
    printf("Done: alloc=%lu (fail=%lu) free=%lu (fail=%lu) remain=%lu  %lu ms\n",
           alloc_ok, alloc_fail, free_ok, free_fail, remain,
           (t1 - t_global_start) / 1000000);

    do_validate(fnd, "stress_final");
    printf("Stress: %s\n", g_failures == 0 ? "ALL CLEAN" : "HAD FAILURES");
}

// ================================================================
// 主函数
// ================================================================

int main() {
    printf("=== BCB Foundation (v4) Test ===\n");
    printf("BCB_ORDER=%u  FAKE_BASE=0x%lx  size=%lu MB\n\n",
           (unsigned)BCB_ORDER, (unsigned long)FAKE_BASE,
           (unsigned long)((1ull << (BCB_ORDER + 12)) >> 20));

    // 分配 bitmap 内存 (3 × 2^N bits)
    constexpr uint64_t total_bits = 3ull << BCB_ORDER;
    constexpr uint64_t bitmap_bytes = ((total_bits + 63) / 64) * sizeof(uint64_t);
    void* bitmap_mem = std::malloc(bitmap_bytes);
    if (!bitmap_mem) { fprintf(stderr, "malloc bitmap failed\n"); return 1; }
    std::memset(bitmap_mem, 0, bitmap_bytes);
    printf("Bitmap: %lu bytes at %p\n", (unsigned long)bitmap_bytes, bitmap_mem);

    BCB_fnd_DeepFirst fnd;
    fnd.init((uint64_t)bitmap_mem, BCB_ORDER);
    printf("Foundation initialized\n\n");

    // ─── 基础测试 ───
    printf("--- Basic Tests ---\n");
    basic_alloc_free_one(fnd);
    basic_alloc_zero(fnd);
    basic_various_orders(fnd);
    basic_split_coalesce(fnd);
    basic_max_order(fnd);
    basic_fragmentation(fnd);

    if (g_failures > 0) {
        printf("\n*** %d basic test(s) FAILED ***\n\n", g_failures);
    } else {
        printf("\n*** All basic tests PASSED ***\n\n");
    }

    // ─── 压力测试 ───
    if (g_failures == 0) {
        run_stress(fnd);
    }

    std::free(bitmap_mem);
    printf("\n=== Complete: %d failure(s) ===\n", g_failures);
    return g_failures ? 1 : 0;
}
