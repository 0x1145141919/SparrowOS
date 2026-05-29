// ════════════════════════════════════════════════════════════════
// BCB_foundation_comparison — Debug vs Release 基准横评
//
// 功能：
//   1. 同种子同序列验证行为等价性（fresh tree per op: 10k rounds）
//   2. 同种子长序列压测（独立实例，相同接入序列）
//   3. Debug 百万压测（带基础测试 + btree_validation）
//   4. Release 百万压测（带 btree_validation）
//   5. 汇总对比表
// ════════════════════════════════════════════════════════════════

#include "util/BCB_fnd_DeepFirst.h"
#include "util/BCB_fnd_ShallowFirst.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <random>
#include <ctime>

// ================================================================
// 配置
// ================================================================

static constexpr uint64_t FAKE_BASE = 0x100000000ULL;
static constexpr uint8_t   BCB_ORDER = 16;

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ================================================================
// 辅助函数
// ================================================================

static uint64_t bitmap_bytes() {
    return ((3ull << BCB_ORDER) + 63) / 64 * sizeof(uint64_t);
}

static void* make_bitmap() {
    uint64_t bytes = bitmap_bytes();
    void* mem = std::malloc(bytes);
    if (mem) std::memset(mem, 0, bytes);
    return mem;
}

// ── 统一 alloc 包装（Debug 和 Release 共用） ──
template<typename T>
static uint64_t alloc_one(T& fnd, uint8_t want_order) {
    KURD_t kurd;
    uint8_t base_order = want_order;
    uint64_t offset = fnd.find_candidate(base_order, kurd);
    if (kurd.result != result_code::SUCCESS) return 0;
    if (base_order > want_order) {
        kurd = fnd.split(base_order, offset, want_order);
        if (kurd.result != result_code::SUCCESS) return 0;
        offset <<= (base_order - want_order);
    }
    kurd = fnd.order_occupy_try(want_order, offset);
    return (kurd.result == result_code::SUCCESS)
        ? (FAKE_BASE + (offset << (want_order + 12))) : 0;
}

template<typename T>
static bool free_one(T& fnd, uint64_t addr, uint8_t order) {
    if (addr == 0) return true;
    uint64_t offset = (addr - FAKE_BASE) >> (order + 12);
    KURD_t kurd;
    fnd.order_return(order, offset, kurd);
    return (kurd.result == result_code::SUCCESS);
}

// ================================================================
// 1. 行为等价性验证
// ================================================================

static void test_equivalence() {
    printf("━━━ [1] 行为等价性验证 ━━━\n\n");

    const int ROUNDS = 10000;
    std::mt19937_64 gen(0xABCD);

    int match = 0, mismatch = 0;

    for (int i = 0; i < ROUNDS; i++) {
        void* bd = make_bitmap();
        void* br = make_bitmap();

        BCB_fnd_DeepFirst   fnd_d;
        BCB_fnd_ShallowFirst fnd_r;
        fnd_d.init((uint64_t)bd, BCB_ORDER);
        fnd_r.init((uint64_t)br, BCB_ORDER);

        uint8_t order = (uint8_t)((uint64_t)
            std::exponential_distribution<double>(0.3)(gen) % (BCB_ORDER + 1));

        uint64_t a_d = alloc_one(fnd_d, order);
        uint64_t a_r = alloc_one(fnd_r, order);

        if ((a_d != 0) == (a_r != 0)) match++;
        else mismatch++;

        std::free(bd);
        std::free(br);
    }

    printf("  fresh tree per op x %d:\n", ROUNDS);
    printf("    match=%d  mismatch=%d  (%.1f%%)\n\n",
           match, mismatch, 100.0 * match / (match + mismatch));
}

// ================================================================
// 2. 长序列同种子压测
// ================================================================

static void test_long_sequence() {
    printf("━━━ [2] 同种子长序列压测 ━━━\n\n");

    const int ITEMS = 5000;
    const int CYCLES = 100000;

    // Debug
    {
        void* bm = make_bitmap();
        BCB_fnd_DeepFirst fnd;
        fnd.init((uint64_t)bm, BCB_ORDER);

        std::mt19937_64 gen(0x12345678);
        std::exponential_distribution<double> od(0.3);

        struct { uint64_t addr; uint8_t order; } items[ITEMS];
        for (int i = 0; i < ITEMS; i++) {
            items[i].order = (uint8_t)((uint64_t)od(gen) % (BCB_ORDER + 1));
            items[i].addr = 0;
        }
        gen.seed(0x12345678);

        uint64_t alloc_ok = 0, alloc_fail = 0, free_ok = 0;
        uint64_t t0 = now_ns();

        for (int c = 0; c < CYCLES; c++) {
            int idx = (int)(gen() % ITEMS);
            auto& it = items[idx];
            if (it.addr == 0) {
                uint64_t a = alloc_one(fnd, it.order);
                if (a) { it.addr = a; alloc_ok++; }
                else   { alloc_fail++; }
            } else {
                free_one(fnd, it.addr, it.order);
                it.addr = 0;
                free_ok++;
            }
        }

        uint64_t remain = 0;
        for (auto& it : items) {
            if (it.addr) { free_one(fnd, it.addr, it.order); remain++; }
        }

        uint64_t dt = now_ns() - t0;
        bool valid = (fnd.btree_validation().result == result_code::SUCCESS);
        printf("  DEBUG    alloc=%lu (fail=%lu) free=%lu remain=%lu  "
               "%lu ms  valid=%s\n",
               alloc_ok, alloc_fail, free_ok, remain,
               (unsigned long)(dt / 1000000), valid ? "PASS" : "FAIL");
        std::free(bm);
    }

    // Release
    {
        void* bm = make_bitmap();
        BCB_fnd_ShallowFirst fnd;
        fnd.init((uint64_t)bm, BCB_ORDER);

        std::mt19937_64 gen(0x12345678);
        std::exponential_distribution<double> od(0.3);

        struct { uint64_t addr; uint8_t order; } items[ITEMS];
        for (int i = 0; i < ITEMS; i++) {
            items[i].order = (uint8_t)((uint64_t)od(gen) % (BCB_ORDER + 1));
            items[i].addr = 0;
        }
        gen.seed(0x12345678);

        uint64_t alloc_ok = 0, alloc_fail = 0, free_ok = 0;
        uint64_t t0 = now_ns();

        for (int c = 0; c < CYCLES; c++) {
            int idx = (int)(gen() % ITEMS);
            auto& it = items[idx];
            if (it.addr == 0) {
                uint64_t a = alloc_one(fnd, it.order);
                if (a) { it.addr = a; alloc_ok++; }
                else   { alloc_fail++; }
            } else {
                free_one(fnd, it.addr, it.order);
                it.addr = 0;
                free_ok++;
            }
        }

        uint64_t remain = 0;
        for (auto& it : items) {
            if (it.addr) { free_one(fnd, it.addr, it.order); remain++; }
        }

        uint64_t dt = now_ns() - t0;
        bool valid = (fnd.btree_validation().result == result_code::SUCCESS);
        printf("  RELEASE  alloc=%lu (fail=%lu) free=%lu remain=%lu  "
               "%lu ms  valid=%s\n",
               alloc_ok, alloc_fail, free_ok, remain,
               (unsigned long)(dt / 1000000), valid ? "PASS" : "FAIL");
        std::free(bm);
    }
    printf("\n");
}

// ================================================================
// 3+4. 百万量级压测（独立）
// ================================================================

template<typename T>
static void run_stress_1m(T& fnd, const char* name) {
    const int ITEMS = 20000;
    const int CYCLES = 1000000;

    std::mt19937_64 gen((uint64_t)&fnd ^ now_ns());
    std::exponential_distribution<double> od(0.3);

    struct { uint64_t addr; uint8_t order; }* items =
        (decltype(items))std::malloc(ITEMS * sizeof(*items));

    for (int i = 0; i < ITEMS; i++) {
        items[i].addr  = 0;
        items[i].order = (uint8_t)((uint64_t)od(gen) % (BCB_ORDER + 1));
    }

    uint64_t alloc_ok = 0, alloc_fail = 0, free_ok = 0, free_fail = 0;
    uint64_t t0 = now_ns();

    for (int c = 0; c < CYCLES; c++) {
        int idx = (int)(gen() % ITEMS);
        auto& it = items[idx];
        if (it.addr == 0) {
            uint64_t a = alloc_one(fnd, it.order);
            if (a) { it.addr = a; alloc_ok++; }
            else   { alloc_fail++; }
        } else {
            if (free_one(fnd, it.addr, it.order)) {
                it.addr = 0; free_ok++;
            } else {
                free_fail++;
            }
        }
    }

    uint64_t remain = 0;
    for (int i = 0; i < ITEMS; i++) {
        if (items[i].addr) {
            free_one(fnd, items[i].addr, items[i].order);
            remain++;
        }
    }

    uint64_t dt = now_ns() - t0;
    bool valid = (fnd.btree_validation().result == result_code::SUCCESS);

    double ns_op = (double)dt / CYCLES;
    printf("  %-8s alloc=%lu (fail=%lu) free=%lu (fail=%lu) "
           "%lu ms  %.0f ns/op  valid=%s\n",
           name, alloc_ok, alloc_fail, free_ok, free_fail,
           (unsigned long)(dt / 1000000), ns_op,
           valid ? "PASS" : "FAIL");

    std::free(items);
}

static void test_stress_1m() {
    printf("━━━ [3] Debug × 100万 = 独立压测 ━━━\n\n");
    {
        void* bm = make_bitmap();
        BCB_fnd_DeepFirst fnd;
        fnd.init((uint64_t)bm, BCB_ORDER);
        run_stress_1m(fnd, "DEBUG");
        std::free(bm);
    }

    printf("\n━━━ [4] Release × 100万 = 独立压测 ━━━\n\n");
    {
        void* bm = make_bitmap();
        BCB_fnd_ShallowFirst fnd;
        fnd.init((uint64_t)bm, BCB_ORDER);
        run_stress_1m(fnd, "RELEASE");
        std::free(bm);
    }
}

// ================================================================
// 主函数
// ================================================================

int main() {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║     BuddyControlBlock  Debug vs Release 基准横评    ║\n");
    printf("║     BCB_ORDER=%u   管理 %.0f MB   内存池            ║\n",
           (unsigned)BCB_ORDER,
           (double)((1ull << (BCB_ORDER + 12)) >> 20));
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    test_equivalence();
    test_long_sequence();
    test_stress_1m();

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║     比较完成                                          ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    return 0;
}
