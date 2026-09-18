#pragma once
#include "stdint.h"
#include "memmodule_err_definitions.h"
#include "memory/memory_base.h"
#include "util/lock.h"
#include "util/Ktemplats.h"
#include "util/BCB_fnd_DeepFirst.h"
#include "memory/all_pages_arr.h"
class KspacePageTable;
struct fpa_stats {
    uint64_t alloc_count;
    uint64_t bcb_scan_total;
    uint64_t bcb_scan_max;//一次alloc中最高扫描BCB数量
    uint64_t alloc_fail;
    uint64_t lock_try_fail;
    uint64_t free_count;
};

namespace MEMMODULE_LOCATIONS{
    constexpr uint8_t LOCATION_CODE_FREEPAGES_ALLOCATOR=28;
    
    constexpr uint8_t LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK=32;

    namespace FREEPAGES_ALLOCATOR{
        constexpr uint8_t EVENT_CODE_INIT= 0;
        constexpr uint8_t EVENT_CODE_INIT_SECOND_STAGE = 1;
        constexpr uint8_t EVENT_CODE_ALLOC = 2;
        constexpr uint8_t EVENT_CODE_FREE = 3;

        namespace COMMON_FAIL_REASONS { }     // [0x00, 0x100)
        namespace COMMON_FATAL_REASONS { }    // [0x00, 0x100)
        namespace COMMON_RETRY_REASONS { }    // [0x00, 0x100)

        namespace init_results::FATAL_REASONS {
            constexpr uint16_t FAIL_NO_AVALIABLE_MEM          = 0x100;
            constexpr uint16_t FAIL_THREAD_COUNT_ZERO          = 0x101;
        }
        namespace init_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_BAD_PARAM_MATCH_THREAD_BUT_BAD_ZERO_CONEFFICIENCY = 0x100;
        }
        namespace alloc_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_SIZE     = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_NUMA_NOT_SUPPORTED = 0x101;
            constexpr uint16_t FAIL_REASON_CODE_INVALID_CONSTRAIN  = 0x102;
            constexpr uint16_t FAIL_REASON_CODE_NO_MATCHED_BCB    = 0x103;
            constexpr uint16_t FAIL_REASON_CODE_NO_AVALIABLE_BCB  = 0x104;
        }
        namespace alloc_results::RETRY_REASONS {
            constexpr uint16_t RETRY_REASON_CODE_TIME_OUT        = 0x100;
        }
        namespace alloc_results::FATAL_REASONS {
            constexpr uint16_t UNREACHABLE_CODE                  = 0x100;
        }
        namespace free_results::FAIL_REASONS {
            constexpr  uint16_t FAIL_REASON_CODE_BASE_NOT_BELONG = 0x100;
        }
        namespace call_violation_results::FATAL_REASONS {
            constexpr uint16_t CALL_VIOLATION                    = 0x100;
        }
    }

   namespace FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES{ 

        constexpr uint8_t EVENT_CODE_INIT= 0;
        constexpr uint8_t EVENT_CODE_ALLOCATE_BUDY_WAY = 1;
        constexpr uint8_t EVENT_CODE_CONANICO_FREE = 2;
        constexpr uint8_t EVENT_CODE_SPLIT_PAGE = 3;
        constexpr uint8_t EVENT_CODE_FLUSH_FREE_COUNT = 4;
        constexpr uint8_t EVENT_CODE_TOP_FOLD = 5;
        constexpr uint8_t EVENT_CODE_FREE_PAGES_FLUSH = 6;
        constexpr uint8_t EVENT_CODE_FREE=7;
        constexpr uint8_t EVENT_CODE_REPLAY_VALIDATE=8;

        namespace COMMON_FAIL_REASONS {}      // [0x00, 0x100)
        namespace COMMON_FATAL_REASONS {}     // [0x00, 0x100)

        namespace allocate_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_ACQUIRE_SIZE_TO_BIG = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_NO_AVALIABLE_BUDDY  = 0x101;
        }
        namespace conanico_free_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_PAGE_INDEX = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER      = 0x101;
            constexpr uint16_t FAIL_REASON_CODE_COALESCING_FAILED  = 0x102;
            constexpr uint16_t FAIL_REASON_CODE_DOUBLE_FREE        = 0x103;
        }
        namespace conanico_free_results::FATAL_REASONS {
            constexpr uint16_t BIN_TREE_CONSISTENCY_VIOLATION      = 0x100;
        }
        namespace split_page_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER    = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_PAGE_NOT_FREE    = 0x101;
        }
        namespace flush_free_count_results::FATAL_REASONS {
            constexpr uint16_t COSISTENCY_VIOLATION               = 0x100;
        }
        namespace free_pages_flush_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_ADDR_NOT_BELONG        = 0x100;
        }
        namespace free_pages_flush_results::FATAL_REASONS {
            constexpr uint16_t COSISTENCY_VIOLATION               = 0x100;
        }
        namespace free_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_BASE_NOT_BELONG   = 0x100;
        }
        namespace replay_validate_results::FATAL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER     = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_INVALID_INDEX     = 0x101;
            constexpr uint16_t FAIL_REASON_CODE_INTERNAL_CONFLICT = 0x102;
            constexpr uint16_t FAIL_REASON_CODE_PARENT_FREE_CHILD_USED = 0x103;
            constexpr uint16_t FAIL_REASON_CODE_PARENT_USED_CHILD_USED = 0x104;
            constexpr uint16_t FAIL_REASON_CODE_INTERNAL_BITMAP_MISSING = 0x105;
        }
    }
    constexpr uint8_t LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_BITMAP=33;
    namespace FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_BITMAP{
        constexpr uint8_t EVENT_CODE_INIT=0;
    }
};
struct buddy_alloc_params;
struct Alloc_result{
    uint64_t base;
    KURD_t result;
};
enum second_stage_init_strategy{
    INIT_STRATEGY_BEST_ALIGN_FIT=0,
    INIT_STRATEGY_MATCH_THREAD=1
};
class FreePagesAllocator {
public:
    static constexpr uint16_t _4KB_PAGESIZE = 4096;

    class BuddyControlBlock {
    private:
        static constexpr uint64_t INVALID_INBCB_INDEX = ~0;
        static constexpr uint8_t DESINGED_MAX_SUPPORT_ORDER = 25;
        static constexpr uint8_t PER_ORDER_CACHE_SUGGEST_COUNT = 8;
        using cache_order_suggest_t = uint64_t[PER_ORDER_CACHE_SUGGEST_COUNT];

        uint8_t max_supprt_order;
        phyaddr_t base;
        // 本 BCB 页基址在 page_frame_state_mgr::pages_arr 中的条目下标。
        // 由构造函数经 phyaddr_to_page_idx(base) 计算（base 必落在某 freeSystemRam 区间内）。
        // alloc/free 据此用 idx_base_* 接口同步账本（O(1) 直写，免区间二分）。
        uint64_t pages_arr_base_idx;
        KURD_t default_kurd();
        KURD_t default_success();
        KURD_t default_error();
        KURD_t default_fatal();

        // ── BCB 级缓存（在底座之上） ──
        cache_order_suggest_t suggest_order_free_page_index[DESINGED_MAX_SUPPORT_ORDER];
        uint8_t suggest_order_cache_cursor[DESINGED_MAX_SUPPORT_ORDER];
        void cache_insert(uint8_t order, uint64_t idx);
        bool cache_pick(uint8_t order, uint64_t& out_idx);

        // ── v4 伙伴系统底座 ──
        BCB_fnd_DeepFirst fnd;

        struct BCB_statistics {
            uint64_t suggest_hit[DESINGED_MAX_SUPPORT_ORDER];
            uint64_t suggest_miss[DESINGED_MAX_SUPPORT_ORDER];
            uint64_t alloc_times_success;
            uint64_t free_times_success;
            uint64_t alloc_times_fail;
            uint64_t scan_count;
            uint64_t split_count;
        } statistics;

        static uint8_t size_to_order(uint64_t size);
        bool is_addr_belong_to_this_BCB_no_lock(phyaddr_t addr);
        void print_basic_info_no_lock();
        void print_bitmap_info_no_lock();
        void print_bitmap_order_info_compress_no_lock(uint8_t order);
        void print_bitmap_order_interval_compress_no_lock(uint8_t order, uint64_t base, uint64_t length);

    public:
        bool is_bcb_avaliable();
        uint8_t get_max_order();
        friend all_pages_arr;
        void print_basic_info();
        void print_bitmap_info();
        void print_all_statistics();
        void print_bitmap_order_info_compress(uint8_t order);
        void print_bitmap_order_interval_compress(uint8_t order, uint64_t base, uint64_t length);
        KURD_t free_pages_flush();  // → fnd.btree_validation()
        BuddyControlBlock(
            phyaddr_t base,
            vaddr_t bitmap_vbase,
            uint8_t max_support_order
        );
        BuddyControlBlock();
        // ── 成年态专用分配/释放：仅 ADULT 态可调，违规即 error ──
        // （幼年态分配/释放请走 juvenile_alloc / juvenile_free）
        phyaddr_t allocate_buddy_way(
            uint64_t size,
            KURD_t& result,
            uint8_t align_log2=0
        );
        phyaddr_t get_base();
        uint8_t get_order();
        uint64_t get_pages_arr_base_idx() const { return pages_arr_base_idx; }
        KURD_t free_buddy_way(
            phyaddr_t base,
            uint64_t size
        );
        bool is_addr_belong_to_this_BCB(phyaddr_t addr);
        bool can_alloc(uint8_t order);
        spintrylock_cpp_t lock;
        ~BuddyControlBlock() = default;
    };
    friend KspacePageTable;
    static uint64_t BCB_count;
    static BuddyControlBlock*BCBS;
    static fpa_stats*statistics_arr;
    static uint64_t*processors_preffered_bcb_idx;//也是只有后
public:
    struct strategy_t{
        second_stage_init_strategy strategy;
        uint64_t thread_coefficient;//当 strategy 是 MATCH_THREAD 时有效，表示每个线程建议的主BCB个数
    };
    static constexpr strategy_t BEST_FIT = {
        .strategy = INIT_STRATEGY_BEST_ALIGN_FIT,
        .thread_coefficient = 1
    };
    static constexpr strategy_t DEFAULT_THREAD = {
        .strategy = INIT_STRATEGY_MATCH_THREAD,
        .thread_coefficient = 1
    };
    private:
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_error();
    static KURD_t default_fatal();
    static KURD_t default_retry();
    public:
    static KURD_t Init(strategy_t strategy,vm_interval* VM_intervals_bcbs_bitmap);
    static phyaddr_t alloc(uint64_t size, buddy_alloc_params params,page_state_t interval_type,KURD_t&kurd);//params只有在
    static KURD_t free(phyaddr_t base, uint64_t size);
    static fpa_stats get_fpa_stats();//当前本地 CPU 的统计数据，必须在 second_stage 初始化完成后才可以调用，否则行为未定义
    static fpa_stats get_fpa_stats(uint64_t pid);//pid 为处理器 id，必须在 second_stage 初始化完成后才可以调用，否则行为未定义，不提供锁保护
    static fpa_stats get_fpa_stats_all();//所有统计信息的总计，除 bcb_scan_max 是取最大，其他字段是求和，不在锁保护下
    // 总 FPA 预算：Init 时累计的全部可用内存字节数（g_all_avaliable_mem_accumulate）。
    // 供测试水位线（如 MMU 压测取 75%）使用；必须在 Init 完成后调用。
    static uint64_t get_total_budget_bytes();
    static constexpr uint64_t INVALID_ALLOC_BASE = ~0ULL;
    // 打印所有 BCB 的完整统计信息
    static void print_all_bcb_statistics();
};
extern spinlock_cpp_t FPA_modify;
