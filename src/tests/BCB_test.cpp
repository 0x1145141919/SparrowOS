// ════════════════════════════════════════════════════════════════
// BCB_test — FreePagesAllocator::BuddyControlBlock 测试
//
// 测试 mixed_bitmap_v2 + buddy 算法 (allocate_buddy_way / free_buddy_way)
// 在用户态运行，用 malloc 模拟 BCB 的 bitmap 区域
// ════════════════════════════════════════════════════════════════

#include "memory/FreePagesAllocator.h"
#include "util/kout.h"
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
    uint32_t hi, lo;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

// ===== 测试辅助 =====
struct mem_seg {
    uint64_t addr;  // 1 = 无效（未分配）
    uint64_t size;
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

// ===== 页框校验 (全量扫描 BCB) =====
static int do_flush(FreePagesAllocator::BuddyControlBlock* bcb) {
    KURD_t fk = bcb->free_pages_flush();
    if (!success_all_kurd(fk)) {
        fprintf(stderr, "  FAIL: free_pages_flush result=%d reason=%d\n",
                (int)fk.result, (int)fk.reason);
        g_failures++;
        return 1;
    }
    return 0;
}

// ================================================================
// 基础测试
// ================================================================

static void test_single(FreePagesAllocator::BuddyControlBlock* bcb) {
    printf("[test] single alloc/free...\n");
    KURD_t k;
    phyaddr_t a = bcb->allocate_buddy_way(4096, k);
    if (!success_all_kurd(k) || a == 0) {
        fprintf(stderr, "  FAIL [L%d]: alloc\n", __LINE__);
        g_failures++; return;
    }
    CHECK_OK(bcb->free_buddy_way(a, 4096));
    do_flush(bcb);
    printf("  OK\n");
}

static void test_various_sizes(FreePagesAllocator::BuddyControlBlock* bcb) {
    printf("[test] various sizes...\n");
    static const uint64_t sizes[] = {4096, 8192, 16384, 32768, 65536,
                                     0x20000, 0x40000, 0x80000, 0x100000};
    uint64_t allocs[sizeof(sizes)/sizeof(sizes[0])];
    bool ok = true;
    KURD_t k;

    for (int i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++) {
        allocs[i] = bcb->allocate_buddy_way(sizes[i], k);
        if (!success_all_kurd(k) || allocs[i] == 0) {
            fprintf(stderr, "  FAIL [L%d]: alloc size=%lu\n", __LINE__, sizes[i]);
            g_failures++; ok = false; break;
        }
    }
    if (ok) {
        for (int i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++) {
            if (allocs[i] != 0)
                CHECK_OK(bcb->free_buddy_way(allocs[i], sizes[i]));
        }
        do_flush(bcb);
        printf("  OK\n");
    }
}

static void test_split_coalesce(FreePagesAllocator::BuddyControlBlock* bcb) {
    printf("[test] split & coalesce...\n");
    KURD_t k;
    phyaddr_t p1 = bcb->allocate_buddy_way(4096, k);
    if (!success_all_kurd(k) || p1 == 0) { g_failures++; return; }
    phyaddr_t p2 = bcb->allocate_buddy_way(4096, k);
    if (!success_all_kurd(k) || p2 == 0) { g_failures++; return; }
    CHECK_OK(bcb->free_buddy_way(p1, 4096));
    CHECK_OK(bcb->free_buddy_way(p2, 4096));
    do_flush(bcb);
    printf("  OK\n");
}

// ================================================================
// 压力测试
// ================================================================

static constexpr int   NUM_ITEMS  = 10000;
static constexpr int   NUM_CYCLES = 1000000;
static constexpr int   FLUSH_INTERVAL = 100000;

static void run_stress(FreePagesAllocator::BuddyControlBlock* bcb) {
    printf("\n=== Stress Test ===\n");
    std::vector<mem_seg> items(NUM_ITEMS);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::lognormal_distribution<double> dist(log(1<<14), 1.0);
    std::uniform_int_distribution<int> idx_dist(0, NUM_ITEMS - 1);

    for (auto& item : items) {
        item.addr = 1;
        item.size = ((uint64_t)dist(gen) % 0x100000) + 4096;
    }

    uint64_t alloc_count = 0, free_count = 0;
    uint64_t t0 = rdtsc();

    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        int idx = idx_dist(gen);
        mem_seg& item = items[idx];

        if (item.addr == 1) {
            KURD_t k;
            phyaddr_t a = bcb->allocate_buddy_way(item.size, k);
            if (success_all_kurd(k) && a != 0) {
                item.addr = a;
                alloc_count++;
            }
        } else {
            KURD_t fk = bcb->free_buddy_way(item.addr, item.size);
            if (success_all_kurd(fk)) {
                item.addr = 1;
                free_count++;
            }
        }

        if ((cycle + 1) % FLUSH_INTERVAL == 0) {
            uint64_t t1 = rdtsc();
            printf("  cycle %7d/%d  alloc=%lu free=%lu  flush...",
                   cycle + 1, NUM_CYCLES, alloc_count, free_count);
            fflush(stdout);
            if (do_flush(bcb) == 0)
                printf(" OK  (%lu ms)\n", (t1 - t0) / 1000000);
            else
                printf(" FAIL\n");
        }
    }

    uint64_t remain = 0;
    for (auto& item : items) {
        if (item.addr != 1) {
            bcb->free_buddy_way(item.addr, item.size);
            item.addr = 1;
            remain++;
        }
    }

    uint64_t t1 = rdtsc();
    printf("Done: %lu alloc %lu free, %lu remaining freed, %lu ms\n",
           alloc_count, free_count, remain, (t1 - t0) / 1000000);
    do_flush(bcb);
    printf("Stress: %s\n", g_failures == 0 ? "ALL CLEAN" : "HAD FAILURES");
}

// ================================================================
// 主函数
// ================================================================

int main() {
    printf("=== BCB Test (mixed_bitmap_v2) ===\n\n");

    constexpr phyaddr_t FAKE_BASE = 0x100000000ULL;
    constexpr uint8_t   BCB_ORDER = 16;

    FreePagesAllocator::BuddyControlBlock* bcb =
        new FreePagesAllocator::BuddyControlBlock(FAKE_BASE, BCB_ORDER);

    printf("BCB base=0x%lx  max_order=%u\n", (unsigned long)FAKE_BASE, (unsigned)BCB_ORDER);

    // 分配 bitmap (v4 底座需要 3×2^N bits)
    constexpr uint64_t total_bits = 3ULL << BCB_ORDER;
    constexpr uint64_t bitmap_bytes = ((total_bits + 63) / 64) * sizeof(uint64_t);
    void* bitmap_mem = std::malloc(bitmap_bytes);
    if (!bitmap_mem) { fprintf(stderr, "malloc bitmap failed\n"); return 1; }
    std::memset(bitmap_mem, 0, bitmap_bytes);
    printf("Bitmap: %lu bytes at %p\n", (unsigned long)bitmap_bytes, bitmap_mem);

    bcb->corebcb_mixedbitmap_base_acclaim((vaddr_t)bitmap_mem);
    printf("BCB initialized\n\n");

    // --- 基础测试 ---
    printf("--- Basic Tests ---\n");
    test_single(bcb);
    test_various_sizes(bcb);
    test_split_coalesce(bcb);

    if (g_failures > 0) {
        printf("\n*** %d basic test(s) FAILED ***\n\n", g_failures);
    } else {
        printf("\n*** All basic tests PASSED ***\n\n");
    }

    // --- 压力测试 ---
    if (g_failures == 0) {
        run_stress(bcb);
    }

    do_flush(bcb);
    delete bcb;
    std::free(bitmap_mem);

    printf("\n=== Complete: %d failure(s) ===\n", g_failures);
    return g_failures ? 1 : 0;
}
