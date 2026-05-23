#include "util/huge_bitmap.h"
#include "util/OS_utils.h"
#include "memory/FreePagesAllocator.h"
#ifdef USER_MODE
#include <cstdint>
#include <cstring>
#include <cstdlib>
#endif
// 添加__popcountdi2函数的实现（GCC内置函数）
extern "C" int __popcountdi2(unsigned long x) {
    // 使用位运算实现汉明重量(Hamming Weight)计算
    x = x - ((x >> 1) & 0x5555555555555555UL);
    x = (x & 0x3333333333333333UL) + ((x >> 2) & 0x3333333333333333UL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fUL;
    x = x + (x >> 8);
    x = x + (x >> 16);
    x = x + (x >> 32);
    return x & 0x7f;
}

#include "util/Ktemplats.h"
#ifdef KERNEL_MODE
#include "memory/all_pages_arr.h"
#include "memory/kpoolmemmgr.h"
#endif
void bitmap_t::bit_set(uint64_t bit_idx, bool value)
{
    if (value)
    {
        uint64_t u64_idx=bit_idx>>6;
        uint64_t to_write=1ULL<<(bit_idx&63);
        bitmap[u64_idx] |= to_write;
    }
    else
    {
        bitmap[bit_idx>>6] &= ~(1ULL << (bit_idx & 63));
    }
}

bool bitmap_t::bit_get(uint64_t bit_idx)
{
    return bitmap[bit_idx>>6] & (1ULL << (bit_idx & 63));
}

void bitmap_t::bits_set(uint64_t start_bit_idx, uint64_t bit_count, bool value)
{
    uint64_t end_bit_idx = start_bit_idx + bit_count;
    
    // 处理起始部分，直到下一个64位边界
    while((start_bit_idx < end_bit_idx) && ((start_bit_idx & 0x3F) != 0)) {
        bit_set(start_bit_idx, value);
        start_bit_idx++;
    }
    
    // 如果还有剩余位需要设置，且至少有一个完整的64位单元
    if(start_bit_idx < end_bit_idx) {
        uint64_t start_u64_idx = start_bit_idx >> 6;
        uint64_t end_u64_idx = end_bit_idx >> 6;
        
        // 处理完整的64位单元
        if(start_u64_idx < end_u64_idx) {
            uint64_t u64_count = end_u64_idx - start_u64_idx;
            u64s_set(start_u64_idx, u64_count, value);
            start_bit_idx = end_u64_idx << 6; // 更新start_bit_idx到处理完的64位边界
        }
        
        // 处理最后不足64位的部分
        while(start_bit_idx < end_bit_idx) {
            bit_set(start_bit_idx, value);
            start_bit_idx++;
        }
    }
}

void bitmap_t::bytes_set(uint64_t start_byte_idx, uint64_t byte_count, bool value)
{
    uint64_t end_byte_idx = start_byte_idx + byte_count;
    uint8_t to_fill_value = value ? BYTE_FULL : 0x00;
    
    // 处理起始部分，直到下一个8字节(64位)边界
    while((start_byte_idx < end_byte_idx) && ((start_byte_idx & 0x7) != 0)) {
        reinterpret_cast<uint8_t*>(bitmap)[start_byte_idx] = to_fill_value;
        start_byte_idx++;
    }
    
    // 如果还有剩余字节需要设置，且至少有一个完整的64位单元
    if(start_byte_idx < end_byte_idx) {
        uint64_t start_u64_idx = start_byte_idx >> 3;
        uint64_t end_u64_idx = end_byte_idx >> 3;
        
        // 处理完整的64位(8字节)单元
        if(start_u64_idx < end_u64_idx) {
            uint64_t u64_count = end_u64_idx - start_u64_idx;
            u64s_set(start_u64_idx, u64_count, value);
            start_byte_idx = end_u64_idx << 3; // 更新start_byte_idx到处理完的8字节边界
        }
        
        // 处理最后不足8字节的部分
        while(start_byte_idx < end_byte_idx) {
            reinterpret_cast<uint8_t*>(bitmap)[start_byte_idx] = to_fill_value;
            start_byte_idx++;
        }
    }
}

void bitmap_t::u64s_set(uint64_t start_u64_idx, uint64_t u64_count, bool value)
{
    uint64_t to_fill_value= value ? U64_FULL_UNIT : 0x00;
    for(uint64_t i = start_u64_idx; i < start_u64_idx + u64_count; i++)
    bitmap[i]= to_fill_value;
}

huge_bitmap::huge_bitmap(uint64_t bits_count)
{
    this->bitmap_size_in_64bit_units = (bits_count + 63) / 64;
}
KURD_t huge_bitmap::second_stage_init()
{
    KURD_t ret=KURD_t();
    #ifdef KERNEL_MODE
    
    uint64_t pgs4kbcount=(bitmap_size_in_64bit_units+511)/512;
    bitmap=(uint64_t*)__wrapped_pgs_valloc(&ret,pgs4kbcount,page_state_t::kernel_pinned,12);
    #endif
    #ifdef USER_MODE
    bitmap=(uint64_t*)malloc(bitmap_size_in_64bit_units*sizeof(uint64_t));
    #endif
    if(!bitmap){
        ksetmem_8(bitmap, 0, bitmap_size_in_64bit_units*sizeof(uint64_t));
    }
    return ret;
}
huge_bitmap::~huge_bitmap()
{
    #ifdef KERNEL_MODE
    
    uint64_t pgs4kbcount=(bitmap_size_in_64bit_units+511-1)/512;
    KURD_t kurd=__wrapped_pgs_vfree(bitmap,pgs4kbcount);
    if(!success_all_kurd(kurd)){

    }
    #endif
    #ifdef USER_MODE
    free(bitmap);
    #endif
}

Ktemplats::kernel_bitmap::kernel_bitmap(uint64_t bit_count)
{
    uint64_t u64_count = (bit_count + 63) >> 6;
    bitmap_size_in_64bit_units = u64_count;

    bitmap = new uint64_t[u64_count];

    if (!bitmap) {
        bitmap_size_in_64bit_units = 0;
        return;
    }

    for (uint64_t i = 0; i < u64_count; ++i)
        bitmap[i] = 0;
}

Ktemplats::kernel_bitmap::~kernel_bitmap()
{
    if (bitmap) {
        delete[] bitmap;
        bitmap = nullptr;
    }
    bitmap_size_in_64bit_units = 0;
}

bool bitmap_t::all_true()
{
    // 遍历所有完整的 64 位单元
    for (uint64_t i = 0; i < bitmap_size_in_64bit_units; ++i) {
        if (bitmap[i] != U64_FULL_UNIT) {
            return false;
        }
    }
    return true;
}

bool bitmap_t::all_false()
{
    // 遍历所有完整的 64 位单元
    for (uint64_t i = 0; i < bitmap_size_in_64bit_units; ++i) {
        if (bitmap[i] != 0x00) {
            return false;
        }
    }
    return true;
}
