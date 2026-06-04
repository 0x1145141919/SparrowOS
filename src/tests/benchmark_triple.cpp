// ════════════════════════════════════════════════════════════════
// benchmark_triple — 三路 Buddy Allocator 性能对比
//
// 固定分配 trace 在三个后端上运行:
//   - v3_simulator  (1-bit, BFS O(2^N))
//   - BuddyControlBlock_foundation (2-bit, DFS O(N))
//   - netty_simulator (8-bit, depth-encoded DFS O(log N))
//
// 编译: g++ -g -O2 -pthread -I../include benchmark_triple.cpp \
//            ../utils/BuddyControlBlock_foundation.o \
//            ../memory/mixed_bitmap.o ../os_error_definitions.o \
//            ../utils/lock.o ../utils/bitmap.o ../utils/util.o \
//            bsp_kout_stub.o panic_stub.o -lpthread -o benchmark_triple
// ════════════════════════════════════════════════════════════════

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <random>
#include <vector>
#include <new>

#include "util/BuddyControlBlock_foundation.h"
#include "v3_simulator.h"
#include "netty_simulator.h"

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static constexpr uint64_t FAKE_BASE = 0x100000000ULL;
static constexpr uint8_t   BCB_ORDER = 16;
static constexpr uint64_t BITMAP_BYTES_V3   = ((2ull << BCB_ORDER) + 63) / 64 * 8;
static constexpr uint64_t BITMAP_BYTES_V4   = ((3ull << BCB_ORDER) + 63) / 64 * 8;
static constexpr uint64_t BITMAP_BYTES_NETTY = 2ull << BCB_ORDER;

// ===== trace =====
struct trace_op {
    bool is_alloc;
    uint8_t order;
    uint64_t addr;     // filled at generation time
};

// ===== alloc/free 包装器模板 =====
template<typename A>
static uint64_t do_alloc(A& a, uint8_t want_order, KURD_t& k) {
    uint8_t bo = want_order;
    uint64_t off = a.find_candidate(bo, k);
    if (!success_all_kurd(k)) return 0;
    if (bo > want_order) {
        k = a.split(bo, off, want_order);
        if (!success_all_kurd(k)) return 0;
        off <<= (bo - want_order);
    }
    k = a.order_occupy_try(want_order, off);
    if (!success_all_kurd(k)) return 0;
    return FAKE_BASE + (off << (want_order + 12));
}

template<typename A>
static bool do_free(A& a, uint64_t addr, uint8_t order) {
    uint64_t off = (addr - FAKE_BASE) >> (order + 12);
    KURD_t k;
    uint8_t r = a.order_return(order, off, k);
    return success_all_kurd(k);
}

// ===== trace generation =====
// 用 v4 backend 产生真实地址，保证 trace 可 replay
static std::vector<trace_op> gen_trace(uint64_t target_ops, unsigned seed = 42) {
    std::vector<trace_op> trace;
    trace.reserve(target_ops * 2);

    // allocator for trace generation
    uint64_t bmb = ((3ull << BCB_ORDER) + 63) / 64 * 8;
    void* bm = std::malloc(bmb);
    std::memset(bm, 0, bmb);
    BuddyControlBlock_foundation g;
    g.init((uint64_t)bm, BCB_ORDER);

    std::mt19937_64 gen(seed);
    std::exponential_distribution<double> od(0.3);

    constexpr size_t N = 20000;
    struct I { uint64_t a = 0; uint8_t o = 0; bool alive = false; };
    I items[N];
    std::uniform_int_distribution<size_t> didx(0, N - 1);

    for (uint64_t i = 0; i < target_ops; i++) {
        size_t idx = didx(gen);
        I& it = items[idx];

        if (!it.alive) {
            double v = od(gen);
            it.o = (uint8_t)((uint64_t)v % (BCB_ORDER + 1));
            KURD_t k;
            uint64_t a = do_alloc(g, it.o, k);
            if (success_all_kurd(k) && a) {
                it.a = a; it.alive = true;
                trace.push_back({true, it.o, a});
            } else { i--; } // retry
        } else {
            trace.push_back({false, it.o, it.a});
            do_free(g, it.a, it.o);
            it.alive = false;
        }
    }
    // cleanup
    for (auto& it : items) {
        if (it.alive) {
            trace.push_back({false, it.o, it.a});
            do_free(g, it.a, it.o);
        }
    }
    std::free(bm);
    printf("  Trace: %zu ops\n", trace.size());
    return trace;
}

// ===== run one backend =====
template<typename A>
static void run(A& a, const std::vector<trace_op>& trace,
                const char* name, uint64_t bm_bytes)
{
    printf("\n--- %s (bitmap=%lu B) ---\n", name, (unsigned long)bm_bytes);
    uint64_t t0 = now_ns(), ti = t0;
    uint64_t ok_a = 0, fa_a = 0, ok_f = 0, fa_f = 0;
    uint64_t ops = 0;
    constexpr uint64_t RI = 100000;

    for (size_t i = 0; i < trace.size(); i++) {
        auto& op = const_cast<trace_op&>(trace[i]);
        if (op.is_alloc) {
            KURD_t k;
            uint64_t addr = do_alloc(a, op.order, k);
            if (success_all_kurd(k) && addr) { op.addr = addr; ok_a++; }
            else { op.addr = 0; fa_a++; }
        } else {
            if (do_free(a, op.addr, op.order)) ok_f++; else fa_f++;
        }
        ops++;
        if (ops % RI == 0) {
            uint64_t tn = now_ns();
            printf("  [%6zuK] a=%lu/%lu f=%lu/%lu  %.0f ns/op\n",
                   ops/1000, ok_a, fa_a, ok_f, fa_f,
                   (double)(tn - ti) / (ops % (RI * 2)));
            ti = tn;
        }
    }

    uint64_t tt = now_ns() - t0;
    printf("  TOTAL: %lu ms  %.0f ns/op\n",
           (unsigned long)(tt / 1000000), (double)tt / trace.size());
    printf("  a=%lu f=%lu  fail_a=%lu fail_f=%lu\n",
           ok_a, ok_f, fa_a, fa_f);
}

int main() {
    printf("=== Triple Backend Benchmark ===\n");
    printf("BCB_ORDER=%u  size=%lu MB\n\n",
           (unsigned)BCB_ORDER,
           (unsigned long)((1ull << (BCB_ORDER + 12)) >> 20));

    auto trace = gen_trace(1000000);

    // v3
    {
        void* bm = std::malloc(BITMAP_BYTES_V3);
        std::memset(bm, 0, BITMAP_BYTES_V3);
        v3_simulator a;
        a.init((uint64_t)bm, BCB_ORDER);
        run(a, trace, "BCB_v3 (1-bit BFS)", BITMAP_BYTES_V3);
        std::free(bm);
    }

    // v4
    {
        void* bm = std::malloc(BITMAP_BYTES_V4);
        std::memset(bm, 0, BITMAP_BYTES_V4);
        BuddyControlBlock_foundation a;
        a.init((uint64_t)bm, BCB_ORDER);
        run(a, trace, "BCB_v4 (2-bit DFS)", BITMAP_BYTES_V4);
        std::free(bm);
    }

    // Netty
    {
        void* bm = std::malloc(BITMAP_BYTES_NETTY);
        std::memset(bm, 0, BITMAP_BYTES_NETTY);
        netty_simulator a;
        a.init((uint64_t)bm, BCB_ORDER);
        run(a, trace, "Netty-style (8-bit depth)", BITMAP_BYTES_NETTY);
        std::free(bm);
    }

    printf("\n=== Done ===\n");
    return 0;
}
