// trace_bcb.cpp — 确定性小规模 BCB 追踪测试
#include "memory/FreePagesAllocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void show_free_counts(FreePagesAllocator::BuddyControlBlock* bcb) {
    printf("  free_counts: ");
    for (int o = 0; o <= 18; o++) {
        // can't access private free_count directly — use free_pages_flush-like logic
    }
    printf("\n");
}

int main() {
    constexpr phyaddr_t BASE = 0x100000000ULL;
    constexpr uint8_t   ORDER = 4;  // tiny: 16 × 4KB = 64KB heap

    auto* bcb = new FreePagesAllocator::BuddyControlBlock(BASE, ORDER);
    uint64_t total_bits = 1ULL << (ORDER + 1);
    uint64_t bitmap_bytes = ((total_bits + 63) / 64) * 8;
    void* bm = malloc(bitmap_bytes);
    memset(bm, 0, bitmap_bytes);
    bcb->corebcb_mixedbitmap_base_acclaim((vaddr_t)bm);

    printf("=== Trace BCB order=%d\n", ORDER);

    // 1. alloc one 4KB block
    KURD_t k;
    phyaddr_t a1 = bcb->allocate_buddy_way(4096, k);
    printf("alloc 4KB → 0x%lx  kurd=%d\n", a1, k.result);
    KURD_t f1 = bcb->free_pages_flush();
    printf("flush after alloc: %s\n", success_all_kurd(f1) ? "OK" : "FAIL");

    // 2. free it
    KURD_t fk = bcb->free_buddy_way(a1, 4096);
    printf("free 0x%lx → kurd=%d\n", a1, fk.result);
    KURD_t f2 = bcb->free_pages_flush();
    printf("flush after free: %s\n", success_all_kurd(f2) ? "OK" : "FAIL");

    // 3. alloc two 4KB blocks
    phyaddr_t a2 = bcb->allocate_buddy_way(4096, k);
    phyaddr_t a3 = bcb->allocate_buddy_way(4096, k);
    printf("alloc 4KB×2 → 0x%lx 0x%lx\n", a2, a3);
    KURD_t f3 = bcb->free_pages_flush();
    printf("flush after 2 allocs: %s\n", success_all_kurd(f3) ? "OK" : "FAIL");

    // 4. free both (should coalesce)
    bcb->free_buddy_way(a2, 4096);
    bcb->free_buddy_way(a3, 4096);
    KURD_t f4 = bcb->free_pages_flush();
    printf("flush after free both: %s\n", success_all_kurd(f4) ? "OK" : "FAIL");

    // 5. alloc different sizes
    phyaddr_t sizes[] = {4096, 8192, 16384, 32768};
    phyaddr_t ptrs[4];
    for (int i = 0; i < 4; i++) {
        ptrs[i] = bcb->allocate_buddy_way(sizes[i], k);
        printf("alloc %lu → 0x%lx\n", sizes[i], ptrs[i]);
    }
    KURD_t f5 = bcb->free_pages_flush();
    printf("flush after 4 mixed allocs: %s\n", success_all_kurd(f5) ? "OK" : "FAIL");

    // 6. free in reverse order
    for (int i = 3; i >= 0; i--) {
        bcb->free_buddy_way(ptrs[i], sizes[i]);
    }
    KURD_t f6 = bcb->free_pages_flush();
    printf("flush after reverse free: %s\n", success_all_kurd(f6) ? "OK" : "FAIL");

    delete bcb;
    free(bm);
    return 0;
}
