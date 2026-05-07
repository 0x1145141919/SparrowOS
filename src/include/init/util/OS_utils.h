#pragma once
#include "stdint.h"
#include "stddef.h"

#ifdef __cplusplus
enum numer_system_select : uint8_t
{
    BIN,
    DEC,
    HEX
};
enum num_format_t : uint8_t {
    u8,
    s8,
    u16,
    s16,
    u32,
    s32,
    u64,
    s64,
};

enum class atomic_memory_order : int {
    relaxed = __ATOMIC_RELAXED,
    consume = __ATOMIC_CONSUME,
    acquire = __ATOMIC_ACQUIRE,
    release = __ATOMIC_RELEASE,
    acq_rel = __ATOMIC_ACQ_REL,
    seq_cst = __ATOMIC_SEQ_CST
};
/**
 * 慎用运算符重载语法糖，默认cst,可能会引入不必要的内存屏障，导致性能下降
 */
template <typename T>
class atomic_scalar_t {
private:
    volatile T value_;

public:
    atomic_scalar_t() : value_(0) {}
    explicit atomic_scalar_t(T initial) : value_(initial) {}

    T load(atomic_memory_order order = atomic_memory_order::seq_cst) const {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }

    void store(T desired, atomic_memory_order order = atomic_memory_order::seq_cst) {
        __atomic_store_n(&value_, desired, static_cast<int>(order));
    }

    T xchg(T desired, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_exchange_n(&value_, desired, static_cast<int>(order));
    }

    bool cmpxchg_strong(
        T& expected,
        T desired,
        atomic_memory_order success = atomic_memory_order::seq_cst,
        atomic_memory_order failure = atomic_memory_order::seq_cst) {
        return __atomic_compare_exchange_n(
            &value_, &expected, desired, false, static_cast<int>(success), static_cast<int>(failure));
    }

    bool cmpxchg_weak(
        T& expected,
        T desired,
        atomic_memory_order success = atomic_memory_order::seq_cst,
        atomic_memory_order failure = atomic_memory_order::seq_cst) {
        return __atomic_compare_exchange_n(
            &value_, &expected, desired, true, static_cast<int>(success), static_cast<int>(failure));
    }

    T add_ka(T arg, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_fetch_add(&value_, arg, static_cast<int>(order));
    }

    T sub_ka(T arg, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_fetch_sub(&value_, arg, static_cast<int>(order));
    }

    T and_ka(T arg, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_fetch_and(&value_, arg, static_cast<int>(order));
    }

    T or_ka(T arg, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_fetch_or(&value_, arg, static_cast<int>(order));
    }

    T xor_ka(T arg, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_fetch_xor(&value_, arg, static_cast<int>(order));
    }

    T nand_ka(T arg, atomic_memory_order order = atomic_memory_order::seq_cst) {
        return __atomic_fetch_nand(&value_, arg, static_cast<int>(order));
    }

    T operator+=(T arg) {
        return static_cast<T>(__atomic_add_fetch(&value_, arg, static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator-=(T arg) {
        return static_cast<T>(__atomic_sub_fetch(&value_, arg, static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator&=(T arg) {
        return static_cast<T>(__atomic_and_fetch(&value_, arg, static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator|=(T arg) {
        return static_cast<T>(__atomic_or_fetch(&value_, arg, static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator^=(T arg) {
        return static_cast<T>(__atomic_xor_fetch(&value_, arg, static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator++() {
        return static_cast<T>(__atomic_add_fetch(&value_, static_cast<T>(1), static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator++(int) {
        return static_cast<T>(__atomic_fetch_add(&value_, static_cast<T>(1), static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator--() {
        return static_cast<T>(__atomic_sub_fetch(&value_, static_cast<T>(1), static_cast<int>(atomic_memory_order::seq_cst)));
    }

    T operator--(int) {
        return static_cast<T>(__atomic_fetch_sub(&value_, static_cast<T>(1), static_cast<int>(atomic_memory_order::seq_cst)));
    }
};

using i8ka = atomic_scalar_t<int8_t>;
using u8ka = atomic_scalar_t<uint8_t>;
using i16ka = atomic_scalar_t<int16_t>;
using u16ka = atomic_scalar_t<uint16_t>;
using i32ka = atomic_scalar_t<int32_t>;
using u32ka = atomic_scalar_t<uint32_t>;
using i64ka = atomic_scalar_t<int64_t>;
using u64ka = atomic_scalar_t<uint64_t>;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t size_t;
void ksystemramcpy(void*src,void*dest,size_t length);
void linearTBSerialDelete(//这是一个对于线性表删除一段连续项,起始索引a,结束索引b的函数
    uint64_t*TotalEntryCount,
    uint64_t a,
    uint64_t b,
    void*linerTbBase,
    uint32_t entrysize
);
void linearTBSerialInsert(
    uint64_t* TotalEntryCount,
    uint64_t insertIndex,
    void* newEntry,
    void* linerTbBase,
    uint32_t entrysize,
    uint64_t entryCount = 1
) ;

int strcmp_in_kernel(const char *str1, const char *str2,uint32_t max_strlen=4096);
int strlen_in_kernel(const char *s);
void ksetmem_8(void* ptr, uint8_t value, uint64_t size_in_byte);
void ksetmem_16(void* ptr, uint16_t value, uint64_t size_in_byte);
void ksetmem_32(void* ptr, uint32_t value, uint64_t size_in_byte);
void ksetmem_64(void* ptr, uint64_t value, uint64_t size_in_byte);
extern "C" void __wrap___stack_chk_fail(void);
uint64_t align_up(uint64_t value, uint64_t alignment);
extern const uint8_t bit_reverse_table[256];
uint64_t reverse_perbytes(uint64_t value) ;
void atomic_write8_wmb(volatile void *addr, uint8_t val);
void atomic_write16_wmb(volatile void *addr, uint16_t val);
void atomic_write32_wmb(volatile void *addr, uint32_t val);
void atomic_write64_wmb(volatile void *addr, uint64_t val);
uint8_t atomic_read8_rmb(volatile void *addr);
uint16_t atomic_read16_rmb(volatile void *addr);
uint32_t atomic_read32_rmb(volatile void *addr);
uint64_t atomic_read64_rmb(volatile void *addr);
void atomic_write8_rdbk(volatile void *addr, uint8_t val);
void atomic_write16_rdbk(volatile void *addr, uint16_t val);
void atomic_write32_rdbk(volatile void *addr, uint32_t val);
void atomic_write64_rdbk(volatile void *addr, uint64_t val);
uint64_t align_down(uint64_t value, uint64_t alignment);
uint64_t rdmsr(uint32_t offset);
void wrmsr_func(uint32_t offset,uint64_t value);
uint64_t rdtsc();
uint64_t read_gs_u64(size_t index);
void gs_u64_write(uint32_t index, uint64_t value);
uint32_t fast_get_processor_id();
uint64_t format_num_to_buffer(char* out, uint64_t raw, num_format_t format, numer_system_select radix);
#ifdef USER_MODE
// 将内存描述符表从文本格式转换为gBaseMemMgr.Init函数所需的格式
void print_memory_descriptor_for_gbasememmgr(const char* input_file_path);
int convert_memory_descriptor_table(const char* input_file_path, const char* output_file_path);
#endif

#ifdef __cplusplus
}
#endif
