// ════════════════════════════════════════════════════════════════
// test_kpoolmemmgr - HCB_v2.1 用户态 Phase 1 测试
//
// 测试目标:kpoolmemmgr_t::HCB_v2 的 flat bitmap next-fit 算法
// (alloc/free/realloc/clear)
// 不依赖:FreePagesAllocator, KspacePageTable, kout, Panic
// 编译:make -f Makefile
// ════════════════════════════════════════════════════════════════

// TEST_MODE defined via compiler flags

// ===== STUB 区 =====
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <new>
#include <vector>
#include <random>
#include <pthread.h>

static thread_local uint32_t g_test_proc_id = 0;

#define kendl "\n"
static class kout_stub {
public:
    void Init() {}
    void shift_dec() {}
    kout_stub& operator<<(const char* s) { fprintf(stderr, "%s", s); return *this; }
    kout_stub& operator<<(const void* p) { fprintf(stderr, "%p", p); return *this; }
    kout_stub& operator<<(unsigned long v) { fprintf(stderr, "%lu", v); return *this; }
    kout_stub& operator<<(long v) { fprintf(stderr, "%ld", v); return *this; }
    kout_stub& operator<<(unsigned v) { fprintf(stderr, "%u", v); return *this; }
    kout_stub& operator<<(int v) { fprintf(stderr, "%d", v); return *this; }
    template<typename T> kout_stub& operator<<(const T&) { return *this; }
} bsp_kout;

struct panic_info_inshort {
    bool is_bug, is_policy, is_hw_fault, is_mem_corruption, is_escalated;
};
struct panic_behaviors_flags { uint64_t v; };
constexpr panic_behaviors_flags default_panic_behaviors_flags{0};

namespace Panic {
    inline void panic(panic_behaviors_flags, const char* msg, ...) {
        fprintf(stderr, "PANIC: %s\n", msg);
        abort();
    }
}

static inline uint64_t rdtsc() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#include "abi/os_error_definitions.h"

void* operator new(size_t sz) { return malloc(sz); }
void* operator new[](size_t sz) { return malloc(sz); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

#include "memory/kpoolmemmgr.h"

// ===== HCB_v2 便利构造器 =====
struct hcb_test_setup {
    void*   data_va;
    void*   bitmap_va;
    uint32_t data_size;
    uint32_t bitmap_size;
    kpoolmemmgr_t::HCB_v2 hcb;

    hcb_test_setup(uint32_t size = 0x200000 /* 2MB */) : data_size(size) {
        uint64_t total_bits = data_size / kpoolmemmgr_t::BYTES_PER_BIT;
        bitmap_size = (total_bits + 7) / 8;

        if (posix_memalign(&data_va, 0x200000, data_size) != 0) {
            fprintf(stderr, "posix_memalign(2MB) failed\n");
            exit(1);
        }
        if (posix_memalign(&bitmap_va, 64, bitmap_size) != 0) {
            fprintf(stderr, "posix_memalign(bitmap) failed\n");
            exit(1);
        }
        memset(bitmap_va, 0, bitmap_size);

        hcb.test_init((vaddr_t)data_va, (vaddr_t)bitmap_va, data_size);
    }

    ~hcb_test_setup() {
        free(data_va);
        free(bitmap_va);
    }
};

// ===== 校验宏 =====
static int g_test_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [L%d]: %s\n", __LINE__, msg); \
        g_test_failures++; \
    } \
} while(0)

#define CHECK_KURD(kurd) CHECK(success_all_kurd(kurd), "KURD not SUCCESS")

#define CHECK_FLUSH(hcb) do { \
    KURD_t _fk = (hcb).flush_free_count(); \
    if (!success_all_kurd(_fk)) { \
        fprintf(stderr, "  FAIL [L%d]: flush_free_count: result=%d reason=%d\n", \
                __LINE__, (int)_fk.result, (int)_fk.reason); \
        g_test_failures++; \
    } \
} while(0)

// ================================================================
// 基础测试
// ================================================================

void test_single_alloc_free() {
    printf("[test] single_alloc_free...\n");
    hcb_test_setup s;
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 64, alloc_flags_t{});
    CHECK_KURD(k);
    CHECK(p != nullptr, "alloc returned null");
    CHECK((uint64_t)p % 8 == 0, "ptr not 8B aligned");
    k = s.hcb.free(p);
    CHECK_KURD(k);
    CHECK_FLUSH(s.hcb);
    printf("  single alloc/free OK\n");
}

void test_various_sizes() {
    printf("[test] various_sizes...\n");
    hcb_test_setup s;
    struct { uint32_t size; void* ptr; } allocs[] = {
        {8, nullptr}, {16, nullptr}, {32, nullptr},
        {64, nullptr}, {128, nullptr}, {256, nullptr},
        {512, nullptr}, {1024, nullptr}, {4096, nullptr},
        {16384, nullptr}, {65536, nullptr},
        {0x20000, nullptr}, {0x80000, nullptr},
    };
    for (auto& a : allocs) {
        KURD_t k = s.hcb.alloc(a.ptr, a.size, alloc_flags_t{});
        CHECK_KURD(k);
        CHECK(a.ptr != nullptr, "alloc returned null");
    }
    for (auto& a : allocs) {
        KURD_t k = s.hcb.free(a.ptr);
        CHECK_KURD(k);
    }
    CHECK_FLUSH(s.hcb);
}

void test_min_alloc() {
    printf("[test] min_alloc (8B + 8B meta = 16B)...\n");
    hcb_test_setup s;
    // 最小分配 = 8B 用户数据 + 8B meta = 16B = 2 bits
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 8, alloc_flags_t{});
    CHECK_KURD(k);
    CHECK(p != nullptr, "min alloc null");
    k = s.hcb.free(p);
    CHECK_KURD(k);
    CHECK_FLUSH(s.hcb);
    printf("  min alloc OK\n");
}

void test_next_fit_wrap() {
    printf("[test] next_fit_wrap (fill heap, free middle, verify reuse)...\n");
    hcb_test_setup s;
    // 分配并释放来验证 next-fit wrap-around
    void* p1, *p2, *p3;
    KURD_t k1 = s.hcb.alloc(p1, 64, alloc_flags_t{});
    KURD_t k2 = s.hcb.alloc(p2, 64, alloc_flags_t{});
    KURD_t k3 = s.hcb.alloc(p3, 64, alloc_flags_t{});
    CHECK_KURD(k1); CHECK_KURD(k2); CHECK_KURD(k3);

    // 释放中间的块
    k2 = s.hcb.free(p2);
    CHECK_KURD(k2);

    // 再分配 — 应重用 p2 的位置 (next-fit)
    void* p4;
    KURD_t k4 = s.hcb.alloc(p4, 64, alloc_flags_t{});
    CHECK_KURD(k4);
    CHECK(p4 == p2, "next-fit should reuse p2");

    s.hcb.free(p1); s.hcb.free(p4); s.hcb.free(p3);
    CHECK_FLUSH(s.hcb);
    printf("  next_fit_wrap OK\n");
}

// ================================================================
// canary / 越界检测测试
// ================================================================

void test_canary() {
    printf("[test] canary fill and check...\n");
    hcb_test_setup s;
    // 分配 10B → 需要 (10+8)=18B → round up to 24B = 3 bits
    // padding = 24 - 18 = 6B of 0xFF
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 10, alloc_flags_t{});
    CHECK_KURD(k);
    // canary 在 user_ptr + 10 处，长度 6
    uint8_t* up = (uint8_t*)p;
    for (int i = 10; i < 16; i++) {
        CHECK(up[i] == 0xFF, "canary byte not 0xFF");
    }
    k = s.hcb.free(p);
    CHECK_KURD(k);
    CHECK_FLUSH(s.hcb);
    printf("  canary OK\n");
}

void test_double_free_detection() {
    printf("[test] double_free detection...\n");
    hcb_test_setup s;
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 32, alloc_flags_t{});
    CHECK_KURD(k);
    k = s.hcb.free(p);
    CHECK_KURD(k);
    // Second free should FAIL, not FATAL (not metadata corruption, just double-free)
    k = s.hcb.free(p);
    CHECK(!success_all_kurd(k), "double free should fail");
    printf("  double_free detection OK\n");
}

// ================================================================
// realloc 测试
// ================================================================

void test_realloc_basic() {
    printf("[test] realloc basic...\n");
    hcb_test_setup s;
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 64, alloc_flags_t{});
    CHECK_KURD(k);

    // 原地缩小
    k = s.hcb.realloc(p, 16, alloc_flags_t{});
    CHECK_KURD(k);

    // 原地扩展（相邻空闲）
    k = s.hcb.realloc(p, 128, alloc_flags_t{});
    CHECK_KURD(k);

    k = s.hcb.free(p);
    CHECK_KURD(k);
    CHECK_FLUSH(s.hcb);
    printf("  realloc basic OK\n");
}

void test_realloc_fallback() {
    printf("[test] realloc fallback (alloc-copy-free)...\n");
    hcb_test_setup s;
    // 填满 heap 让原地扩展失败 → fallback
    void* p1 = nullptr;
    KURD_t k = s.hcb.alloc(p1, 0x100000, alloc_flags_t{});  // 1MB
    CHECK_KURD(k);
    void* p2 = nullptr;
    k = s.hcb.alloc(p2, 64, alloc_flags_t{});  // 紧接
    CHECK_KURD(k);
    // realloc p2 到更大 — 相邻位被占，fallback
    k = s.hcb.realloc(p2, 4096, alloc_flags_t{});
    CHECK_KURD(k);
    s.hcb.free(p1);
    s.hcb.free(p2);
    CHECK_FLUSH(s.hcb);
    printf("  realloc fallback OK\n");
}

void test_clear() {
    printf("[test] clear...\n");
    hcb_test_setup s;
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 128, alloc_flags_t{});
    CHECK_KURD(k);
    memset(p, 0xAA, 128);
    k = s.hcb.clear(p);
    CHECK_KURD(k);
    uint8_t* buf = (uint8_t*)p;
    bool all_zero = true;
    for (int i = 0; i < 128; i++) if (buf[i] != 0) { all_zero = false; break; }
    CHECK(all_zero, "clear did not zero payload");
    k = s.hcb.free(p);
    CHECK_KURD(k);
    printf("  clear OK\n");
}

// ================================================================
// 碎片测试 — 验证 flat bitmap 的碎片行为
// ================================================================

void test_fragmentation() {
    printf("[test] fragmentation (alternating alloc/free)...\n");
    hcb_test_setup s;

    // 分配 64 个 1KB 块 = 64KB，间隔释放
    constexpr int N = 64;
    void* ptrs[N];
    for (int i = 0; i < N; i++) {
        KURD_t k = s.hcb.alloc(ptrs[i], 1024, alloc_flags_t{});
        CHECK_KURD(k);
    }
    // 释放偶数块 — 制造外部碎片
    for (int i = 0; i < N; i += 2) {
        KURD_t k = s.hcb.free(ptrs[i]);
        CHECK_KURD(k);
    }
    // 再分配 512B — 应能放入碎片缝隙
    for (int i = 0; i < N / 2; i++) {
        void* p = nullptr;
        KURD_t k = s.hcb.alloc(p, 512, alloc_flags_t{});
        CHECK_KURD(k);
    }
    // 清理
    for (int i = 1; i < N; i += 2) {
        s.hcb.free(ptrs[i]);
    }
    CHECK_FLUSH(s.hcb);
    printf("  fragmentation OK\n");
}

// ================================================================
// 压力测试
// ================================================================

static constexpr int    NUM_TEST_ITEMS = 10000;
static constexpr int    NUM_CYCLES     = 1000000;
static constexpr int    CHECK_INTERVAL = 100000;

struct test_item {
    void*  ptr    = nullptr;
    size_t size   = 0;
    bool   in_use = false;
};

static void run_stress_test() {
    printf("\n=== Stress Test ===\n");
    hcb_test_setup s;
    std::vector<test_item> items(NUM_TEST_ITEMS);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::lognormal_distribution<double> size_dist(log(128), 1.2);
    std::uniform_int_distribution<int> idx_dist(0, NUM_TEST_ITEMS - 1);
    alloc_flags_t flags{};

    // 初始化随机 size
    for (auto& item : items) {
        uint64_t sz = (uint64_t)size_dist(gen) % 0x100000 + 1;
        item.size = sz < 8 ? 8 : sz;  // minimum 8B
    }

    uint64_t alloc_count = 0, free_count = 0;
    uint64_t start_tsc = rdtsc();

    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        int idx = idx_dist(gen);
        test_item& item = items[idx];

        if (item.in_use) {
            KURD_t k = s.hcb.free(item.ptr);
            if (success_all_kurd(k)) {
                item.in_use = false;
                item.ptr = nullptr;
                free_count++;
            }
        } else {
            KURD_t k = s.hcb.alloc(item.ptr, item.size, flags);
            if (success_all_kurd(k)) {
                item.in_use = true;
                alloc_count++;
                memset(item.ptr, 0xCD, item.size > 4096 ? 4096 : item.size);
            }
        }

        if ((cycle + 1) % CHECK_INTERVAL == 0) {
            uint64_t now = rdtsc();
            printf("  cycle %7d / %d | alloc=%lu free=%lu | checking...",
                   cycle + 1, NUM_CYCLES, alloc_count, free_count);
            fflush(stdout);
            CHECK_FLUSH(s.hcb);
            printf(" OK  (%lu ms)\n", (now - start_tsc) / 1000000);
        }
    }

    uint64_t end_tsc = rdtsc();
    printf("Stress test done: %lu alloc, %lu free in %lu ms\n",
           alloc_count, free_count, (end_tsc - start_tsc) / 1000000);

    uint64_t freed_remaining = 0;
    for (auto& item : items) {
        if (item.in_use) {
            s.hcb.free(item.ptr);
            item.in_use = false;
            freed_remaining++;
        }
    }
    printf("Freed %lu remaining allocations\n", freed_remaining);
    CHECK_FLUSH(s.hcb);
    printf("Stress test: ALL CLEAN\n");
}

// ================================================================
// Phase 2: 多 HCB 多线程测试
// ================================================================

void test_set_processor_id(uint32_t id);
extern uint64_t logical_processor_count;

struct thread_arg {
    int id;
    int allocs_per_thread;
    int iterations;
    int failures;
};

static void* multi_heap_thread(void* arg) {
    thread_arg* ta = (thread_arg*)arg;
    test_set_processor_id(ta->id);
    std::mt19937_64 rng(42 + ta->id);
    std::uniform_int_distribution<int> size_dist(8, 4096);
    std::uniform_int_distribution<int> action_dist(0, 99);
    alloc_flags_t flags{};

    std::vector<std::pair<void*, size_t>> live;
    for (int i = 0; i < ta->iterations; i++) {
        int action = action_dist(rng);
        if (action < 55 || live.empty()) {
            int sz = size_dist(rng);
            KURD_t k;
            void* p = kpoolmemmgr_t::kalloc(sz, k, flags);
            if (p) {
                memset(p, 0xCD, sz > 64 ? 64 : sz);
                live.push_back({p, (size_t)sz});
            }
        } else {
            int idx = rng() % live.size();
            kpoolmemmgr_t::kfree(live[idx].first);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& l : live) kpoolmemmgr_t::kfree(l.first);
    printf("  Thread %d: done, %lu live freed\n", ta->id, live.size());
    return nullptr;
}

static void run_multi_heap_test() {
    printf("\n=== Phase 2: Multi-HCB Multi-Thread ===\n");

    uint64_t nproc = 4;
    logical_processor_count = nproc;
    uint64_t hcb_count = nproc * (1 << kpoolmemmgr_t::PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2);
    printf("  processors=%lu, HCBs=%lu (%lu each)\n",
           nproc, hcb_count, (1u << kpoolmemmgr_t::PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2));

    kpoolmemmgr_t::Init();

    KURD_t k = kpoolmemmgr_t::multi_heap_enable();
    if (!success_all_kurd(k)) {
        printf("  multi_heap_enable FAILED! kurd=%d\n", k.result);
        g_test_failures++;
        return;
    }
    printf("  multi_heap_enable OK\n");

    printf("  logical_processor_count=%lu, heap_area.start=0x%lx, muli_enabled=%d\n",
           logical_processor_count, kpoolmemmgr_t::heap_area.start,
           kpoolmemmgr_t::is_muli_heap_enabled);

    int nthreads = 4;
    pthread_t threads[4];
    thread_arg args[4];
    for (int i = 0; i < nthreads; i++) {
        args[i] = {i, 50000, 200000, 0};
        pthread_create(&threads[i], nullptr, multi_heap_thread, &args[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(threads[i], nullptr);

    printf("\n  Verifying onlined HCBs...\n");
    uint32_t validated = 0;
    for (uint32_t i = 0; i < hcb_count; i++) {
        if (!kpoolmemmgr_t::HCB_ARRAY[i].valid) continue;
        KURD_t fk = kpoolmemmgr_t::HCB_ARRAY[i].flush_free_count();
        if (!success_all_kurd(fk)) {
            printf("  HCB[%u] flush FAILED! result=%d\n", i, fk.result);
            g_test_failures++;
        }
        validated++;
    }
    printf("  %u/%lu HCBs validated\n", validated, hcb_count);
    printf("  All HCBs verified\n");
}

// ================================================================
// 主函数
// ================================================================

int main() {
    printf("=== kpoolmemmgr HCB_v2.1 Phase 1 Test ===\n\n");
    printf("BYTES_PER_BIT=%u, HCB_DEFAULT_SIZE=0x%x (%u KB)\n\n",
           (unsigned)kpoolmemmgr_t::BYTES_PER_BIT,
           (unsigned)kpoolmemmgr_t::HCB_DEFAULT_SIZE,
           (unsigned)kpoolmemmgr_t::HCB_DEFAULT_SIZE / 1024);

    // --- 基础测试 ---
    printf("--- Basic Tests ---\n");
    test_single_alloc_free();
    test_various_sizes();
    test_min_alloc();
    test_next_fit_wrap();
    test_canary();
    test_double_free_detection();
    test_realloc_basic();
    test_realloc_fallback();
    test_clear();
    test_fragmentation();

    if (g_test_failures > 0) {
        printf("\n*** %d basic test(s) FAILED ***\n\n", g_test_failures);
    } else {
        printf("\n*** All basic tests PASSED ***\n\n");
    }

    // --- 压力测试 ---
    if (g_test_failures == 0) {
        run_stress_test();
    }

    // --- Phase 2 ---
    printf("\n[skip] Phase 2 (multi-heap) requires kpoolmemmgr.cpp + stubs\n");

    printf("\n=== Test Complete: %d failure(s) ===\n", g_test_failures);
    return g_test_failures ? 1 : 0;
}
