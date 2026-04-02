#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#include "memory/page_struct.h"
#include <util/lock.h>
#include <util/Ktemplats.h>
#include "memory/memmodule_err_definitions.h"
#include "abi/boot.h"
typedef  uint64_t phyaddr_t;
namespace MEMMODULE_LOCAIONS{
    constexpr uint8_t LOCATION_CODE_TRANSPARNENT_PAGE=0x3;
    namespace TRANSPARNENT_PAGE_EVENTS{
        constexpr uint8_t EVENT_CODE_SPILT=0x1;
        constexpr uint8_t EVENT_CODE_MERGE=0x2;
        constexpr uint8_t EVENT_CODE_MERGE_FREE=0x3;
        namespace SPILT_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_TARGET_ORDER=1;
                constexpr uint16_t FAIL_REASON_CODE_TARGET_ORDER_NOT_SMALLER=2;
                constexpr uint16_t FAIL_REASON_CODE_INDEX_ALIGN_TOO_SMALL=3;
                constexpr uint16_t NOT_HEAD_PAGE=4;
                constexpr uint16_t FAIL_REASON_CODE_OUT_OF_RANGE=5;
            }
            namespace FATAL_REASONS_CODE{
                constexpr uint16_t CONSISTENCY_VIOLATION=1;
            }
        }
        namespace MERGE_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_TARGET_ORDER=1;
                constexpr uint16_t FAIL_REASON_CODE_TARGET_ORDER_NOT_GREATER=2;
                constexpr uint16_t FAIL_REASON_CODE_INDEX_ALIGN_TOO_SMALL=3;
                constexpr uint16_t FAIL_REASON_CODE_OUT_OF_RANGE=4;
                constexpr uint16_t FAIL_REASON_CODE_NOT_HEAD_PAGE=5;
                constexpr uint16_t FAIL_REASON_CODE_ORDER_MISMATCH=6;
                constexpr uint16_t FAIL_REASON_CODE_TYPE_MISMATCH=7;
                constexpr uint16_t FAIL_REASON_CODE_TRANSPARENT_PAGE_INVALID=8;
                constexpr uint16_t FAIL_REASON_CODE_HEAD_PTR_MISMATCH=9;
                constexpr uint16_t FAIL_REASON_CODE_HUGE_ORDER_MISMATCH=10;
                constexpr uint16_t NOT_HEAD_PAGE=11;
            }
        }
        namespace MERGE_FREE_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INVALID_TARGET_ORDER=1;
                constexpr uint16_t FAIL_REASON_CODE_TARGET_ORDER_NOT_GREATER=2;
                constexpr uint16_t FAIL_REASON_CODE_INDEX_ALIGN_TOO_SMALL=3;
                constexpr uint16_t FAIL_REASON_CODE_OUT_OF_RANGE=4;
                constexpr uint16_t FAIL_REASON_CODE_NOT_HEAD_PAGE=5;
                constexpr uint16_t FAIL_REASON_CODE_ORDER_MISMATCH=6;
                constexpr uint16_t FAIL_REASON_CODE_NOT_FREE=7;
                constexpr uint16_t FAIL_REASON_CODE_NOT_ALLOCATABLE=8;
                constexpr uint16_t FAIL_REASON_CODE_REFCOUNT_NONZERO=9;
                constexpr uint16_t FAIL_REASON_CODE_TRANSPARENT_PAGE_INVALID=10;
                constexpr uint16_t FAIL_REASON_CODE_HEAD_PTR_MISMATCH=11;
                constexpr uint16_t FAIL_REASON_CODE_HUGE_ORDER_MISMATCH=12;
                constexpr uint16_t NOT_HEAD_PAGE=13;
            }
        }
    };
    constexpr uint8_t LOCATION_CODE_PAGES_ARR=0x4;
    namespace PAGES_ARR_EVENTS{
        constexpr uint8_t EVENT_CODE_SIMP_PAGES_SET=0x1;
        namespace SIMP_PAGES_SET_RESULTS_CODE{
            namespace FAIL_REASONS_CODE{
                constexpr uint16_t FAIL_REASON_CODE_INTERVAL_NOT_IN_FREERAM=1;
            }
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
    static KURD_t Init(init_to_kernel_info*info);
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
    vaddr_t stack_alloc(KURD_t*kurd_out,uint64_t _4kbpgscount);//专用栈分配接口，内部调用__wrapped_pgs_valloc，返回的是栈底指针，栈底指针下面有1页读写作为缓冲
    //但是注意，栈底指针在高位，栈顶在低位
}
