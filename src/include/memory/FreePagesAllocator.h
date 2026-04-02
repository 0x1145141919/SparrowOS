#pragma once
#include "stdint.h"
#include "memmodule_err_definitions.h"
#include "memory/memory_base.h"
#include "util/lock.h"
#include "util/Ktemplats.h"
#include "util/bitmap.h"
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
namespace MEMMODULE_LOCAIONS{
    constexpr uint8_t LOCATION_CODE_FREEPAGES_ALLOCATOR=28;
    
    constexpr uint8_t LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK=32;
    namespace FREEPAGES_ALLOCATOR{
        constexpr uint8_t EVENT_CODE_INIT= 0;
        constexpr uint8_t EVENT_CODE_INIT_SECOND_STAGE = 1;
        constexpr uint8_t EVENT_CODE_ALLOC = 2;
        namespace INIT_SECOND_STAGE_RESULTS_CODE{
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t FAIL_NO_AVALIABLE_MEM= 1;
                constexpr uint16_t FAIL_THREAD_COUNT_ZERO= 2;
            }
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_BAD_PARAM_MATCH_THREAD_BUT_BAD_ZERO_CONEFFICIENCY= 1;
            }

        }
        namespace ALLOC_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_SIZE = 1;
                constexpr uint16_t FAIL_REASON_CODE_NUMA_NOT_SUPPORTED = 2;
                constexpr uint16_t FAIL_REASON_CODE_INVALID_CONSTRAIN = 3;
                constexpr uint16_t FAIL_REASON_CODE_NO_MATCHED_BCB = 4;
                constexpr uint16_t FAIL_REASON_CODE_NO_AVALIABLE_BCB = 5;
            }
            namespace RETRY_REASONS_CODE{
                constexpr uint16_t RETRY_REASON_CODE_TIME_OUT = 1;
            }
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t UNREACHABLE_CODE = 1;
            }
        }
        constexpr uint8_t EVENT_CODE_FREE = 3;
        namespace FREE_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr  uint16_t FAIL_REASON_CODE_BASE_NOT_BELONG=1;
            }
        }
    }
   namespace FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES{ 

        constexpr uint8_t EVENT_CODE_INIT= 0;
        constexpr uint8_t EVENT_CODE_ALLOCATE_BUDY_WAY = 1;
        namespace ALLOCATE_RESULTS_CODE{ 
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_ACQUIRE_SIZE_TO_BIG= 1;
                constexpr uint16_t FAIL_REASON_CODE_NO_AVALIABLE_BUDDY= 2;
            }
        }
        constexpr uint8_t EVENT_CODE_CONANICO_FREE = 2;
        namespace CONANICO_FREE_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_PAGE_INDEX= 1;
                constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER= 2;
                constexpr uint16_t FAIL_REASON_CODE_COALESCING_FAILED= 3;
                constexpr uint16_t FAIL_REASON_CODE_DOUBLE_FREE= 5;
            }
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t BIN_TREE_CONSISTENCY_VIOLATION=1;
            }
        }
        constexpr uint8_t EVENT_CODE_SPLIT_PAGE = 3;
        namespace SPLIT_PAGE_RESULTS_CODE{ 
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER= 1;
                constexpr uint16_t FAIL_REASON_CODE_PAGE_NOT_FREE= 2;
            }
        }
        constexpr uint8_t EVENT_CODE_FLUSH_FREE_COUNT = 4;
        namespace FLUSH_FREE_COUNT_RESULTS_CODE{
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t COSISTENCY_VIOLATION=1; 
            }
        }
        constexpr uint8_t EVENT_CODE_TOP_FOLD = 5;
        constexpr uint8_t EVENT_CODE_FREE_PAGES_FLUSH = 6;
        namespace FREE_PAGES_FLUSH_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_ADDR_NOT_BELONG=1;
            }
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t COSISTENCY_VIOLATION=1; 
            }
        }
        constexpr uint8_t EVENT_CODE_FREE=7;
        namespace FREE_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_BASE_NOT_BELONG= 1;
            }
        }
        constexpr uint8_t EVENT_CODE_REPLAY_VALIDATE=8;
        namespace REPLAY_VALIDATE_RESULTS_CODE{
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_ORDER=1;
                constexpr uint16_t FAIL_REASON_CODE_INVALID_INDEX=2;
                constexpr uint16_t FAIL_REASON_CODE_INTERNAL_CONFLICT=3;
                constexpr uint16_t FAIL_REASON_CODE_PARENT_FREE_CHILD_USED=4;
                constexpr uint16_t FAIL_REASON_CODE_PARENT_USED_CHILD_USED=5;
                constexpr uint16_t FAIL_REASON_CODE_INTERNAL_BITMAP_MISSING=6;
            }
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
    struct flags_t {
        uint64_t allow_new_BCB : 1;
    };

public:
    static constexpr uint16_t _4KB_PAGESIZE = 4096;
    static flags_t flags;

    class BuddyControlBlock {
    private:
        static constexpr uint64_t INVALID_INBCB_INDEX = ~0;
        static constexpr uint8_t DESINGED_MAX_SUPPORT_ORDER = 24;
        static constexpr uint8_t PER_ORDER_CACHE_SUGGEST_COUNT = 8;
        using cache_order_suggest_t = uint64_t[PER_ORDER_CACHE_SUGGEST_COUNT];
        
        bool is_splited_bitmap_valid;
        uint8_t max_supprt_order;
        phyaddr_t base; // base 至少是 4KB 对齐的，可以不是 1<<(MAX_SUPPORT_ORDER+12) 对齐的，只会影响外部获得的地址
        KURD_t default_kurd();
        KURD_t default_success();
        KURD_t default_error();
        KURD_t default_fatal();

        cache_order_suggest_t suggest_order_free_page_index[DESINGED_MAX_SUPPORT_ORDER]; // 每个 order 多条缓存，基于 BCB 开始地址的索引
        uint8_t suggest_order_cache_cursor[DESINGED_MAX_SUPPORT_ORDER];
        void cache_insert(uint8_t order, uint64_t idx);
        bool cache_pick(uint8_t order, uint64_t& out_idx);
        KURD_t conanico_free(
            uint64_t in_bcb_idx,
            uint8_t order
        );
        void free_page_without_merge( // 需要事件码
            uint64_t in_bcb_idx,
            uint8_t order
        );

        class mixed_bitmap_t : bitmap_t {
        private:
            uint64_t entry_count;
            KURD_t default_kurd();
            KURD_t default_success();
            KURD_t default_error();
            KURD_t default_fatal();

        public:
            using bitmap_t::bit_set;
            using bitmap_t::bit_get;
            mixed_bitmap_t(uint64_t entry_count,vaddr_t base_addr);
            void mixedbitmap_base_specify(vaddr_t bitmap_base_addr = 0);
            uint64_t find_free_in_interval(
                uint64_t start_idx,
                uint64_t interval_length
            );
            ~mixed_bitmap_t();
        };

#ifdef REPALY_MODE
        void replay_internal_init();
        void replay_internal_mark_split(uint8_t order, uint64_t idx);
        void replay_internal_mark_free(uint8_t order, uint64_t idx);
        KURD_t replay_validate_node(uint8_t order, uint64_t idx, const char* tag);

        Ktemplats::kernel_bitmap* order_Internal_bitmap[DESINGED_MAX_SUPPORT_ORDER];
#endif
        friend mixed_bitmap_t::mixed_bitmap_t(uint64_t entry_count,vaddr_t base_addr);
        mixed_bitmap_t* order_freepage_existency_bitmaps; // 这个位图编码为 1 表示这个 order 的对应引索的页面是存在的，反之不存在
        uint64_t order_bases[DESINGED_MAX_SUPPORT_ORDER]; // 在 mixed_bitmap_t 里面各 order 的引索基址

        struct BCB_statistics {
            uint64_t free_count[DESINGED_MAX_SUPPORT_ORDER];
            uint64_t suggest_hit[DESINGED_MAX_SUPPORT_ORDER];
            uint64_t suggest_miss[DESINGED_MAX_SUPPORT_ORDER];
            uint64_t alloc_times_success;
            uint64_t free_times_success;
            uint64_t alloc_times_fail;
            uint64_t scan_count;
            uint64_t fold_count_success;
            uint64_t fold_count_fail;
            uint64_t split_count;
        } statistics;

        KURD_t split_page(
            uint64_t splited_idx,
            uint8_t splited_order,
            uint8_t target_order
        );
        static uint8_t size_to_order(uint64_t size);
        bool is_reclusive_fold_success(uint64_t idx, uint8_t order); // true 成功，false 失败，是对这个 order 之下的所有二叉树进行折叠
        bool is_addr_belong_to_this_BCB_no_lock(phyaddr_t addr);
        void print_basic_info_no_lock();
        void print_bitmap_info_no_lock();
        void print_bitmap_order_info_compress_no_lock(uint8_t order);
        void print_bitmap_order_interval_compress_no_lock(uint8_t order, uint64_t base, uint64_t length);
#ifdef REPALY_MODE
        KURD_t replay_validate_tree_no_lock(const char* tag);
#endif

    public:
        bool is_bcb_avaliable();
        uint8_t get_max_order();
        friend all_pages_arr;
        void print_basic_info();
        void print_bitmap_info();
        void print_all_statistics();  // 新增：打印全部统计信息
        void print_bitmap_order_info_compress(uint8_t order);
        void print_bitmap_order_interval_compress(uint8_t order, uint64_t base, uint64_t length);
        KURD_t free_pages_flush(); // 强制扫描位图校准 free_count[DESINGED_MAX_SUPPORT_ORDER] 数据结构
        BuddyControlBlock(
            phyaddr_t base,
            uint8_t max_support_order
        );
        BuddyControlBlock();
        void corebcb_mixedbitmap_base_acclaim(vaddr_t bitmap_base_addr = 0);
        phyaddr_t allocate_buddy_way(
            uint64_t size,
            KURD_t& result,
            uint8_t align_log2=0
        );
        phyaddr_t get_base();
        uint8_t get_order();
        void top_fold();
        KURD_t free_buddy_way(
            phyaddr_t base,
            uint64_t size
        );
#ifdef REPALY_MODE
        KURD_t replay_validate_tree(const char* tag);
#endif
        bool is_addr_belong_to_this_BCB(phyaddr_t addr);
        bool can_alloc(uint8_t order);
        spintrylock_cpp_t lock;
        ~BuddyControlBlock() = default;
    };
    friend KspacePageTable;
    static uint64_t BCB_count;
    static BuddyControlBlock*BCBS;
    static all_pages_arr::free_segs_t* memory_crumbs;
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
    static KURD_t second_stage(strategy_t strategy);
    static KURD_t Init(strategy_t strategy,loaded_VM_interval* VM_intervals_bcbs_bitmap);
    static all_pages_arr::free_segs_t* get_memory_crumbs();
    static phyaddr_t alloc(uint64_t size, buddy_alloc_params params,page_state_t interval_type,KURD_t&kurd);//params只有在
    static KURD_t free(phyaddr_t base, uint64_t size);
    static fpa_stats get_fpa_stats();//当前本地 CPU 的统计数据，必须在 second_stage 初始化完成后才可以调用，否则行为未定义
    static fpa_stats get_fpa_stats(uint64_t pid);//pid 为处理器 id，必须在 second_stage 初始化完成后才可以调用，否则行为未定义，不提供锁保护
    static fpa_stats get_fpa_stats_all();//所有统计信息的总计，除 bcb_scan_max 是取最大，其他字段是求和，不在锁保护下
    static constexpr uint64_t INVALID_ALLOC_BASE = ~0ULL;
    // 打印所有 BCB 的完整统计信息
    static void print_all_bcb_statistics();
};
extern spinlock_interrupt_about_cpp_t FPA_modify;
