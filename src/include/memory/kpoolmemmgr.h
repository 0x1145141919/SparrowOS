#pragma once
#include "stdint.h"
#include "memmodule_err_definitions.h"
#include "abi/boot.h"
#include <util/lock.h>
#include "memory/memory_base.h"

typedef uint64_t size_t;
typedef uint64_t phyaddr_t;
typedef uint64_t vaddr_t;

namespace MEMMODULE_LOCATIONS {
    // kpoolmemmgr facade 使用位置码 4
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR = 4;
    namespace KPOOLMEMMGR_EVENTS {
        constexpr uint8_t EVENT_CODE_INIT = 0;
        constexpr uint8_t EVENT_CODE_ALLOC = 1;
        constexpr uint8_t EVENT_CODE_REALLOC = 2;
        constexpr uint8_t EVENT_CODE_PER_PROCESSOR_HEAP_INIT = 3;

        namespace COMMON_FAIL_REASONS {}
        namespace COMMON_FATAL_REASONS {}

        namespace alloc_results::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_NO_AVALIABLE_MEM = 0x04;
            constexpr uint16_t REASON_CODE_SIZE_IS_ZERO     = 0x05;
        }
        namespace realloc_results::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_DEMAND_SIZE_IS_ZERO  = 0x04;
            constexpr uint16_t REASON_CODE_PTR_NOT_IN_ANY_HEAP  = 0x05;
            constexpr uint16_t REASON_CODE_NO_AVALIABLE_MEM     = 0x06;
        }
        namespace per_processor_heap_init_results::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_ALREADY_ENABLED       = 0x01;
            constexpr uint16_t REASON_CODE_BAD_PROCESSOR_COUNT   = 0x02;
            constexpr uint16_t REASON_CODE_NO_VADDR_SPACE        = 0x03;
            constexpr uint16_t REASON_CODE_VM_ADD_FAIL           = 0x04;
            constexpr uint16_t REASON_CODE_IDX_OUT_OF_RANGE      = 0x05;
            constexpr uint16_t REASON_CODE_HEAP_ALREADY_EXISTS   = 0x06;
            constexpr uint16_t REASON_CODE_HEAP_NOT_EXIST        = 0x07;
        }
    }

    // HCB_v2.1 使用位置码 7（替换原 v3）
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR_HCB = 7;
    namespace KPOOLMEMMGR_HCB_EVENTS {
        constexpr uint8_t EVENT_CODE_LINKTIME_INIT = 0;
        constexpr uint8_t EVENT_CODE_ONLINE        = 1;
        constexpr uint8_t EVENT_CODE_OFFLINE       = 2;
        constexpr uint8_t EVENT_CODE_ALLOC         = 3;
        constexpr uint8_t EVENT_CODE_FREE          = 4;
        constexpr uint8_t EVENT_CODE_REALLOC       = 5;
        constexpr uint8_t EVENT_CODE_CLEAR         = 6;

        // 公共原因 [0x00, 0x100)
        namespace COMMON_FAIL_REASONS {
            constexpr uint16_t REASON_CODE_BAD_ADDR           = 0x00;
            constexpr uint16_t REASON_CODE_ADDR_NOT_THIS_HEAP = 0x01;
            constexpr uint16_t REASON_CODE_HEAP_NOT_ONLINE    = 0x02;
        }
        namespace COMMON_FATAL_REASONS {
            constexpr uint16_t REASON_CODE_METADATA_DESTROYED = 0x00;
        }

        // 事件私有原因 [0x100, 0xFFF)
        namespace alloc_results::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_SIZE_IS_ZERO       = 0x100;
            constexpr uint16_t REASON_CODE_SIZE_TOO_LARGE     = 0x101;
            constexpr uint16_t REASON_CODE_NO_AVAILABLE_BLOCK = 0x102;
        }
        namespace free_results::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_DOUBLE_FREE_DETECT   = 0x100;
            constexpr uint16_t REASON_CODE_CANARY_CORRUPTED     = 0x101;
        }
        namespace online_results::FAIL_REASONS {
            constexpr uint16_t REASON_CODE_ALREADY_ONLINE       = 0x100;
        }
    }
}

// ════════════════════════════════════════════════════════════════
// HCB_v2.1 — Flat bitmap Heap Control Block
//
// 替换 HCB_v3 (buddy tree):
//   - 8B/bit flat bitmap (vs 3×2^N buddy tree)
//   - 8B data_meta (vs 16B buddy_meta)
//   - next-fit scan + scan_cache (vs. per-order buddy cache)
//   - 0xFF canary 越界检测 (vs. MAGIC magic value)
//   - 精确 used_bytes / entry_count 统计
// ════════════════════════════════════════════════════════════════

class kpoolmemmgr_t {
#ifdef TEST_MODE
public:
#endif
    static constexpr uint32_t HCB_DEFAULT_SIZE = 0x200000; // 2MB
    static constexpr uint8_t  BYTES_PER_BIT    = 8;         // bitmap 1 bit = 8 bytes
    static constexpr uint8_t  PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2 = 0x4; // 16 heaps/CPU

    // ── data_meta (8B) ──
    struct alignas(8) data_meta {
        uint32_t data_size;      // 4B, 用户请求大小; 0 = 已释放
        uint8_t  alloc_flags;    // 1B, 编码 alloc_flags_t 位域
        uint8_t  magic;          // 1B, debug 保留
        uint16_t align_pad_bits; // 2B, 对齐浪费的 bitmap bits (free 时用于归还完整 run)
    };
    static_assert(sizeof(data_meta) == 8, "data_meta must be 8 bytes");

    // ── ScanCache (取代 per-order buddy cache) ──
    struct ScanCache {
        uint64_t hint_u64_idx      = 0;  // 下一个待扫 64-bit word
        uint64_t last_success_idx  = 0;  // next-fit 起点 (bit index)
        uint64_t largest_free_hint = 0;  // 最长连续空闲 bits 缓存
        uint64_t free_bit_count    = 0;  // 总空闲 bits
        uint64_t entry_count       = 0;  // 活跃分配数
        uint64_t used_bytes_acc    = 0;  // 用户 payload 累计
    };

#ifdef TEST_MODE
public:
#else
private:
#endif
    class HCB_v2 {
    public:
        bool valid = false;
        spintrylock_cpp_t hcb_lock;

        // ── 生命周期 ──
        KURD_t online(uint32_t size, vaddr_t data_va, vaddr_t bitmap_va);
        KURD_t offline();
        void   linktime_init();

        // ── 分配器接口 ──
        KURD_t alloc(void*& addr, uint32_t size, alloc_flags_t flags);
        KURD_t free(void* ptr);
        KURD_t realloc(void*& ptr, uint32_t new_size, alloc_flags_t flags);
        KURD_t clear(void* ptr);

        // ── 查询 ──
        bool     is_addr_belong(void* addr) const;
        uint64_t used_bytes() const;
        bool     is_full() const;
        KURD_t   flush_free_count();

#ifdef TEST_MODE
        void test_init(vaddr_t data_va, vaddr_t bitmap_va, uint32_t size);
#endif

        // ── 统计 ──
        uint64_t stat_alloc       = 0;
        uint64_t stat_free        = 0;
        uint64_t stat_alloc_fail  = 0;
        uint64_t stat_oom         = 0;  // 内存耗尽次数
        uint64_t stat_realloc_expand = 0;  // 原地扩展次数
        uint64_t stat_scan_fail   = 0;  // 扫描 round 数

    private:
        friend class kpoolmemmgr_t;

        vaddr_t   vbase_     = 0;
        phyaddr_t data_pbase = 0;
        uint32_t  total_size_ = 0;
        uint32_t  total_bits_ = 0;     // total_size_ / BYTES_PER_BIT

        vaddr_t   bitmap_va_ = 0;
        phyaddr_t bitmap_pbase = 0;
        uint32_t  bitmap_bytes_ = 0;

        ScanCache cache_;

        // ── bitmap 操作 ──
        uint64_t  bit_idx_from_ptr(void* ptr) const;
        bool      bitmap_test(uint64_t bit_idx) const;
        void      bitmap_set(uint64_t bit_idx, uint64_t count);
        void      bitmap_clear(uint64_t bit_idx, uint64_t count);
        bool      bitmap_scan_from(uint64_t start_idx, uint64_t needed,
                                   uint64_t& out_idx, uint64_t& out_runlen);

        // ── 内部 ──
        data_meta* meta_from_ptr(void* ptr) const;
        void       fill_canary(uint8_t* user_ptr, uint32_t user_size,
                               uint64_t total_bits_alloced);
        void       check_canary(uint8_t* user_ptr, uint32_t user_size,
                                uint64_t total_bits_alloced);
        uint32_t   bits_needed(uint32_t user_size) const;

        // ── KURD 模板 ──
        static KURD_t kurd_default_success();
        static KURD_t kurd_default_error();
        static KURD_t kurd_default_fatal();
    };

    // ── 静态成员 ──
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();

    static bool    is_muli_heap_enabled;
    static HCB_v2  first_linekd_heap;
#ifdef TEST_MODE
public:
#endif
    static HCB_v2* HCB_ARRAY;
    static KURD_t alloc_heap(uint32_t idx);
    static KURD_t free_heap(uint32_t idx);
    static HCB_v2* find_hcb_by_address(void* ptr);
    static VM_DESC heap_area;
    static VM_DESC heap_area_bitmaps;

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
