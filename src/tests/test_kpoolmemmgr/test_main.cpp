// ════════════════════════════════════════════════════════════════
// test_kpoolmemmgr - HCB_v3 用户态 Phase 1 测试
//
// 测试目标:kpoolmemmgr_t::HCB_v3 的 buddy 算法 (alloc/free/realloc/clear)
// 不依赖:FreePagesAllocator, KspacePageTable, kout, Panic
// 编译:make -f Makefile
// ════════════════════════════════════════════════════════════════

// TEST_MODE defined via compiler flags

// ===== STUB 区 =====
// 在 include kernel headers 之前提供所有需要的桩

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

// --- kernel global stubs (bsp_kout_stub.cpp 已提供 fast_get_processor_id / base_kernel_address) ---
static thread_local uint32_t g_test_proc_id = 0;

// --- kout stub ---
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

// --- Panic stub ---
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

// --- rdtsc stub (use CLOCK_MONOTONIC) ---
static inline uint64_t rdtsc() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ksystemramcpy declared in util/OS_utils.h — no stub needed

// --- KURD helpers (defined in os_error_definitions.cpp, protos here) ---
#include "abi/os_error_definitions.h"

// --- operator new/delete (kernel provides these; stub for test) ---
// Only define the non-placement versions; placement new is in <new>
void* operator new(size_t sz) { return malloc(sz); }
void* operator new[](size_t sz) { return malloc(sz); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// ===== 包含 kernel 头 =====
// 这些在 TEST_MODE 下应只编译纯类型和算法代码
#include "memory/kpoolmemmgr.h"

// ===== HCB_v3 便利构造器 =====
// 在用户态用 mmap 分配 2M 对齐的 data buffer 和 bitmap buffer
// 然后用 test_init 初始化 HCB_v3

struct hcb_test_setup {
    void*   data_va;
    void*   bitmap_va;
    uint32_t data_size;
    uint32_t bitmap_size;
    kpoolmemmgr_t::HCB_v3 hcb;

    hcb_test_setup(uint32_t size = 0x200000 /* 2MB */) : data_size(size) {
        // bitmap size: total_bits for max_order=16 → 2^17 bits = 16KB
        constexpr uint64_t total_bits = 1ULL << (kpoolmemmgr_t::MAX_ORDER + 1);
        bitmap_size = ((total_bits + 63) / 64) * sizeof(uint64_t);

        // 2M 对齐 data buffer
        if (posix_memalign(&data_va, 0x200000, data_size) != 0) {
            fprintf(stderr, "posix_memalign(2MB) failed\n");
            exit(1);
        }
        bitmap_va = malloc(bitmap_size);
        if (!bitmap_va) {
            fprintf(stderr, "malloc bitmap failed\n");
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

// ----- 1. 单次 alloc/free -----
void test_single_alloc_free() {
    printf("[test] single_alloc_free...\n");
    hcb_test_setup s;
    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 64, alloc_flags_t{});
    CHECK_KURD(k);
    CHECK(p != nullptr, "alloc returned null");
    k = s.hcb.free(p);
    CHECK_KURD(k);
    printf("  single alloc/free OK\n");
}

// ----- 2. 各种 size 分配 -----
void test_various_sizes() {
    printf("[test] various_sizes...\n");
    hcb_test_setup s;
    struct { uint32_t size; void* ptr; } allocs[] = {
        {32, nullptr}, {64, nullptr}, {128, nullptr},
        {256, nullptr}, {512, nullptr}, {1024, nullptr},
        {4096, nullptr}, {16384, nullptr}, {65536, nullptr},
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

// ----- 3. 分裂 + 折叠 -----
void test_split_and_coalesce() {
    printf("[test] split_and_coalesce...\n");
    hcb_test_setup s;
    // 分配 4KB (order 7: 32B * 2^7 = 4KB) → 触发分裂
    void* p1 = nullptr, *p2 = nullptr;
    KURD_t k = s.hcb.alloc(p1, 4096, alloc_flags_t{});
    CHECK_KURD(k);
    // 再分配一个 4KB 级联分裂
    k = s.hcb.alloc(p2, 4096, alloc_flags_t{});
    CHECK_KURD(k);
    // 释放 p1 → 应该触发折叠
    k = s.hcb.free(p1);
    CHECK_KURD(k);
    // 释放 p2 → 应该触发进一步折叠
    k = s.hcb.free(p2);
    CHECK_KURD(k);
    CHECK_FLUSH(s.hcb);
}

// ----- 4. realloc -----
void test_realloc() {
    printf("[test] realloc...\n");
    hcb_test_setup s;

    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 64, alloc_flags_t{});
    CHECK_KURD(k);
    CHECK(p != nullptr, "alloc null");

    // realloc 到更大
    k = s.hcb.realloc(p, 512, alloc_flags_t{});
    CHECK_KURD(k);

    // realloc 到更小 (should succeed in-place)
    k = s.hcb.realloc(p, 32, alloc_flags_t{});
    CHECK_KURD(k);

    k = s.hcb.free(p);
    CHECK_KURD(k);
    CHECK_FLUSH(s.hcb);
}

// ----- 5. clear -----
void test_clear() {
    printf("[test] clear...\n");
    hcb_test_setup s;

    void* p = nullptr;
    KURD_t k = s.hcb.alloc(p, 128, alloc_flags_t{});
    CHECK_KURD(k);

    // write pattern
    memset(p, 0xAA, 128);
    k = s.hcb.clear(p);
    CHECK_KURD(k);

    // verify zeroed
    uint8_t* buf = (uint8_t*)p;
    bool all_zero = true;
    for (int i = 0; i < 128; i++) if (buf[i] != 0) { all_zero = false; break; }
    CHECK(all_zero, "clear did not zero payload");

    k = s.hcb.free(p);
    CHECK_KURD(k);
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

    // 初始化测试集
    for (auto& item : items) {
        uint64_t sz = (uint64_t)size_dist(gen) % 0x100000 + 1;  // 1B ~ 1MB
        item.size = sz < 32 ? 32 : sz;  // minimum order 0 size
    }

    uint64_t alloc_count = 0, free_count = 0;
    uint64_t start_tsc = rdtsc();

    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        int idx = idx_dist(gen);
        test_item& item = items[idx];

        if (item.in_use) {
            // free
            KURD_t k = s.hcb.free(item.ptr);
            if (success_all_kurd(k)) {
                item.in_use = false;
                item.ptr = nullptr;
                free_count++;
            }
        } else {
            // alloc
            KURD_t k = s.hcb.alloc(item.ptr, item.size, flags);
            if (success_all_kurd(k)) {
                item.in_use = true;
                alloc_count++;
                // write pattern to verify memory
                memset(item.ptr, 0xCD, item.size > 4096 ? 4096 : item.size);
            }
        }

        // 每 CHECK_INTERVAL 轮校验
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

    // 释放所有残留
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
// Phase 2: 多 HCB 多线程测试 (通过 kpoolmemmgr_t 接口)
// ================================================================

// 声明在 bsp_kout_stub.cpp 中
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
    std::uniform_int_distribution<int> size_dist(32, 4096);
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

    // 初始化 first heap
    kpoolmemmgr_t::Init();

    // multi_heap_enable
    KURD_t k = kpoolmemmgr_t::multi_heap_enable();
    if (!success_all_kurd(k)) {
        printf("  multi_heap_enable FAILED! kurd=%d\n", k.result);
        g_test_failures++;
        return;
    }
    printf("  multi_heap_enable OK\n");

    // 验证全局设置
    printf("  logical_processor_count=%lu, heap_area.start=0x%lx, muli_enabled=%d\n",
           logical_processor_count, kpoolmemmgr_t::heap_area.start,
           kpoolmemmgr_t::is_muli_heap_enabled);



    // 启动线程
    int nthreads = 4;
    pthread_t threads[4];
    thread_arg args[4];
    for (int i = 0; i < nthreads; i++) {
        args[i] = {i, 50000, 200000, 0};
        pthread_create(&threads[i], nullptr, multi_heap_thread, &args[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(threads[i], nullptr);

    // 验证所有已 onlined 的 HCB
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
    printf("=== kpoolmemmgr HCB_v3 Phase 1 Test ===\n\n");
    printf("MAX_ORDER=%u, BYTES_PER_ORDER0=%u, HCB_DEFAULT_SIZE=0x%x (%u KB)\n\n",
           (unsigned)kpoolmemmgr_t::MAX_ORDER,
           (unsigned)kpoolmemmgr_t::BYTES_PER_ORDER0,
           (unsigned)kpoolmemmgr_t::HCB_DEFAULT_SIZE,
           (unsigned)kpoolmemmgr_t::HCB_DEFAULT_SIZE / 1024);

    // --- 基础测试 ---
    printf("--- Basic Tests ---\n");
    test_single_alloc_free();
    test_various_sizes();
    test_split_and_coalesce();
    test_realloc();
    test_clear();

    if (g_test_failures > 0) {
        printf("\n*** %d basic test(s) FAILED ***\n\n", g_test_failures);
    } else {
        printf("\n*** All basic tests PASSED ***\n\n");
    }

    // --- 压力测试 ---
    if (g_test_failures == 0) {
        run_stress_test();
    }

    // --- Phase 2: 多堆多线程 ---
    if (g_test_failures == 0) {
        run_multi_heap_test();
    }

    printf("\n=== Test Complete: %d failure(s) ===\n", g_test_failures);
    return g_test_failures ? 1 : 0;
}
