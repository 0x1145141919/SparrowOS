#pragma once
#include "stdint.h"
#include "util/bitmap.h"
#include "memmodule_err_definitions.h"
#include "abi/boot.h"
#include <util/lock.h>
#include "memory/memory_base.h"

typedef uint64_t size_t;
typedef uint64_t phyaddr_t;
typedef uint64_t vaddr_t;

namespace MEMMODULE_LOCAIONS {
    // kpoolmemmgr 沿用 [4~7]
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR = 4;
    namespace KPOOLMEMMGR_EVENTS {
        constexpr uint8_t EVENT_CODE_INIT = 0;
        constexpr uint8_t EVENT_CODE_ALLOC = 1;
        namespace ALLOC_RESULTS::FAIL_RESONS {
            constexpr uint16_t REASON_CODE_NO_AVALIABLE_MEM = 4;
            constexpr uint16_t REASON_CODE_SIZE_IS_ZERO = 5;
        }
        constexpr uint8_t EVENT_CODE_REALLOC = 2;
        namespace REALLOC_RESULTS::FAIL_RESONS {
            constexpr uint16_t REASON_CODE_DEMAND_SIZE_IS_ZERO = 4;
            constexpr uint16_t REASON_CODE_PTR_NOT_IN_ANY_HEAP = 5;
            constexpr uint16_t REASON_CODE_NO_AVALIABLE_MEM = 6;
        }
        constexpr uint8_t EVENT_CODE_PER_PROCESSOR_HEAP_INIT = 3;
        namespace PER_PROCESSOR_HEAP_INIT_RESULTS::FAIL_RESONS {
            constexpr uint16_t REASON_CODE_ALREADY_ENABLED    = 1;
            constexpr uint16_t REASON_CODE_BAD_PROCESSOR_COUNT = 2;
            constexpr uint16_t REASON_CODE_NO_VADDR_SPACE     = 3;
            constexpr uint16_t REASON_CODE_VM_ADD_FAIL        = 4;
            constexpr uint16_t REASON_CODE_IDX_OUT_OF_RANGE   = 5;
            constexpr uint16_t REASON_CODE_HEAP_ALREADY_EXISTS = 6;
            constexpr uint16_t REASON_CODE_HEAP_NOT_EXIST     = 7;
        }
    }
    // HCB_v3 使用位置码 7
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR_HCB_V3 = 7;
    namespace KPOOLMEMMGR_HCB_V3_EVENTS {
        constexpr uint8_t EVENT_CODE_ONLINE  = 0;
        constexpr uint8_t EVENT_CODE_OFFLINE = 1;
        constexpr uint8_t EVENT_CODE_ALLOC   = 2;
        constexpr uint8_t EVENT_CODE_FREE    = 3;
        constexpr uint8_t EVENT_CODE_REALLOC = 4;
        constexpr uint8_t EVENT_CODE_INTERNAL_ALLOC = 5;
        constexpr uint8_t EVENT_CODE_INTERNAL_FREE = 6;
        constexpr uint8_t EVENT_CODE_CLEAR = 7;
        namespace COMMON_FAIL_REASONS {//上界32
            constexpr uint16_t REASON_CODE_BAD_ADDR = 0;//非内核地址，不满足16B对齐
            constexpr uint16_t REASON_CODE_ADDR_NOT_THIS_HEAP = 1;
        }
        namespace COMMON_FATAL_REASONS {//上界32
            constexpr uint16_t REASON_CODE_METADATA_DESTROYED = 0;//非内核地址，不满足16B对齐
        }
        namespace INTERNAL_ALLOC_RESULTS {
            namespace FAIL_RESONS
            {
                constexpr uint16_t REASON_CODE_NO_AVALIABLE_BUDDY = 32;
            }
        }
        namespace ALLOC_RESULTS::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_SIZE_IS_ZERO       = 32;
            constexpr uint16_t REASON_CODE_SIZE_TOO_LARGE     = 33;//大于等于2mb时报错
        }
        namespace FREE_RESULTS::FATAL_REASONS {
            constexpr uint16_t DOUBLE_FREE_DETECT = 32;
            constexpr uint16_t MERGE_BUT_ALREADY_FREE = 34;
        }
    }
}

// ════════════════════════════════════════════════════════════════
// HCB_v3 — BCB-based Heap Control Block (replaces HCB_v2)
//
// BCB order 0 = 32B (16B buddy_meta + 16B payload), max_order = 16
// 编译时链接: first_linekd_heap 的 bitmap/data 在 BSS, online() 在
// 内核入口尽早调用. 之后 new/delete 立即可用.
// ════════════════════════════════════════════════════════════════
class kpoolmemmgr_t {
#ifdef TEST_MODE
public:
#endif
    static constexpr uint32_t HCB_DEFAULT_SIZE     = 0x200000; // 2MB
    static constexpr uint32_t BYTES_PER_ORDER0      = 32;
    static constexpr uint8_t  MAX_ORDER             = 16;
    static constexpr uint8_t  PER_ORDER_CACHE_COUNT = 8;
    static constexpr uint8_t  PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2 = 0x4;
    static constexpr uint64_t MAGIC_ALLOCATED = 0xDEADBEEFCAFEBABEull;

public:
    // ── buddy_meta (16B, data_meta 替代) ──
    struct alignas(16) buddy_meta {
        uint32_t data_size;
        uint8_t  flags;        // alloc_flags_t 压缩
        uint64_t magic;        // MAGIC_ALLOCATED
    };
    static_assert(sizeof(buddy_meta) == 16, "buddy_meta must be 16 bytes");

    // ── HCB_v3 内部类 ──
#ifdef TEST_MODE
public:
#else
private:
#endif
    class mixed_bitmap_v2 : bitmap_t {
        uint8_t out_order = 0;
    public:
        using bitmap_t::bit_set;
        using bitmap_t::bit_get;
        mixed_bitmap_v2() = default;
        KURD_t online(vaddr_t bitmap_va, uint8_t out_order);
        KURD_t offline();
        uint64_t scan_free_block(uint8_t& order);
        void bit_set0(uint64_t offset, uint8_t order);
        void bit_set1(uint64_t offset, uint8_t order);
        bool bit_get(uint64_t offset, uint8_t order);
        // 用于 linktime_init 等已有现成物理页的场景（BSS数据，不需要分配物理页）
        void init_existing(vaddr_t bitmap_va, uint8_t out_order_val);
        phyaddr_t bitmap_pbase = 0;
        uint64_t  bitmap_allocated_size = 0;
    };

    struct BuddyCache {
        uint64_t entries[PER_ORDER_CACHE_COUNT];
        uint8_t  cursor = 0;
    };

#ifdef TEST_MODE
public:
#else
private:
#endif
    class HCB_v3 {
    public:
        bool valid = false;
        KURD_t online(uint32_t size, vaddr_t data_va, vaddr_t bitmap_va);
        KURD_t offline();
        KURD_t alloc(void*& addr, uint32_t size, alloc_flags_t flags);
        KURD_t free(void* ptr);
        KURD_t realloc(void*& ptr, uint32_t new_size, alloc_flags_t flags);
        KURD_t clear(void* ptr);
        bool is_addr_belong(void* addr) const;
        uint64_t used_bytes() const;
        bool is_full() const;
        // 扫描位图校验 order_free_count 一致性
        KURD_t flush_free_count();

        // ══ 只用于 first_linekd_heap 编译时链接 ══
        void linktime_init();

#ifdef TEST_MODE
        // ══ 用户态测试用 — 使用预分配的 mmap/malloc 内存初始化 HCB ══
        void test_init(vaddr_t data_va, vaddr_t bitmap_va, uint32_t size);
#endif

        // 统计
        uint64_t order_free_count[MAX_ORDER + 1] = {};
        uint64_t stat_alloc   = 0;
        uint64_t stat_free    = 0;
        uint64_t stat_alloc_fail = 0;
        uint64_t stat_coalesce   = 0;
        uint64_t stat_split      = 0;
        uint64_t stat_cache_hit  = 0;
        uint64_t stat_scan       = 0;

    private:
        friend class kpoolmemmgr_t;
        mixed_bitmap_v2 bcb_bitmap;
        BuddyCache      caches_[MAX_ORDER + 1];
        vaddr_t         vbase_  = 0;
        phyaddr_t       data_pbase = 0;
        uint32_t        total_size_ = 0;
        uint8_t         max_order_ = MAX_ORDER;

        // BCB core
        buddy_meta* meta_from_ptr(void* ptr) const;
        uint8_t     size_to_order(uint32_t size_with_meta) const;
        uint64_t    ptr_to_offset(void* ptr) const;
        KURD_t internal_alloc(uint64_t& out_offset, uint8_t order);
        KURD_t internal_free(uint64_t offset, uint8_t order);
        KURD_t internal_split(uint64_t offset, uint8_t from_order, uint8_t to_order);
        void   free_page_without_merge(uint64_t offset, uint8_t order);
        void   cache_insert(uint8_t order, uint64_t offset);
        bool   cache_pick(uint8_t order, uint64_t& out_offset);

        spintrylock_cpp_t hcb_lock;

        // KURD 位置级模板
        KURD_t kurd_default_success();
        KURD_t kurd_default_error();
        KURD_t kurd_default_fatal();
    };

    // ── 静态成员 ──
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();

    static bool     is_muli_heap_enabled;
    // first_linekd_heap: BSS 静态数据, 内核入口尽早 online().
    // 编译时 bitmap/data 在 BSS, 第一行代码即可 alloc.
    static HCB_v3   first_linekd_heap;
    static HCB_v3*  HCB_ARRAY;          // 动态分配, multi_heap_enable 后可用
    static spinrwlock_cpp_t HCB_ARRAY_lock;
    static KURD_t   alloc_heap(uint32_t idx);
    static KURD_t   free_heap(uint32_t idx);
    static HCB_v3*  find_hcb_by_address(void* ptr);
    static VM_DESC  heap_area;
    static VM_DESC  heap_area_bitmaps;

public:
    static void* kalloc(uint64_t size, KURD_t& no_succes_report,
                        alloc_flags_t flags = default_flags);
    static void* realloc(void* ptr, KURD_t& no_succes_report,
                         uint64_t size, alloc_flags_t flags = default_flags);
    static void  clear(void* ptr);
    static void Init();
    static KURD_t multi_heap_enable();
    static void   kfree(void* ptr);

    kpoolmemmgr_t() = default;
    ~kpoolmemmgr_t() = default;
};

extern "C" {
    void* __wrapped_heap_alloc(uint64_t size, KURD_t* kurd,
                               alloc_flags_t flags = default_flags);
    void  __wrapped_heap_free(void* addr);
    void* __wrapped_heap_realloc(void* addr, uint64_t size,
                                 KURD_t* kurd, alloc_flags_t flags);
}

// ── new/delete ──
void* operator new(size_t size);
void* operator new(size_t size, alloc_flags_t flags);
void* operator new[](size_t size);
void* operator new[](size_t size, alloc_flags_t flags);
void  operator delete(void* ptr) noexcept;
void  operator delete(void* ptr, size_t) noexcept;
void  operator delete[](void* ptr) noexcept;
void  operator delete[](void* ptr, size_t) noexcept;
void* operator new(size_t, void* ptr) noexcept;
void* operator new[](size_t, void* ptr) noexcept;
