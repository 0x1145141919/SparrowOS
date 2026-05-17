#include "init/heap_alloc.h"
#include "util/OS_utils.h"
#include "init/init_linker_symbols.h"
#include "init/panic.h"

// C++ 运行时桩实现 - 用于 freestanding 环境
namespace {
    struct __cxa_packed_key {
        void *key;
    };
    
    // 简单的 atexit 处理桩
    struct atexit_entry {
        void (*func)(void*);
        void* arg;
        void* dso_handle;
    };
    
    constexpr int MAX_ATEXIT_ENTRIES = 64;
    atexit_entry atexit_table[MAX_ATEXIT_ENTRIES];
    int atexit_count = 0;
}

// 定义 __dso_handle - C++ 共享库句柄
void* __dso_handle __attribute__((weak)) = nullptr;

// __cxa_atexit 实现 - 注册清理函数
extern "C" int __cxa_atexit(void (*func)(void*), void* arg, void* dso_handle) {
    if (atexit_count < MAX_ATEXIT_ENTRIES) {
        atexit_table[atexit_count].func = func;
        atexit_table[atexit_count].arg = arg;
        atexit_table[atexit_count].dso_handle = dso_handle;
        atexit_count++;
    }
    return 0;
}

// __cxa_finalize 实现 - 调用所有注册的清理函数
extern "C" void __cxa_finalize(void* dso_handle) {
    for (int i = atexit_count - 1; i >= 0; i--) {
        if (dso_handle == nullptr || atexit_table[i].dso_handle == dso_handle) {
            if (atexit_table[i].func) {
                atexit_table[i].func(atexit_table[i].arg);
            }
        }
    }
}

#ifdef USER_MODE
#include "stdlib.h"
#endif  
#ifdef USER_MODE
    constexpr uint64_t FIRST_STATIC_HEAP_SIZE=1ULL<<24;
    
    INIT_HCB::INIT_HCB()
    {
        bitmap_controller.Init();
    }
#endif
constexpr uint32_t max_heap_size=1ULL<<24;
constexpr uint32_t real_heap_size=1ULL<<22;
uint64_t raw_bitmap[max_heap_size/1024];
INIT_HCB heap;
INIT_HCB::INIT_HCB()
{
}

INIT_HCB::~INIT_HCB()
{
    // 空实现，内核生命周期内不主动销毁堆
}

INIT_HCB::HCB_bitmap_error_code_t INIT_HCB::HCB_bitmap::param_checkment(uint64_t bit_idx, uint64_t bit_count)
{
    if(bit_idx+bit_count>bitmap_size_in_64bit_units*64){
        return INIT_HCB::HCB_bitmap_error_code_t::HCB_BITMAP_BAD_PARAM;
    }
    if(bit_count>bitmap_size_in_64bit_units*64){
        return INIT_HCB::HCB_bitmap_error_code_t::TOO_BIG_MEM_DEMAND;
    }
    return INIT_HCB::HCB_bitmap_error_code_t::SUCCESS;
}
int INIT_HCB::HCB_bitmap::Init()
{
    uint32_t bitmap_size_in_byte=0;
#ifdef KERNEL_MODE
    this->bitmap=raw_bitmap;
    bitmap_size_in_byte=real_heap_size>>7;
#endif
#ifdef USER_MODE
    this->bitmap=(uint64_t*)malloc(FIRST_STATIC_HEAP_SIZE/128);
    bitmap_size_in_byte=FIRST_STATIC_HEAP_SIZE/128;
#endif
    bitmap_size_in_64bit_units=bitmap_size_in_byte/8;
    ksetmem_8(this->bitmap, 0, bitmap_size_in_byte);
    bitmap_used_bit=0;
    return OS_SUCCESS;
}
KURD_t INIT_HCB::HCB_bitmap::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,INIT_MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB_BITMAP,0,0,err_domain::CORE_MODULE);
}
KURD_t INIT_HCB::HCB_bitmap::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}
KURD_t INIT_HCB::HCB_bitmap::default_fail()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}
KURD_t INIT_HCB::HCB_bitmap::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}
INIT_HCB::HCB_bitmap::HCB_bitmap()
{
}
INIT_HCB::HCB_bitmap::~HCB_bitmap()
{
    
}
int INIT_HCB::HCB_bitmap::continual_avaliable_bits_search(uint64_t bit_count, uint64_t &result_base_idx)
{
    if (bit_count == 0) {
        return OS_SUCCESS;
    }
    uint64_t bit_scanner_end = bitmap_size_in_64bit_units << 6;
    uint64_t seg_base_bit_idx = 0;
    bool seg_value = bit_get(0);
    uint64_t seg_length = 1;

    while (seg_base_bit_idx + seg_length < bit_scanner_end) {
        uint64_t current_bit_idx = seg_base_bit_idx + seg_length;

        if ((current_bit_idx & 0x3F) == 0) {
            uint64_t* to_scan_u64 = reinterpret_cast<uint64_t*>(
                reinterpret_cast<uint8_t*>(bitmap) + (current_bit_idx >> 3));

            if (seg_value) {
                if (*to_scan_u64 == U64_FULL_UNIT) {
                    seg_length += 64;
                    continue;
                }
            } else {
                if (*to_scan_u64 == 0x00) {
                    seg_length += 64;
                    if (seg_length >= bit_count) {
                        result_base_idx = seg_base_bit_idx;
                        return OS_SUCCESS;
                    }
                    continue;
                }
            }
        }

        bool new_bit_value = bit_get(current_bit_idx);

        if (seg_value == new_bit_value) {
            seg_length++;
            if (!seg_value && seg_length >= bit_count) {
                result_base_idx = seg_base_bit_idx;
                return OS_SUCCESS;
            }
        } else {
            if (!seg_value && seg_length >= bit_count) {
                result_base_idx = seg_base_bit_idx;
                return OS_SUCCESS;
            }
            seg_base_bit_idx = current_bit_idx;
            seg_value = new_bit_value;
            seg_length = 1;
        }
    }

    if (!seg_value && seg_length >= bit_count) {
        result_base_idx = seg_base_bit_idx;
        return OS_SUCCESS;
    }

    return OS_NOT_EXIST;
}

int INIT_HCB::HCB_bitmap::continual_avaliable_bytes_search(uint64_t byte_count, uint64_t &result_base_idx)
{
    if (byte_count == 0) {
        return OS_SUCCESS;
    }
    const uint64_t total_bytes = bitmap_size_in_64bit_units * 8;
    uint64_t seg_base_byte_idx = 0;
    bool seg_value = !!reinterpret_cast<uint8_t*>(bitmap)[0];
    uint64_t seg_length = 1;

    while (seg_base_byte_idx + seg_length < total_bytes) {
        uint64_t current_byte_idx = seg_base_byte_idx + seg_length;

        if ((current_byte_idx & 0x7) == 0) {
            uint64_t* to_scan_u64 = reinterpret_cast<uint64_t*>(
                reinterpret_cast<uint8_t*>(bitmap) + current_byte_idx);

            if (seg_value) {
                if (*to_scan_u64 == U64_FULL_UNIT) {
                    seg_length += 8;
                    continue;
                }
            } else {
                if (*to_scan_u64 == 0x00ULL) {
                    seg_length += 8;
                    if (seg_length >= byte_count) {
                        result_base_idx = seg_base_byte_idx;
                        return OS_SUCCESS;
                    }
                    continue;
                }
            }
        }

        bool new_value = !!reinterpret_cast<uint8_t*>(bitmap)[current_byte_idx];

        if (seg_value == new_value) {
            seg_length++;
            if (!seg_value && seg_length >= byte_count) {
                result_base_idx = seg_base_byte_idx;
                return OS_SUCCESS;
            }
        } else {
            if (!seg_value && seg_length >= byte_count) {
                result_base_idx = seg_base_byte_idx;
                return OS_SUCCESS;
            }
            seg_base_byte_idx = current_byte_idx;
            seg_value = new_value;
            seg_length = 1;
        }
    }

    if (!seg_value && seg_length >= byte_count) {
        result_base_idx = seg_base_byte_idx;
        return OS_SUCCESS;
    }

    return OS_NOT_EXIST;
}

int INIT_HCB::HCB_bitmap::continual_avaliable_u64s_search(uint64_t u64_count, uint64_t &result_base_idx)
{
    if (u64_count == 0) {
        return OS_SUCCESS;
    }
    const uint64_t total_u64s = bitmap_size_in_64bit_units;
    uint64_t seg_base_u64_idx = 0;
    bool seg_value = !!bitmap[0];
    uint64_t seg_length = 1;

    while (seg_base_u64_idx + seg_length < total_u64s) {
        uint64_t current_u64_idx = seg_base_u64_idx + seg_length;
        bool new_value = !!bitmap[current_u64_idx];

        if (seg_value == new_value) {
            seg_length++;
            if (!seg_value && seg_length >= u64_count) {
                result_base_idx = seg_base_u64_idx;
                return OS_SUCCESS;
            }
        } else {
            if (!seg_value && seg_length >= u64_count) {
                result_base_idx = seg_base_u64_idx;
                return OS_SUCCESS;
            }
            seg_base_u64_idx = current_u64_idx;
            seg_value = new_value;
            seg_length = 1;
        }
    }

    if (!seg_value && seg_length >= u64_count) {
        result_base_idx = seg_base_u64_idx;
        return OS_SUCCESS;
    }

    return OS_NOT_EXIST;
}
INIT_HCB::HCB_bitmap_error_code_t
 INIT_HCB::HCB_bitmap::
continual_avaliable_u64s_search_higher_alignment(
    uint64_t u64idx_align_log2,
    uint64_t u64_count,
    uint64_t &result_base_idx
)
{
    if (u64_count == 0)
        return SUCCESS;

    const uint64_t total_u64s = bitmap_size_in_64bit_units;

    // 用完即弃的对齐计算
    auto align_u64_idx = [&](uint64_t idx) -> uint64_t {
        if (u64idx_align_log2 == 0)
            return idx;
        const uint64_t align = 1ULL << u64idx_align_log2;
        return (idx + align - 1) & ~(align - 1);
    };

    uint64_t seg_base_u64_idx = 0;
    bool seg_value = !!bitmap[0];
    uint64_t seg_length = 1;

    while (seg_base_u64_idx + seg_length < total_u64s) {
        uint64_t cur_idx = seg_base_u64_idx + seg_length;
        bool new_value = !!bitmap[cur_idx];

        if (new_value == seg_value) {
            seg_length++;
        } else {
            // 完整 free 段处理
            if (!seg_value) {
                uint64_t seg_end = seg_base_u64_idx + seg_length;
                uint64_t aligned_base = align_u64_idx(seg_base_u64_idx);

                if (aligned_base >= seg_base_u64_idx &&
                    aligned_base + u64_count <= seg_end) {
                    result_base_idx = aligned_base;
                    return SUCCESS;
                }
            }

            // 切换段
            seg_base_u64_idx = cur_idx;
            seg_value = new_value;
            seg_length = 1;
        }
    }

    // 最后一个段
    if (!seg_value) {
        uint64_t seg_end = seg_base_u64_idx + seg_length;
        uint64_t aligned_base = align_u64_idx(seg_base_u64_idx);

        if (aligned_base >= seg_base_u64_idx &&
            aligned_base + u64_count <= seg_end) {
            result_base_idx = aligned_base;
            return SUCCESS;
        }
    }

    return AVALIBLE_MEMSEG_SEARCH_FAIL;
}

bool INIT_HCB::HCB_bitmap::target_bit_seg_is_avaliable
(uint64_t bit_idx, 
    uint64_t bit_count,
INIT_HCB::HCB_bitmap_error_code_t&err)
{
    if (bit_count == 0) return true;
    err=param_checkment(bit_idx,bit_count);
    if(err!=SUCCESS){
        return false;
    }
    uint64_t start_bit = bit_idx;
    uint64_t end_bit = bit_idx + bit_count;

    uint64_t start_u64 = start_bit >> 6;
    uint64_t end_u64   = (end_bit - 1) >> 6;  // inclusive
    uint64_t start_off = start_bit & 63;
    uint64_t end_off   = end_bit & 63;

    // 1️⃣ 如果目标区间小于64bit，直接逐bit检查，可读性高
    if (bit_count <= 64)
    {
        for (uint64_t i = 0; i < bit_count; ++i)
        {
            if (bit_get(bit_idx + i))
            {
                return false;
            }
        }
        return true;
    }

    // 2️⃣ 跨越多个 u64 块的情况
    // ---- 前导未对齐部分 ----
    if (start_off != 0)
    {
        uint64_t mask = (~0ULL) << start_off;
        if (bitmap[start_u64] & mask)
        {

            return false;
        }
        start_u64++;
    }

    // ---- 中间完整的 u64 区域 ----
    for (uint64_t i = start_u64; i < end_u64; ++i)
    {
        if (bitmap[i] != 0ULL)
        {

            return false;
        }
    }

    // ---- 尾部未对齐部分 ----
    if (end_off != 0)
    {
        uint64_t mask = (1ULL << end_off) - 1ULL;
        if (bitmap[end_u64] & mask)
        {

            return false;
        }
    }

    return true;
}

INIT_HCB::HCB_bitmap_error_code_t
 INIT_HCB::HCB_bitmap::bit_seg_set(uint64_t bit_idx, uint64_t bit_count, bool value)
{
    if (bit_count == 0) return SUCCESS;

    HCB_bitmap_error_code_t status=param_checkment(bit_idx,bit_count);
    if(status!=SUCCESS){
        return status;
    }
    uint64_t end_bit = bit_idx + bit_count;
    uint64_t start_align64 = (bit_idx + 63) & ~63ULL;   // 下一个64位边界
    uint64_t end_align64 = end_bit & ~63ULL;            // 末尾对齐边界

    // ----------- 1. 处理前导未对齐部分 -----------
    if (bit_idx < start_align64 && bit_idx < end_bit)
    {
        uint64_t pre_bits = (start_align64 > end_bit ? end_bit : start_align64) - bit_idx;
        bits_set(bit_idx, pre_bits, value);
        bit_idx += pre_bits;
    }

    // ----------- 2. 处理中间完整的u64块 -----------
    if (bit_idx < end_align64)
    {
        uint64_t u64_start_idx = bit_idx >> 6;               // /64
        uint64_t u64_count = (end_align64 - bit_idx) >> 6;   // /64
        u64s_set(u64_start_idx, u64_count, value);
        bit_idx += u64_count << 6;
    }

    // ----------- 3. 处理尾部未对齐部分 -----------
    if (bit_idx < end_bit)
    {
        bits_set(bit_idx, end_bit - bit_idx, value);
    }
    return SUCCESS;
}

int INIT_HCB::first_linekd_heap_Init()
{

    int status=this->bitmap_controller.Init();
    phybase=(phyaddr_t)&__init_heap_start;
    vbase=(vaddr_t)&__init_heap_start;
    total_size_in_bytes=(uint64_t)&__init_heap_end-(uint64_t)&__init_stack_start;
    return status;  

}
KURD_t INIT_HCB::clear(void *ptr)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR;
    fail.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR;
    fatal.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_CLEAR;
    if (!is_addr_belong_to_this_hcb(ptr)){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::CLEAR_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;
    }

    // ptr 是用户可见地址，meta 在其前面
    data_meta *meta = (data_meta *)((uint8_t *)ptr - sizeof(data_meta));
    if (meta->magic != MAGIC_ALLOCATED)
    {
        fatal.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::CLEAR_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }

    // 清零用户数据区域（不清 meta）
    // setmem 的原型假设为 setmem(void* addr, size_t size, int value)
    ksetmem_8(ptr, 0, meta->data_size);

    return success;
}
KURD_t INIT_HCB::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,INIT_MEMMODULE_LOCAIONS::LOCATION_CODE_KPOOLMEMMGR_HCB,0,0,err_domain::CORE_MODULE);
}

KURD_t INIT_HCB::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}

KURD_t INIT_HCB::default_fail()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}

KURD_t INIT_HCB::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}

/**
 * @brief 在堆中分配内存
 * 
 * 根据请求的大小和对齐要求，在堆中分配一块内存区域。
 * 分配策略根据大小分为三种：小块 (4 位对齐)、中块 (7 位对齐) 和大块 (10 位对齐)
 * 
 * @param addr 返回分配的内存地址
 * @param size 请求分配的内存大小 (字节)
 * @param is_crucial 是否为关键变量，当内存损坏时决定是否触发内核恐慌
 * @param vaddraquire true 返回虚拟地址，false 返回物理地址
 * @param alignment 对齐要求，实际对齐值为 2^alignment 字节对齐，最大支持 10(1024 字节对齐)
 * @return int OS_SUCCESS 表示成功分配，其他值表示分配失败
 */
KURD_t INIT_HCB::in_heap_alloc(
    void *&addr,
    uint32_t size,
    alloc_init_flags_t flags)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC;
    fail.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC;
    fatal.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_ALLOC;
    // -----------------------------
    // 基本参数与边界检查
    // -----------------------------
    if (flags.align_log2 >=32)
        {
            fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_TOO_HIGH_ALIGN_DEMAND;
            return fail;
        }
    if (size == 0)
        {
            fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SIZE_DEMAND_IS_ZERO;
            return fail;
        }
    if(size>=1ULL<<16){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SIZE_DEMAND_TOO_LARGE;
        return fail;
    }
    // 常量定义（在内核空间必须为常量或宏）
    static constexpr uint32_t SMALL_UNIT_BYTES=16;
    static constexpr uint32_t MID_UNIT_BYTES = SMALL_UNIT_BYTES<<3;
    static constexpr uint32_t LARGE_UNIT_BYTES = MID_UNIT_BYTES<<3;
    // -----------------------------
    // 计算对齐方式
    // -----------------------------
    uint8_t size_alignment = 0;
    uint8_t aquire_alignment = 0;
    uint8_t final_alignment_aquire;

    // 根据请求大小确定对齐档次
    if (size < 2 * 8 * bytes_per_bit)
        size_alignment = 4;    // 16B 对齐
    else if (size < 2 * 64 * bytes_per_bit)
        size_alignment = 7;    // 128B 对齐
    else
        size_alignment = 10;   // 1024B 对齐

    // 根据 alignment 参数映射档次
    if (flags.align_log2 <= 4)
        aquire_alignment = 4;
    else if (flags.align_log2 <= 7)
        aquire_alignment = 7;
    else if (flags.align_log2 <= 10)
        aquire_alignment = 10;
    else aquire_alignment = flags.align_log2;
    // 最终取较大者
    final_alignment_aquire = (size_alignment > aquire_alignment) ?
                             size_alignment : aquire_alignment;

    // -----------------------------
    // 分配逻辑（不同对齐档）
    // -----------------------------
    int status = OS_SUCCESS;
    uintptr_t base_addr = (uintptr_t)(flags.vaddraquire ? vbase : phybase);

    switch (final_alignment_aquire)
    {
    case 4: {  // 小块分配 (16字节对齐)
        uint32_t serial_bits_count = (size + SMALL_UNIT_BYTES - 1 + sizeof(data_meta)) >> 4;
        uint64_t base_bit_idx = 0;
        status = bitmap_controller.continual_avaliable_bits_search(serial_bits_count, base_bit_idx);
        if (status != OS_SUCCESS)
            {
                fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }
        bitmap_controller.bit_seg_set(base_bit_idx, serial_bits_count,true);
        bitmap_controller.bitmap_used_bit+=serial_bits_count;
        uintptr_t offset = bytes_per_bit * (base_bit_idx + 1);
        addr = (void *)(base_addr + offset);
        break;
    }
    case 7: {  // 中块分配 (128字节对齐)
        uint32_t serial_bytes_count = ((size + MID_UNIT_BYTES - 1) >> 7) + 1;
        uint64_t base_byte_idx = 0;
        status = bitmap_controller.continual_avaliable_bytes_search(serial_bytes_count, base_byte_idx);
        if (status != OS_SUCCESS)
            {
                fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }

        bitmap_controller.bit_set(7 + (base_byte_idx << 3) , true);
        bitmap_controller.bitmap_used_bit++;
        uint32_t total_bitcount = (size + SMALL_UNIT_BYTES - 1) >> 4;
        bitmap_controller.bit_seg_set((base_byte_idx+1)*8, total_bitcount,true);
        bitmap_controller.bitmap_used_bit+=total_bitcount;
        uintptr_t offset = bytes_per_bit * ((base_byte_idx + 1) << 3);
        addr = (void *)(base_addr + offset);
        break;
    }
    case 10: {  // 大块分配 (1024字节对齐)
        uint32_t serial_u64_count = ((size + LARGE_UNIT_BYTES - 1) >> 10) + 1;
        uint64_t base_u64_idx = 0;
        status = bitmap_controller.continual_avaliable_u64s_search(serial_u64_count, base_u64_idx);
        if (status != OS_SUCCESS)
            {
                fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }

        bitmap_controller.bit_set( 63 + (base_u64_idx << 6), true);
        bitmap_controller.bitmap_used_bit++;
        uint32_t total_bitcount = (size + SMALL_UNIT_BYTES - 1) >> 4;
        bitmap_controller.bit_seg_set((base_u64_idx+1)*64, total_bitcount,true);
        bitmap_controller.bitmap_used_bit += total_bitcount;
        uintptr_t offset = bytes_per_bit * ((base_u64_idx + 1) << 6);
        addr = (void *)(base_addr + offset);
        break;
    }
    default:
    {
    if (final_alignment_aquire <= 10)
        {
            fatal.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FATAL_REASONS::REASON_CODE_ALIGN_DEMAND_INVALID;
            return fatal;
        }

    // ------------------------------------
    // 对齐参数（u64 粒度）
    // ------------------------------------
    const uint8_t u64_align_log2 = final_alignment_aquire - 10;
    const uint64_t u64_align = 1ULL << u64_align_log2;

    // ------------------------------------
    // 真实 payload 需要的 u64 数量
    // ------------------------------------
    const uint32_t payload_u64_count =
        (align_up(size, LARGE_UNIT_BYTES)) >> 10;

    // 最坏情况下需要的搜索长度（payload + padding）
    const uint32_t search_u64_count =
        align_up(payload_u64_count, u64_align)+ u64_align;

    uint64_t base_u64_idx = 0;

    // ------------------------------------
    // 搜索（只保证：有一个对齐起点能放下 payload）
    // ------------------------------------
    status = bitmap_controller.continual_avaliable_u64s_search_higher_alignment(
        u64_align_log2,
        search_u64_count,
        base_u64_idx
    );
    if (status != OS_SUCCESS)
        {
                fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::ALLOC_RESULTS::FAIL_RESONS::REASON_CODE_SEARCH_MEMSEG_FAIL;
                return fail;
            }

    // ------------------------------------
    // 计算真正的对齐起点
    // ------------------------------------
    uint64_t real_base_u64 =
        base_u64_idx+u64_align ;

    uint64_t real_base_bit = real_base_u64 << 6;

    // ------------------------------------
    // 标记 bitmap
    // sentinel: real_base_bit - 1
    // ------------------------------------
    bitmap_controller.bit_set(real_base_bit - 1, true);
    bitmap_controller.bitmap_used_bit++;

    const uint32_t total_bitcount =
        (size + SMALL_UNIT_BYTES - 1) >> 4;

    bitmap_controller.bit_seg_set(
        real_base_bit,
        total_bitcount,
        true
    );

    bitmap_controller.bitmap_used_bit += total_bitcount;

    // ------------------------------------
    // 返回地址（与 real_base_u64 严格一致）
    // ------------------------------------
    uintptr_t offset = bytes_per_bit * real_base_bit;
    addr = (void *)(base_addr + offset);

    break;
    }

    }

    // -----------------------------
    // 设置元数据（data_meta）
    // -----------------------------
    data_meta *meta = (data_meta *)((uint8_t *)addr - sizeof(data_meta));
    meta->magic = MAGIC_ALLOCATED;
    meta->data_size = size;
    meta->alloc_flags= flags;
    return success;
}

KURD_t INIT_HCB::free(void *ptr)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE;
    fail.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE;
    fatal.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_FREE;
    if(uint64_t(ptr)&15||!is_addr_belong_to_this_hcb(ptr)){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }
    uint32_t in_heap_offset;
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)ptr<MIN_KVADDR)
    {//物理地址
        if((uint64_t)ptr<phybase+sizeof(data_meta)||
    phybase+total_size_in_bytes<=(uint64_t)ptr
    ){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }
    in_heap_offset=(uint64_t)ptr-(uint64_t)phybase;
    }else{
        if((uint64_t)ptr<vbase+sizeof(data_meta)||
    vbase+total_size_in_bytes<=(uint64_t)ptr){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }
    in_heap_offset=(uint64_t)ptr-(uint64_t)vbase;
    }
    data_meta *meta = (data_meta *)((uint8_t *)ptr - sizeof(data_meta));
    if(meta->magic != MAGIC_ALLOCATED)
    {
        fatal.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }
    uint32_t total_bits_count=(meta->data_size+15)/16;
    uint32_t base_bit_idx=in_heap_offset/16;
    bitmap_controller.bit_set(base_bit_idx-1,false);
    bitmap_controller.bitmap_used_bit--;
    bitmap_controller.bitmap_used_bit-=total_bits_count;
    bitmap_controller.bit_seg_set(
        base_bit_idx,
        total_bits_count,
        false
    );
    return success;
}

KURD_t INIT_HCB::in_heap_realloc(
    void *&ptr, 
    uint32_t new_size,
    alloc_init_flags_t flags)
{
    KURD_t success=default_success();
    KURD_t  fail=default_fail();
    KURD_t fatal=default_fatal();
    success.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC;
    fail.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC;
    fatal.event_code=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::EVENT_CODE_INHEAP_REALLOC;
    if(uint64_t(ptr)&15||!is_addr_belong_to_this_hcb(ptr)){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    }  //本堆的内存至少16字节对齐
    uint32_t in_heap_offset;
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)ptr<MIN_KVADDR)
    {//物理地址
        if((uint64_t)ptr<phybase+sizeof(data_meta)||
    phybase+total_size_in_bytes<=(uint64_t)ptr
    ){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    } 
    in_heap_offset=(uint64_t)ptr-(uint64_t)phybase;
    }else{
        if((uint64_t)ptr<vbase+sizeof(data_meta)||
    vbase+total_size_in_bytes<=(uint64_t)ptr){
        fail.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FAIL_RESONS::REASON_CODE_BAD_ADDR;
        return fail;   
    } 
    in_heap_offset=(uint64_t)ptr-(uint64_t)vbase;
    }
    data_meta *meta = (data_meta *)((uint8_t *)ptr - sizeof(data_meta));
    uint16_t old_size=meta->data_size;
    if(meta->magic != MAGIC_ALLOCATED)
    {
        fatal.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::FREE_RESULTS::FATAL_REASONS::REASON_CODE_METADATA_DESTROYED;
        return fatal;
    }uint32_t old_bits_count=(meta->data_size+15)/16;
    uint32_t base_bit_idx=in_heap_offset/16;
    uint32_t new_bits_count=(new_size+15)/16;
    if(!flags.is_when_realloc_force_new_addr)
    {
    if(new_bits_count<=old_bits_count)
    {
        meta->data_size=new_size;
        bitmap_controller.bit_seg_set(
            base_bit_idx+new_bits_count,
            old_bits_count-new_bits_count,
            false
        );
        bitmap_controller.bitmap_used_bit-=(old_bits_count-new_bits_count);
        return success;
    }else{
        HCB_bitmap_error_code_t err;
        bool try_append=bitmap_controller.target_bit_seg_is_avaliable(
            base_bit_idx+old_bits_count,
            new_bits_count-old_bits_count,
            err
        );
        if(err!=SUCCESS)try_append=false;
        if(try_append)
        {
             bitmap_controller.bit_seg_set(
                base_bit_idx+old_bits_count,
                new_bits_count-old_bits_count,
                true
            );
            bitmap_controller.bitmap_used_bit+=(new_bits_count-old_bits_count);
            return success;
        }else{
            void*new_ptr;
            KURD_t status=in_heap_alloc(
                new_ptr,new_size,flags
            );
            if(status.result==result_code::SUCCESS){
                ksystemramcpy(ptr,new_ptr,old_size);
                status=free(ptr);
                ptr=new_ptr;
                bitmap_controller.bitmap_used_bit+=(new_bits_count-old_bits_count);
                return success;
            }else{
                return status;
            }
        }
    }
    }else{
        void*new_ptr;
            KURD_t status=in_heap_alloc(
                new_ptr,new_size,flags
            );
            if(status.result==result_code::SUCCESS){
                ksystemramcpy(ptr,new_ptr,old_size);
                status=free(ptr);
                ptr=new_ptr;
                bitmap_controller.bitmap_used_bit+=(new_bits_count-old_bits_count);
                return success;
            }else{
                return status;
            }
    }fatal.reason=INIT_MEMMODULE_LOCAIONS::KPOOLMEMMGR_HCB_EVENTS::INHEAP_REALLOC_RESULTS::FATAL_REASONS::REASON_CODE_UNREACHABLE_CODE;
    return fatal;
}

uint64_t INIT_HCB::get_used_bytes_count()
{
    return this->bitmap_controller.bitmap_used_bit*this->bytes_per_bit;
}

bool INIT_HCB::is_full()
{
    uint64_t used_bit_count=bitmap_controller.bitmap_used_bit;
    return used_bit_count==bitmap_controller.bitmap_size_in_64bit_units*64 ;
}
void INIT_HCB::count_used_bytes()
{
    bitmap_controller.count_bitmap_used_bit();
}

bool INIT_HCB::is_addr_belong_to_this_hcb(void *addr)
{
    uint64_t MIN_KVADDR=0;
    uint64_t MAX_PHYADDR=0;;
    #ifdef PGLV_5
    MIN_KVADDR=0xff00000000000000;
    MAX_PHYADDR=1ULL<<56;
    #endif
    #ifdef PGLV_4
    MIN_KVADDR=0xffff800000000000;
    MAX_PHYADDR=1ULL<<47;
    #endif
    if((uint64_t)addr<MAX_PHYADDR)
    {//物理地址
        if((uint64_t)addr<phybase+sizeof(data_meta)||
    phybase+total_size_in_bytes<=(uint64_t)addr
    )return false;

    }else{
        if(MAX_PHYADDR<=(uint64_t)addr&&(uint64_t)addr<MIN_KVADDR)return false;
        if((uint64_t)addr<vbase+sizeof(data_meta)||
    vbase+total_size_in_bytes<=(uint64_t)addr)return false;

    }
    return true;
}
void* operator new(size_t size) {
    KURD_t kurd;
    void* result= nullptr;
    kurd=heap.in_heap_alloc(result,size,default_init_flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void *operator new(size_t size, alloc_init_flags_t flags)
{
    KURD_t kurd;
    void* result= nullptr;
    kurd=heap.in_heap_alloc(result,size,flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}
void *operator new[](size_t size)
{
    KURD_t kurd;
    void* result= nullptr;
    kurd=heap.in_heap_alloc(result,size,default_init_flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void *operator new[](size_t size, alloc_init_flags_t flags)
{
    KURD_t kurd;
    void* result= nullptr;
    kurd=heap.in_heap_alloc(result,size,flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void* operator new[](size_t size, bool vaddraquire, uint8_t alignment) {
    alloc_init_flags_t flags = default_init_flags;
    flags.vaddraquire = vaddraquire;
    flags.align_log2 = alignment;
    
    KURD_t kurd;
    void* result= nullptr;
    kurd=heap.in_heap_alloc(result,size,flags);
    if(error_kurd(kurd)||result==nullptr){
        panic_info_inshort inshort={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(
            default_panic_behaviors_flags,
            "new operator failed",
            nullptr,
            &inshort,
            kurd
        );
    }
    return result;
}

void operator delete(void* ptr) noexcept {
    heap.free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {

}

void operator delete[](void* ptr) noexcept {
    heap.free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {

}

// 放置 new 操作符
void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

void* operator new[](size_t, void* ptr) noexcept {
    return ptr;
}
