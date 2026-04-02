#pragma once
#include "stdint.h"
#include "util/bitmap.h"
#include "memmodule_err_definitions.h"
#include "abi/boot.h"
#include <util/lock.h>
typedef uint64_t size_t;
typedef uint64_t phyaddr_t;
typedef uint64_t vaddr_t;

enum data_type_t:uint8_t
{
    DT_UNKNOWN = 0,
    DT_ARRAY = 1,
    DT_STRUCT = 2,
    DT_CLASS = 3,
};

namespace MEMMODULE_LOCAIONS
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
class kpoolmemmgr_t
{
private:
    static constexpr uint32_t HCB_DEFAULT_SIZE=0x200000;
    static constexpr uint32_t HCB_DEFAULT_BITMAP_SIZE=HCB_DEFAULT_SIZE/128;
    static constexpr uint8_t PER_PROCESSOR_MAX_HCB_COUNT_ALIGN2 = 0x4;
public:
    class HCB_v2//堆控制块，必须是连续物理地址空间的连续内存
    {
        public:
        static constexpr int SIZE_BUCKET_COUNT = 10;
        struct HCB_statistics_t
        {//原始大小落入(1<<(4+SIZE_BUCKET_COUNT))<=size<(1<<(5+SIZE_BUCKET_COUNT))的落入对应的桶
        uint64_t alloc_count;
        uint64_t free_count;
        uint64_t alloc_fail_count;
        uint64_t realloc_count;
        uint64_t realloc_fail_count;
        uint32_t peak_used_bytes_count;
        uint64_t alloc[SIZE_BUCKET_COUNT];//alloc接口接收的size各桶的出现次数
        uint64_t free[SIZE_BUCKET_COUNT];//free接口接收的size各桶的出现次数
        };
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
        
        
        HCB_statistics_t statistics;
        class HCB_bitmap:public bitmap_t
        { 
            HCB_bitmap_error_code_t param_checkment(uint64_t bit_idx,uint64_t bit_count);
            KURD_t default_kurd();
            KURD_t default_success();
            KURD_t default_fail();
            KURD_t default_fatal();
            public:
            static constexpr uint64_t invalid_cache=~0ull;
            struct scan_cache_t
            {
            uint64_t hint_u64_idx;        // 下一次扫描起点
            uint64_t last_success_idx;    // 最近成功分配位置(u64引索)
            uint64_t largest_free_hint_len; // 若扫描到空洞但是由于大小不够放弃，单位是u64
            uint64_t largest_free_hint_base;   // 若扫描到空洞但是由于大小不够放弃的话就尝试更新这两个字段
            };//无效就要标记相应条目无效
            scan_cache_t scan_cache;
            struct hcbbitmap_statistics
            {
            uint64_t call_bits_scan_count;
            uint64_t bits_scan_largest_free_hint_hit_count;
            uint64_t bits_scan_largest_free_hint_miss_count;
            uint64_t bits_scan_all_steps_accum;
            uint64_t bits_scan_max_steps_in_a_term;
            uint64_t call_bytes_scan_count;
            uint64_t bytes_scan_largest_free_hint_hit_count;
            uint64_t bytes_scan_largest_free_hint_miss_count;
            uint64_t bytes_scan_all_steps_accum;
            uint64_t bytes_scan_max_steps_in_a_term;
            uint64_t call_u64s_scan_count;
            uint64_t u64s_scan_largest_free_hint_hit_count;
            uint64_t u64s_scan_largest_free_hint_miss_count;
            uint64_t u64s_scan_all_steps_accum;
            uint64_t u64s_scan_max_steps_in_a_term;
            uint64_t call_u64s_scan_higher_alignment_count;
            uint64_t call_bits_seg_set_count;
            };
            hcbbitmap_statistics statistics;
            int Init(loaded_VM_interval*first_static_heap_bitmap);//只有第一个堆的初始化会调用此函数，所以不用KURD
            KURD_t second_stage_Init(
                uint32_t entries_count
            );
            HCB_bitmap();
            ~HCB_bitmap();
            HCB_bitmap_error_code_t continual_avaliable_bits_search(uint64_t bit_count,uint64_t&result_base_idx);
            HCB_bitmap_error_code_t continual_avaliable_bytes_search(uint64_t byte_count,uint64_t&result_base_idx);
            HCB_bitmap_error_code_t continual_avaliable_u64s_search(uint64_t u64_count,uint64_t&result_base_idx);
            HCB_bitmap_error_code_t continual_avaliable_u64s_search_higher_alignment(uint64_t u64idx_align_log2,uint64_t u64_count,uint64_t&result_base_idx);
            bool target_bit_seg_is_avaliable(uint64_t bit_idx,uint64_t bit_count,HCB_bitmap_error_code_t&err);//考虑用uint64_t来优化扫描，扫描的时候要加锁
            HCB_bitmap_error_code_t bits_seg_set(uint64_t bit_idx,uint64_t bit_count,bool value);//优化的set函数，中间会解析，然后调用对应的bits_set，bytes_set，u64s_set，并且会参数检查，加锁
            friend class HCB_v2;
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
            uint32_t data_size;
            alloc_flags_t alloc_flags;
            uint64_t magic;
        };
        static_assert(sizeof(data_meta)==16,"data_meta size must be 16 bytes");
        int first_linekd_heap_Init(loaded_VM_interval *first_static_heap, loaded_VM_interval *first_static_heap_bitmap);//只能由first_linekd_heap调用的初始化
        //用指针检验是不是那个特殊堆
        HCB_v2(uint32_t size,vaddr_t vbase);//给某个逻辑处理器初始化一个HCB
        HCB_v2();
        KURD_t second_stage_Init();
        ~HCB_v2();
        //分配之后对于数据区的清零操作
        //必须是魔数有效才会操作
        KURD_t clear(void*ptr);
        /**
         * @brief 分配内存  
         * 思路：
         * 1.根据大小与对齐需求选择扫描器
         * 2.制造扫描大小
         * 3.使用对应扫描器扫描内存
         * 4.分配数据与元数据
         * 对于16字节对齐的时候是直接元数据，数据一起分配
         * 对于128,1024字节对齐则是元数据单独分配一个bit
         * 遵守最低浪费原则，先解析出数据区要分配的bit数目，再解析出byte,u64要分配的数目
         * 由前到后先大单元分配，再逐渐减小
         * 5.填写元数据
         * 
         */
        KURD_t in_heap_alloc(void *&addr,uint32_t size,alloc_flags_t flags);
        /**
         * @brief 释放内存
         * 思路：
         * 1.先检查元信息表项，若魔数被篡改，返回应该触发内核恐慌的返回值，
         * 2.释放元信息表项
         * 3.释放数据
         * 4.出了函数，在内核池管理器内核恐慌，并且报告相关错误信息
         */
        KURD_t free(void*ptr);
        /**
         * @brief 重新分配内存
         * 思路：
         * 1.先检查元信息表项，若魔数被篡改，则返回应该触发内核恐慌的返回值
         * 2.先尝试原地拓展
         * 3.若失败则尝试in_heap_alloc，释放原始数据，进行复制
         * 4.实在不行这个堆内存空间无法使用，返回错误码
         * 5.返回管理器的realloc接口的时候根据返回值尝试新堆alloc,释放原堆，或者内核恐慌，并且报告相关错误信息
         * */
        KURD_t in_heap_realloc(void*&ptr, uint32_t new_size,alloc_flags_t flags);
        uint64_t get_used_bytes_count();
        bool is_full();
        void count_used_bytes();
        bool is_addr_belong_to_this_hcb(void* addr);
        phyaddr_t tran_to_phy(void* addr);//这两个是通过HCB里面的虚拟基址，物理基址直接算出来的
        vaddr_t tran_to_virt(phyaddr_t addr);//地址翻译函数若没有命中就返回垃圾值
    };
private:
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();
    static bool is_muli_heap_enabled;//是否允许在HCB_ARRAY中分配新的HCB,应该在全局页管理器初始化完成之后调用
    //这个位开启后会优先在cpu专属堆里面操作，再尝试first_linekd_heap
    static HCB_v2 first_linekd_heap;
    static HCB_v2**HCB_ARRAY;
    static spinrwlock_cpp_t HCB_ARRAY_lock;
    static KURD_t alloc_heap(uint32_t idx);
    static KURD_t free_heap(uint32_t idx);
    static HCB_v2*find_hcb_by_address(void* ptr);
public:
    /**
     * @param vaddraquire true返回虚拟地址，false返回物理地址
     * @param alignment 实际对齐值=2<<alignment,最高支持到13，8kb对齐
     */
    static void *kalloc(uint64_t size,KURD_t&no_succes_report,alloc_flags_t flags=default_flags);//这两个的KURD还是要返回，但是是在返回的指针中，不过要通过检查对齐来判断可不可能是KURD
    static void *realloc(void *ptr,KURD_t&no_succes_report, uint64_t size,alloc_flags_t flags=default_flags); // 根据表在优先在基地址不变的情况下尝试修改堆对象大小
    // 实在不行就创建一个新对象
    static void clear(void *ptr); // 主要用于结构体清理内存，new一个结构体后用这个函数根据传入的起始地址查找堆的元信息表项，并把该元信息项对应的内存空间全部写0
    // 别用这个清理new之后的对象
    static KURD_t Init(loaded_VM_interval *first_static_heap,loaded_VM_interval*first_static_heap_bitmap); // 真正的初始化，全局对象手动初始化函数，但是是全局单例，设计上都是依赖静态资源，理论上这个模块初始化不可能失败，不引入KURD
    static KURD_t multi_heap_enable();
    static void kfree(void *ptr);
    static phyaddr_t get_phy(vaddr_t addr);
    static vaddr_t get_virt(phyaddr_t addr);
    kpoolmemmgr_t();
    ~kpoolmemmgr_t();
};
extern "C"{
    void* __wrapped_heap_alloc(uint64_t size,KURD_t*kurd,alloc_flags_t flags=default_flags);
    void __wrapped_heap_free(void*addr);
    void* __wrapped_heap_realloc(void*addr,uint64_t size,KURD_t*kurd,alloc_flags_t flags);
}
constexpr int INDEX_NOT_EXIST = -100;
// 全局 new/delete 操作符重载声明
//new重载里面new失败就要第一时间panic，不然后续的垃圾地址会产生页错误
void* operator new(size_t size);
void* operator new(size_t size,alloc_flags_t flags);
void* operator new[](size_t size);
void* operator new[](size_t size,alloc_flags_t flags);
void operator delete(void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete[](void* ptr, size_t) noexcept;


// 放置 new 操作符
void* operator new(size_t, void* ptr) noexcept;
void* operator new[](size_t, void* ptr) noexcept;
