#pragma once
#include "stdint.h"
#include "util/bitmap.h"
struct alloc_init_flags_t{
    bool is_longtime;
    bool is_crucial_variable;
    bool vaddraquire;
    bool force_first_linekd_heap;
    bool is_when_realloc_force_new_addr;//在realloc中强制重新分配内存，非realloc接口忽视此位但是会忠实记录进入metadata,realloc中此位不设置会优先原地调整，原地调整解决则不会修改源地址和元数据flags
    uint8_t align_log2;
};
constexpr alloc_init_flags_t default_init_flags={
    .is_longtime=false,
    .is_crucial_variable=false,
    .vaddraquire=true,
    .force_first_linekd_heap=false,
    .is_when_realloc_force_new_addr=false,
    .align_log2=4
};
namespace INIT_MEMMODULE_LOCAIONS
{
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR=4;//[4~7]是kpoolmemmgr的子模块
    namespace KPOOLMEMMGR_EVENTS{
        constexpr uint8_t EVENT_CODE_INIT=0;
        constexpr uint8_t EVENT_CODE_ALLOC=1;
        namespace ALLOC_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_NO_AVALIABLE_MEM=4;
                constexpr uint16_t REASON_CODE_SIZE_IS_ZERO=5;
            }
            
        }
        constexpr uint8_t EVENT_CODE_REALLOC=2;
        namespace REALLOC_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_DEMAND_SIZE_IS_ZERO=4;
                constexpr uint16_t REASON_CODE_PTR_NOT_IN_ANY_HEAP=5;
                constexpr uint16_t REASON_CODE_NO_AVALIABLE_MEM=6;
            }
            
        }
        constexpr uint8_t EVENT_CODE_PER_PROCESSOR_HEAP_INIT=3;
        namespace PER_PROCESSOR_HEAP_INIT_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_ALREADY_ENABLED=1;
                constexpr uint16_t REASON_CODE_BAD_PROCESSOR_COUNT=2;
                constexpr uint16_t REASON_CODE_NO_VADDR_SPACE=3;
                constexpr uint16_t REASON_CODE_VM_ADD_FAIL=4;
                constexpr uint16_t REASON_CODE_IDX_OUT_OF_RANGE=5;
                constexpr uint16_t REASON_CODE_HEAP_ALREADY_EXISTS=6;
                constexpr uint16_t REASON_CODE_HEAP_NOT_EXIST=7;
            }
        }
    }
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR_HCB=5;
    namespace KPOOLMEMMGR_HCB_EVENTS{
        constexpr uint8_t EVENT_CODE_INIT=0;
        namespace INIT_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_first_linekd_heap_NOT_ALLOWED=1;
            }
        }
        constexpr uint8_t EVENT_CODE_CLEAR=1;
        namespace CLEAR_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_BAD_ADDR=1;
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_METADATA_DESTROYED=1;
            }
        }
        constexpr uint8_t EVENT_CODE_ALLOC=2;
        namespace ALLOC_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_TOO_HIGH_ALIGN_DEMAND=1;
                constexpr uint16_t REASON_CODE_SIZE_DEMAND_IS_ZERO=2;
                constexpr uint16_t REASON_CODE_SIZE_DEMAND_TOO_LARGE=3;
                constexpr uint16_t REASON_CODE_SEARCH_MEMSEG_FAIL=4;
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_ALIGN_DEMAND_INVALID=1;//理应根据前面的步骤不会出现
            }
        }
        constexpr uint8_t EVENT_CODE_FREE=3;
        namespace FREE_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_BAD_ADDR=1;
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_METADATA_DESTROYED=1;
            }
        }
        constexpr uint8_t EVENT_CODE_INHEAP_REALLOC=4;
        namespace INHEAP_REALLOC_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_BAD_ADDR=1;
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_METADATA_DESTROYED=1;
                constexpr uint16_t REASON_CODE_UNREACHABLE_CODE=2;
            }
        }
    }
    constexpr uint8_t LOCATION_CODE_KPOOLMEMMGR_HCB_BITMAP=6;//INIT事件涉及到KURD,但是不是这个层面产生的，是页框系统的
    namespace KPOOLMEMMGR_HCB_BITMAP_EVENTS{ 
        constexpr uint8_t EVENT_CODE_INIT=0;
        namespace INIT_RESULTS{
            namespace FAIL_RESONS{
                constexpr uint16_t REASON_CODE_HCB_BITMAP_INIT_FAIL=1;
            }
        }
    }
};
using phyaddr_t=uint64_t;
using vaddr_t=uint64_t;
class INIT_HCB//堆控制块，必须是连续物理地址空间的连续内存
    {
        private:
            // 活跃分配魔数（推荐）
        static constexpr uint64_t MAGIC_ALLOCATED    = 0xDEADBEEFCAFEBABEull;
        KURD_t default_kurd();
        KURD_t default_success();
        KURD_t default_fail();
        KURD_t default_fatal();
        enum HCB_bitmap_error_code_t:uint8_t
        {
            SUCCESS=0,
            HCB_BITMAP_BAD_PARAM=1,
            AVALIBLE_MEMSEG_SEARCH_FAIL=2,
            TOO_BIG_MEM_DEMAND=3,
        };
        
        class HCB_bitmap:public bitmap_t
        { 
            HCB_bitmap_error_code_t param_checkment(uint64_t bit_idx,uint64_t bit_count);
            KURD_t default_kurd();
            KURD_t default_success();
            KURD_t default_fail();
            KURD_t default_fatal();
            public:
            int Init();//只有第一个堆的初始化会调用此函数，所以不用 KURD
            KURD_t second_stage_Init(
                uint32_t entries_count
            );
            HCB_bitmap();
            ~HCB_bitmap();
            // 公开基类的 protected 成员
            using bitmap_t::bitmap_rwlock;
            using bitmap_t::used_bit_count_lock;
            using bitmap_t::bitmap_used_bit;
            using bitmap_t::bitmap_size_in_64bit_units;
            using bitmap_t::bit_set;
            using bitmap_t::bit_get;
            using bitmap_t::bits_set;
            using bitmap_t::bytes_set;
            using bitmap_t::u64s_set;
            using bitmap_t::get_bitmap_used_bit;
            using bitmap_t::count_bitmap_used_bit;
            using bitmap_t::used_bit_count_add;
            using bitmap_t::used_bit_count_sub;
            int continual_avaliable_bits_search(uint64_t bit_count,uint64_t&result_base_idx);
            int continual_avaliable_bytes_search(uint64_t byte_count,uint64_t&result_base_idx);
            int continual_avaliable_u64s_search(uint64_t u64_count,uint64_t&result_base_idx);
            HCB_bitmap_error_code_t continual_avaliable_u64s_search_higher_alignment(uint64_t u64idx_align_log2,uint64_t u64_count,uint64_t&result_base_idx);
            bool target_bit_seg_is_avaliable(uint64_t bit_idx,uint64_t bit_count,HCB_bitmap_error_code_t&err);//考虑用 uint64_t 来优化扫描，扫描的时候要加锁
            HCB_bitmap_error_code_t bit_seg_set(uint64_t bit_idx,uint64_t bit_count,bool value);//优化的 set 函数，中间会解析，然后调用对应的 bits_set，bytes_set，u64s_set，并且会参数检查，加锁
        };
        HCB_bitmap bitmap_controller;
        phyaddr_t phybase;
        vaddr_t vbase;
        uint32_t total_size_in_bytes;
        struct data_flags{
            uint32_t alignment:4;//实际对齐为2<<alignment字节对齐，最大记录为64kb对齐，实际分配上最大1024字节对齐
            uint32_t is_longtime_alloc:1;//是否是长时间分配的变量
            uint32_t is_crucial_variable:1;//是否是关键变量,如果是，在free，in_heap_realloc的时候检查到魔数被篡改，会触发内核恐慌
        };
        
        static constexpr uint8_t bytes_per_bit = 0x10;//一个bit控制16字节数
        public: 
        struct alignas(16) data_meta{//每个被分配的都有元信息以及相应魔数
            //是分配在堆内，后面紧接着就是数据
            uint16_t data_size;
            alloc_init_flags_t alloc_flags;
            uint64_t magic;
        };
        static_assert(sizeof(data_meta)==16,"data_meta size must be 16 bytes");
        int first_linekd_heap_Init();//只能由first_linekd_heap调用的初始化
        //用指针检验是不是那个特殊堆
        INIT_HCB(uint32_t size,vaddr_t vbase);//给某个逻辑处理器初始化一个HCB
        INIT_HCB();
        KURD_t second_stage_Init();
        ~INIT_HCB();
        //分配之后对于数据区的清零操作
        //必须是魔数有效才会操作
        KURD_t clear(void*ptr);

        KURD_t in_heap_alloc(void *&addr,uint32_t size,alloc_init_flags_t flags);

        KURD_t free(void*ptr);

        KURD_t in_heap_realloc(void*&ptr, uint32_t new_size,alloc_init_flags_t flags);
        uint64_t get_used_bytes_count();
        bool is_full();
        void count_used_bytes();
        bool is_addr_belong_to_this_hcb(void* addr);
    };
extern INIT_HCB heap;
void* operator new(uint64_t size);
void* operator new(uint64_t size,alloc_init_flags_t flags);
void* operator new[](uint64_t size);
void* operator new[](uint64_t size,alloc_init_flags_t flags);
void operator delete(void* ptr) noexcept;
void operator delete(void* ptr, uint64_t) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete[](void* ptr, uint64_t) noexcept;


// 放置 new 操作符
void* operator new(uint64_t, void* ptr) noexcept;
void* operator new[](uint64_t, void* ptr) noexcept;
