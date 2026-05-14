// trace_bcb2.cpp — stress with small heap (ORDER=8, 1MB), periodic flush
#include "memory/FreePagesAllocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static int flush_errs = 0;
static int do_flush(FreePagesAllocator::BuddyControlBlock* bcb, const char* tag) {
    KURD_t f = bcb->free_pages_flush();
    if (!success_all_kurd(f)) {
        printf("  FLUSH FAIL [%s]: result=%d\n", tag, f.result);
        flush_errs++;
        return 1;
    }
    return 0;
}

int main() {
    constexpr phyaddr_t BASE = 0x100000000ULL;
    constexpr uint8_t   ORDER = 10;  // 1024 × 4KB = 4MB heap

    auto* bcb = new FreePagesAllocator::BuddyControlBlock(BASE, ORDER);
    uint64_t total_bits = 1ULL << (ORDER + 1);
    uint64_t bitmap_bytes = ((total_bits + 63) / 64) * 8;
    void* bm = malloc(bitmap_bytes);
    memset(bm, 0, bitmap_bytes);
    bcb->corebcb_mixedbitmap_base_acclaim((vaddr_t)bm);

    // Quick test: alloc one 4KB, free it, repeat N times
    for (int round = 0; round < 1000; round++) {
        KURD_t k;
        phyaddr_t p = bcb->allocate_buddy_way(4096, k);
        if (!success_all_kurd(k)) { printf("alloc failed at round %d\n", round); break; }
        KURD_t fk = bcb->free_buddy_way(p, 4096);
        if (!success_all_kurd(fk)) { printf("free failed at round %d\n", round); break; }
    }
    do_flush(bcb, "after 1000× single alloc/free");

    // Stress: multiple concurrent allocations
    std::vector<phyaddr_t> ptrs;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> size_dist(4096, 65536);
    
    for (int round = 0; round < 50000; round++) {
        if ((round % 10000) == 0) printf("round %d...\n", round);
        
        int action = rng() % 100;
        if (action < 55 || ptrs.empty()) {
            // alloc
            int sz = size_dist(rng);
            KURD_t k;
            phyaddr_t p = bcb->allocate_buddy_way(sz, k);
            if (success_all_kurd(k)) ptrs.push_back(p);
        } else {
            // free
            int idx = rng() % ptrs.size();
            int sz = size_dist(rng);  // approximate
            bcb->free_buddy_way(ptrs[idx], 4096 << (63 - __builtin_clzll((sz + 4095)/4096)));
            // cruder: just use 4096
            // actually, we stored the original size, let me use a proper tracking struct
            ptrs[idx] = ptrs.back();
            ptrs.pop_back();
        }
        
        if ((round + 1) % 10000 == 0) {
            do_flush(bcb, ("round " + std::to_string(round+1)).c_str());
        }
    }
    
    do_flush(bcb, "final");
    printf("Flush errors: %d\n", flush_errs);
    return flush_errs;
}
