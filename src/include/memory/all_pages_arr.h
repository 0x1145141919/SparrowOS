#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#include <util/lock.h>
#include <util/Ktemplats.h>
#include "memory/memmodule_err_definitions.h"
#include "abi/boot.h"
typedef  uint64_t phyaddr_t;
namespace MEMMODULE_LOCATIONS{
    constexpr uint8_t LOCATION_CODE_TRANSPARENT_PAGE=0x03;
    namespace TRANSPARENT_PAGE_EVENTS{
        constexpr uint8_t EVENT_CODE_SPLIT=0x01;
        constexpr uint8_t EVENT_CODE_MERGE=0x02;
        constexpr uint8_t EVENT_CODE_MERGE_FREE=0x03;

        namespace COMMON_FAIL_REASONS {}    // [0x00, 0x100)
        namespace COMMON_FATAL_REASONS {}   // [0x00, 0x100)

        namespace split_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_TARGET_ORDER    = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_TARGET_ORDER_NOT_SMALLER = 0x101;
            constexpr uint16_t FAIL_REASON_CODE_INDEX_ALIGN_TOO_SMALL   = 0x102;
            constexpr uint16_t NOT_HEAD_PAGE                            = 0x103;
            constexpr uint16_t FAIL_REASON_CODE_OUT_OF_RANGE            = 0x104;
        }
        namespace split_results::FATAL_REASONS {
            constexpr uint16_t CONSISTENCY_VIOLATION                    = 0x100;
        }
        namespace merge_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_TARGET_ORDER    = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_TARGET_ORDER_NOT_GREATER = 0x101;
            constexpr uint16_t FAIL_REASON_CODE_INDEX_ALIGN_TOO_SMALL   = 0x102;
            constexpr uint16_t FAIL_REASON_CODE_OUT_OF_RANGE            = 0x103;
            constexpr uint16_t FAIL_REASON_CODE_NOT_HEAD_PAGE           = 0x104;
            constexpr uint16_t FAIL_REASON_CODE_ORDER_MISMATCH          = 0x105;
            constexpr uint16_t FAIL_REASON_CODE_TYPE_MISMATCH           = 0x106;
            constexpr uint16_t FAIL_REASON_CODE_TRANSPARENT_PAGE_INVALID = 0x107;
            constexpr uint16_t FAIL_REASON_CODE_HEAD_PTR_MISMATCH       = 0x108;
            constexpr uint16_t FAIL_REASON_CODE_HUGE_ORDER_MISMATCH     = 0x109;
        }
        namespace merge_free_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INVALID_TARGET_ORDER    = 0x100;
            constexpr uint16_t FAIL_REASON_CODE_TARGET_ORDER_NOT_GREATER = 0x101;
            constexpr uint16_t FAIL_REASON_CODE_INDEX_ALIGN_TOO_SMALL   = 0x102;
            constexpr uint16_t FAIL_REASON_CODE_OUT_OF_RANGE            = 0x103;
            constexpr uint16_t FAIL_REASON_CODE_NOT_HEAD_PAGE           = 0x104;
            constexpr uint16_t FAIL_REASON_CODE_ORDER_MISMATCH          = 0x105;
            constexpr uint16_t FAIL_REASON_CODE_NOT_FREE                = 0x106;
            constexpr uint16_t FAIL_REASON_CODE_NOT_ALLOCATABLE         = 0x107;
            constexpr uint16_t FAIL_REASON_CODE_REFCOUNT_NONZERO        = 0x108;
            constexpr uint16_t FAIL_REASON_CODE_TRANSPARENT_PAGE_INVALID = 0x109;
            constexpr uint16_t FAIL_REASON_CODE_HEAD_PTR_MISMATCH       = 0x10A;
            constexpr uint16_t FAIL_REASON_CODE_HUGE_ORDER_MISMATCH     = 0x10B;
        }
    };
    constexpr uint8_t LOCATION_CODE_PAGES_ARR=0x04;
    namespace PAGES_ARR_EVENTS{
        constexpr uint8_t EVENT_CODE_SIMP_PAGES_SET=0x01;

        namespace COMMON_FAIL_REASONS {}    // [0x00, 0x100)
        namespace COMMON_FATAL_REASONS {}   // [0x00, 0x100)

        namespace simp_pages_set_results::FAIL_REASONS {
            constexpr uint16_t FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM = 0x100;
        }
    };

};
/**
 * phymemspace_mgr权责约定：
 * 向外暴露的数据结构中pages_array_2mb保证在初始化逻辑中后可以不越界的情况下自由读写不产生页错误
 * 
 */
class all_pages_arr{
    
    static uint64_t mem_map_entry_count;
    static page*mem_map;
    struct phyinterval_t{
        uint64_t base;
        uint64_t numof4kbpgs;
        uint64_t baseidx_in_memmap;
    };
    static phyinterval_t*mem_map_intervals;
    static uint64_t mem_map_intervals_count;
    public:

    struct free_segs_t{
        uint64_t count;
        struct entry_t{ 
            phyaddr_t base;
            uint64_t size;
        };
        entry_t*entries;
    };
    
    static free_segs_t*free_segs_get();
    static void subtb_alloc_shift_pages_way();    
    static KURD_t Init(vm_interval* pages_arr_interval);
    page* operator[](phyaddr_t phyaddr);
    /**
     * 设置内部的类的页面类型为TYPE，refcoutn为1,没有其它任何隐式行为
     */
    static KURD_t simp_pages_set(phyaddr_t phybase,uint64_t _4kbpgscount,page_state_t TYPE); 
    #ifdef USER_MODE
    all_pages_arr();
    #endif
};
extern all_pages_arr dram_map;
extern "C"{
    void* __wrapped_pgs_valloc(KURD_t*kurd_out,uint64_t _4kbpgscount, page_state_t TYPE, uint8_t alignment_log2);
    KURD_t __wrapped_pgs_vfree(void*vbase,uint64_t _4kbpgscount);
    vaddr_t stack_alloc(KURD_t*kurd_out,uint64_t _4kbpgscount);//专用栈分配接口，_4kbpgscount = 可用页数（不含guard page）
    //返回 priv_stack_base（栈顶，4K对齐），[base-4K, base)为guard page，[base, base+4K*pages)为可用栈空间
}
