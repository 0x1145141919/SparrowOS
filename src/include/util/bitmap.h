#pragma once
#include <stdint.h> 
#include "lock.h"
#include "abi/os_error_definitions.h"
//这是个字节内不反转的位图实现
//使用每一项为宽度1bit的位图
//以每64bit为单元存储
//内核态常用数据结构
//私有接口不加锁,但提供自旋锁，自行在外部接口中使用
class bitmap_t { 
    protected:
    spinrwlock_cpp_t bitmap_rwlock;
    spinlock_cpp_t used_bit_count_lock;
    uint64_t*bitmap;
    uint64_t bitmap_size_in_64bit_units;
    //byte,bit扫描的时候使用的指针，需要在构造函数里面初始化
    static constexpr uint64_t U64_FULL_UNIT = 0xFFFFFFFFFFFFFFFF;
    static constexpr uint8_t BYTE_FULL = 0xFF;
    //静态常量
    
public:    
    void bit_set(uint64_t bit_idx,bool value);
    bool bit_get(uint64_t bit_idx);
    void bits_set(uint64_t start_bit_idx,uint64_t bit_count,bool value);
    void bytes_set(uint64_t start_byte_idx,uint64_t byte_count,bool value);
    void u64s_set(uint64_t start_u64_idx,uint64_t u64_count,bool value);
    bool all_true();
    bool all_false();
    //上面三个函数不进行边界检查以及锁检查,设计思路上是基本操作，bits_set某种程度上可以被bytes_set,u64s_set优化但不会采用
};

class bitmap_base{
    protected:
    uint64_t*bitmap;//设bit_index,则通过bitmap访问那个bit则在bitmap[bit_index>>6]通过（1<<(bit_index&63)）进行访问
    uint64_t bitcount;
    public:
    struct bit_proxy{
        uint64_t* word;
        uint64_t  mask;
        operator bool() const { return (*word & mask) != 0; }
        bit_proxy& operator=(bool v){
            if(v) *word |= mask; else *word &= ~mask;
            return *this;
        }
    };
    void enable(uint64_t*bitmap,uint64_t bitcount){//安排原始数据的操作即是使能
        this->bitmap = bitmap;
        this->bitcount = bitcount;
    };
    uint64_t*get_base();
    void disable(){//擦出数据指针即是失能
        bitmap = nullptr;
        bitcount = 0;
    };
    void bitmap_base_boomer(uint64_t idx) const {
        if(__builtin_expect(idx >= bitcount, 0)){
            *(uint64_t*)1 = 0;// 越界 → 页错误
        }
        if(__builtin_expect(!bitmap, 0)){
            *(uint64_t*)0 = 0;// 空指针 → 页错误
        }
    }
    bit_proxy operator[](uint64_t idx){
        bitmap_base_boomer(idx);
        return { bitmap + (idx >> 6), 1ULL << (idx & 63) };
    }
    bool operator[](uint64_t idx) const {
        bitmap_base_boomer(idx);
        return (bitmap[idx >> 6] >> (idx & 63)) & 1ULL;
    }
};