#include "util/OS_utils.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "linker_symbols.h"
#include "stdint.h"
#include "memory/memory_base.h"
#include "abi/os_error_definitions.h"
#ifdef USER_MODE
#include <x86intrin.h>
#endif
#ifdef KERNEL_MODE 
#include "kintrin.h"
#endif
typedef uint64_t size_t;


uint64_t align_down(uint64_t x, uint64_t a){ return x & ~(a-1); }

// min/max 函数实现
uint64_t min(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

uint64_t max(uint64_t a, uint64_t b) {
    return (a > b) ? a : b;
}

// log2 函数实现：计算以2为底的对数（向上取整）
uint8_t log2(uint64_t value) {
    if (value == 0) return 0;
    if (value == 1) return 0;
    
    uint8_t result = 0;
    uint64_t temp = value - 1; // 减1后向下取整，等效于原值向上取整
    
    while (temp > 0) {
        temp >>= 1;
        result++;
    }
    
    return result;
}

// log2_up 函数实现：计算以2为底的对数（向下取整）
uint8_t log2_up(uint64_t value) {
    if (value == 0) return 0;
    uint8_t result = 0;
    while (value > 1) {
        value >>= 1;
        result++;
    }
    return result;
}
uint64_t alignup_and_shift_right(uint64_t value, uint8_t align_log2)
{
    return align_up(value, 1ull<<align_log2)>>align_log2;
}
uint64_t format_num_to_buffer(char* out, uint64_t raw, num_format_t format, numer_system_select radix)
{
    if (!out) return 0;

    uint8_t len_in_bytes = 8;
    bool is_signed = false;
    switch (format) {
        case num_format_t::u8:  len_in_bytes = 1; is_signed = false; break;
        case num_format_t::s8:  len_in_bytes = 1; is_signed = true;  break;
        case num_format_t::u16: len_in_bytes = 2; is_signed = false; break;
        case num_format_t::s16: len_in_bytes = 2; is_signed = true;  break;
        case num_format_t::u32: len_in_bytes = 4; is_signed = false; break;
        case num_format_t::s32: len_in_bytes = 4; is_signed = true;  break;
        case num_format_t::u64: len_in_bytes = 8; is_signed = false; break;
        case num_format_t::s64: len_in_bytes = 8; is_signed = true;  break;
        default: break;
    }

    uint64_t mask = ~0ULL;
    if (len_in_bytes < 8) {
        mask = (1ULL << (len_in_bytes * 8)) - 1;
    }
    uint64_t value = raw & mask;
    bool negative = false;
    if (radix == numer_system_select::DEC && is_signed) {
        const bool sign_bit_set = (value >> (len_in_bytes * 8 - 1)) & 1;
        if (sign_bit_set) {
            negative = true;
            value = (~value + 1) & mask;
        }
    }

    uint32_t base = 10;
    if (radix == numer_system_select::BIN) base = 2;
    else if (radix == numer_system_select::HEX) base = 16;

    char rev[70];
    uint32_t idx = 0;
    if (value == 0) {
        rev[idx++] = '0';
    } else {
        while (value > 0) {
            const uint32_t digit = value % base;
            value /= base;
            rev[idx++] = (digit < 10) ? static_cast<char>('0' + digit)
                                      : static_cast<char>('A' + (digit - 10));
        }
    }
    if (negative) {
        rev[idx++] = '-';
    }
    for (uint32_t i = 0; i < idx; ++i) {
        out[i] = rev[idx - 1 - i];
    }
    return idx;
}

void __kspace_stack_chk_fail(void)
{
    asm volatile ("int $0xc");
}

// 定义栈保护的canary值
uintptr_t __stack_chk_guard = 0x595e9f73bb9247cf;

// 使用链接器wrap选项重载__stack_chk_fail函数
extern "C" void __wrap___stack_chk_fail(void)
{
    __kspace_stack_chk_fail();
}

bool is_aligned(uint64_t value, uint8_t align_log2)
{
    return !(value & ((1ull << align_log2) - 1));
}
/**
 * @brief 通用函数：将虚拟 - 物理地址区间按照页面大小拆分为多个条目
 *
 * 根据虚拟地址和物理地址的同余关系（congruence），智能地将区间拆分为 1GB/2MB/4KB 的页面组合。
 * 拆分策略优先使用大页面以减少 TLB 条目数，边界不对齐部分使用小页面填充。
 *
 * @param result 输出参数，存储拆分后的页面信息包
 * @param vmentry VM 描述符，包含虚拟地址和物理地址信息
 * @return int OS_SUCCESS 成功，其他错误码见 abi/os_error_definitions.h
 */
bool is_addr_kernel_address(void *addr)
{
    return (uint64_t)addr >= (uint64_t)&base_kernel_address;
}
seg_to_pages_info_pakage_t vm_interval::to_pages_info() const
{
    constexpr uint64_t _4KB_SIZE = 0x1000;
    constexpr uint64_t _2MB_SIZE = 1ULL << 21;
    constexpr uint64_t _1GB_SIZE = 1ULL << 30;

    seg_to_pages_info_pakage_t result;
    result.clear();

    vaddr_t va_start = vbase();
    vaddr_t va_end   = va_start + byte_cnt();
    phyaddr_t pa_start = pbase();

    if (va_start >= va_end) return result;  // 空区间

    // 计算同余等级
    if (va_start % _1GB_SIZE == pa_start % _1GB_SIZE) {
        result.congruence_level = congruence_level_1gb;
    } else if (va_start % _2MB_SIZE == pa_start % _2MB_SIZE) {
        result.congruence_level = congruence_level_2mb;
    } else {
        result.congruence_level = congruence_level_4kb;
    }

    auto paddr_of = [va_start, pa_start](vaddr_t v) -> phyaddr_t {
        return pa_start + v - va_start;
    };
    auto mine = [](uint64_t a, uint64_t b) -> uint64_t { return a < b ? a : b; };
    auto maxe = [](uint64_t a, uint64_t b) -> uint64_t { return a > b ? a : b; };

    switch (result.congruence_level) {
    case congruence_level_1gb: {
        vaddr_t _1gb_begin = align_up(va_start, _1GB_SIZE);
        vaddr_t _1gb_end   = align_down(va_end, _1GB_SIZE);

        if (_1gb_end > _1gb_begin) {
            result.entryies[0] = {
                .vbase = _1gb_begin,
                .phybase = paddr_of(_1gb_begin),
                .page_size_in_byte = _1GB_SIZE,
                .num_of_pages = (_1gb_end - _1gb_begin) / _1GB_SIZE,
            };

            vaddr_t _2mb_begin = align_up(va_start, _2MB_SIZE);
            vaddr_t _2mb_end   = align_down(va_end, _2MB_SIZE);

            if (_2mb_begin < _1gb_begin) {
                result.entryies[1] = {
                    .vbase = _2mb_begin,
                    .phybase = paddr_of(_2mb_begin),
                    .page_size_in_byte = _2MB_SIZE,
                    .num_of_pages = (mine(_2mb_end, _1gb_begin) - _2mb_begin) / _2MB_SIZE,
                };
            }
            if (_1gb_end < _2mb_end) {
                result.entryies[2] = {
                    .vbase = _1gb_end,
                    .phybase = paddr_of(_1gb_end),
                    .page_size_in_byte = _2MB_SIZE,
                    .num_of_pages = (_2mb_end - maxe(_1gb_end, _2mb_begin)) / _2MB_SIZE,
                };
            }

            if (va_start < _2mb_begin) {
                result.entryies[3] = {
                    .vbase = va_start,
                    .phybase = pa_start,
                    .page_size_in_byte = _4KB_SIZE,
                    .num_of_pages = (_2mb_begin - va_start) / _4KB_SIZE,
                };
            }
            if (_2mb_end < va_end) {
                result.entryies[4] = {
                    .vbase = _2mb_end,
                    .phybase = paddr_of(_2mb_end),
                    .page_size_in_byte = _4KB_SIZE,
                    .num_of_pages = (va_end - _2mb_end) / _4KB_SIZE,
                };
            }
            break;
        }
    }
        [[fallthrough]];

    case congruence_level_2mb: {
        vaddr_t _2mb_begin = align_up(va_start, _2MB_SIZE);
        vaddr_t _2mb_end   = align_down(va_end, _2MB_SIZE);

        if (_2mb_begin < _2mb_end) {
            result.entryies[0] = {
                .vbase = _2mb_begin,
                .phybase = paddr_of(_2mb_begin),
                .page_size_in_byte = _2MB_SIZE,
                .num_of_pages = (_2mb_end - _2mb_begin) / _2MB_SIZE,
            };

            if (va_start < _2mb_begin) {
                result.entryies[1] = {
                    .vbase = va_start,
                    .phybase = pa_start,
                    .page_size_in_byte = _4KB_SIZE,
                    .num_of_pages = (_2mb_begin - va_start) / _4KB_SIZE,
                };
            }
            if (_2mb_end < va_end) {
                result.entryies[2] = {
                    .vbase = _2mb_end,
                    .phybase = paddr_of(_2mb_end),
                    .page_size_in_byte = _4KB_SIZE,
                    .num_of_pages = (va_end - _2mb_end) / _4KB_SIZE,
                };
            }
            break;
        }
    }
        [[fallthrough]];

    case congruence_level_4kb: {
        result.entryies[0] = {
            .vbase = va_start,
            .phybase = pa_start,
            .page_size_in_byte = _4KB_SIZE,
            .num_of_pages = (va_end - va_start) / _4KB_SIZE,
        };
        break;
    }
    }

    return result;
}

int vm_interval_to_pages_info(seg_to_pages_info_pakage_t &result, vm_interval interval)
{
    VM_DESC tmp = {
        .start = interval.vbase(),
        .end = interval.vbase() + interval.byte_cnt(),
        .map_type = VM_DESC::MAP_NONE,
        .phys_start = interval.pbase(),
    };
    return vm_interval_to_pages_info(result, tmp);
}

int vm_interval_to_pages_info(seg_to_pages_info_pakage_t &result, VM_DESC vmentry)
{
    constexpr uint32_t _4KB_SIZE=0x1000;
    constexpr uint32_t _2MB_SIZE=1ULL<<21;
    constexpr uint32_t _1GB_SIZE=1ULL<<30;    
  result.clear();
    
    // 参数校验
    if(vmentry.start % _4KB_SIZE || vmentry.end % _4KB_SIZE || vmentry.phys_start % _4KB_SIZE) {
        // 参数未 4KB 对齐，返回错误
      return OS_INVALID_PARAMETER;
    }
    
    // 处理边界情况
    if (vmentry.start >= vmentry.end) {
      return OS_INVALID_PARAMETER;
    }
    
    // 计算同余级别
    if(vmentry.start % _1GB_SIZE == vmentry.phys_start % _1GB_SIZE) {
      result.congruence_level = congruence_level_1gb;
    } else if(vmentry.start % _2MB_SIZE == vmentry.phys_start % _2MB_SIZE) {
      result.congruence_level = congruence_level_2mb;
    } else {
      result.congruence_level = congruence_level_4kb;
    }
    
    // 辅助 lambda 函数
    auto paddr = [vbase = vmentry.start, pbase = vmentry.phys_start](vaddr_t vaddr) -> phyaddr_t{
      return pbase + vaddr - vbase;
    };
    
    auto min = [](vaddr_t a, vaddr_t b) -> vaddr_t{
      return a < b ? a : b;
    };
    
    auto max = [](vaddr_t a, vaddr_t b) -> vaddr_t{
      return a > b ? a : b;
    };
    
    switch(result.congruence_level) {
        case congruence_level_1gb: {
          vaddr_t _1gb_begin = align_up(vmentry.start, _1GB_SIZE);
          vaddr_t _1gb_end = align_down(vmentry.end, _1GB_SIZE);
            
            if(_1gb_end > _1gb_begin) {
                // 1GB 区域（中间对齐部分）
              result.entryies[0].vbase = _1gb_begin;
              result.entryies[0].phybase = paddr(_1gb_begin);
              result.entryies[0].num_of_pages = (_1gb_end - _1gb_begin) / _1GB_SIZE;
              result.entryies[0].page_size_in_byte = _1GB_SIZE;
                
              vaddr_t _2mb_begin = align_up(vmentry.start, _2MB_SIZE);
              vaddr_t _2mb_end = align_down(vmentry.end, _2MB_SIZE);
                
                // 2MB 区域（1GB 前后的对齐部分）
                if(_2mb_begin < _1gb_begin) {
                  result.entryies[1].vbase = _2mb_begin;
                  result.entryies[1].phybase = paddr(_2mb_begin);
                  result.entryies[1].num_of_pages = (min(_2mb_end, _1gb_begin) - _2mb_begin) / _2MB_SIZE;
                  result.entryies[1].page_size_in_byte = _2MB_SIZE;
                }
                if(_1gb_end < _2mb_end) {
                  result.entryies[2].vbase = _1gb_end;
                  result.entryies[2].phybase = paddr(_1gb_end);
                  result.entryies[2].num_of_pages = (_2mb_end - max(_1gb_end, _2mb_begin)) / _2MB_SIZE;
                  result.entryies[2].page_size_in_byte = _2MB_SIZE;
                }
                
                // 4KB 区域（最前和最后的剩余部分）
                if(vmentry.start < _2mb_begin) {
                  result.entryies[3].vbase = vmentry.start;
                  result.entryies[3].phybase = vmentry.phys_start;
                  result.entryies[3].num_of_pages = (_2mb_begin - vmentry.start) / _4KB_SIZE;
                  result.entryies[3].page_size_in_byte = _4KB_SIZE;
                }
                if(_2mb_end < vmentry.end) {
                  result.entryies[4].vbase = _2mb_end;
                  result.entryies[4].phybase = paddr(_2mb_end);
                  result.entryies[4].num_of_pages = (vmentry.end - _2mb_end) / _4KB_SIZE;
                  result.entryies[4].page_size_in_byte = _4KB_SIZE;
                }
                break;
            }
        }
        [[fallthrough]];
        
        case congruence_level_2mb: {
          vaddr_t _2mb_begin = align_up(vmentry.start, _2MB_SIZE);
          vaddr_t _2mb_end = align_down(vmentry.end, _2MB_SIZE);
            
            if(_2mb_begin < _2mb_end) {
                // 2MB 区域（中间对齐部分）
              result.entryies[0].vbase = _2mb_begin;
              result.entryies[0].phybase = paddr(_2mb_begin);
              result.entryies[0].num_of_pages = (_2mb_end - _2mb_begin) / _2MB_SIZE;
              result.entryies[0].page_size_in_byte = _2MB_SIZE;
                
                // 4KB 区域（前后剩余部分）
                if(vmentry.start < _2mb_begin) {
                  result.entryies[1].vbase = vmentry.start;
                  result.entryies[1].phybase = vmentry.phys_start;
                  result.entryies[1].num_of_pages = (_2mb_begin - vmentry.start) / _4KB_SIZE;
                  result.entryies[1].page_size_in_byte = _4KB_SIZE;
                }
                if(_2mb_end < vmentry.end) {
                  result.entryies[2].vbase = _2mb_end;
                  result.entryies[2].phybase = paddr(_2mb_end);
                  result.entryies[2].num_of_pages = (vmentry.end - _2mb_end) / _4KB_SIZE;
                  result.entryies[2].page_size_in_byte = _4KB_SIZE;
                }
                break;
            }
        }
        [[fallthrough]];
        
        case congruence_level_4kb: {
          result.entryies[0].vbase = vmentry.start;
          result.entryies[0].phybase = vmentry.phys_start;
          result.entryies[0].num_of_pages = (vmentry.end - vmentry.start) / _4KB_SIZE;
          result.entryies[0].page_size_in_byte = _4KB_SIZE;
            break;
        }
        
        default: {
            // 不应该到达这里
          return OS_UNREACHABLE_CODE;
        }
    }
   
  return OS_SUCCESS;
}
// 获取2bit宽度位图中指定索引的值（返回0-3）
uint64_t align_up(uint64_t value, uint64_t alignment) {
    // 检查alignment是否为2的幂
    if ((alignment & (alignment - 1)) != 0) {
        // 如果不是2的幂，可以返回0或者处理错误
        return 0; // 或者抛出错误/断言
    }
    // 计算对齐后的值
    return (value + alignment - 1) & ~(alignment - 1);
}
/* 带写屏障的8位原子写入（编译器原语，跨ISA） */
void atomic_write8_wmb(volatile void *addr, uint8_t val)
{
    __atomic_store_n((volatile uint8_t *)addr, val, __ATOMIC_RELEASE);
}

/* 带写屏障的16位原子写入（编译器原语，跨ISA） */
void atomic_write16_wmb(volatile void *addr, uint16_t val)
{
    __atomic_store_n((volatile uint16_t *)addr, val, __ATOMIC_RELEASE);
}

/* 带写屏障的32位原子写入（编译器原语，跨ISA） */
void atomic_write32_wmb(volatile void *addr, uint32_t val)
{
    __atomic_store_n((volatile uint32_t *)addr, val, __ATOMIC_RELEASE);
}

/* 带写屏障的64位原子写入（编译器原语，跨ISA） */
void atomic_write64_wmb(volatile void *addr, uint64_t val)
{
    __atomic_store_n((volatile uint64_t *)addr, val, __ATOMIC_RELEASE);
}

/* 带读屏障的8位原子读取（编译器原语，跨ISA） */
uint8_t atomic_read8_rmb(volatile void *addr)
{
    return __atomic_load_n((volatile uint8_t *)addr, __ATOMIC_ACQUIRE);
}

/* 带读屏障的16位原子读取（编译器原语，跨ISA） */
uint16_t atomic_read16_rmb(volatile void *addr)
{
    return __atomic_load_n((volatile uint16_t *)addr, __ATOMIC_ACQUIRE);
}

/* 带读屏障的32位原子读取（编译器原语，跨ISA） */
uint32_t atomic_read32_rmb(volatile void *addr)
{
    return __atomic_load_n((volatile uint32_t *)addr, __ATOMIC_ACQUIRE);
}

/* 带读屏障的64位原子读取（编译器原语，跨ISA） */
uint64_t atomic_read64_rmb(volatile void *addr)
{
    return __atomic_load_n((volatile uint64_t *)addr, __ATOMIC_ACQUIRE);
}

/* 带回读验证的8位原子写入（编译器原语，跨ISA） */
void atomic_write8_rdbk(volatile void *addr, uint8_t val)
{
    atomic_write8_wmb(addr, val);
    while (__atomic_load_n((volatile uint8_t *)addr, __ATOMIC_ACQUIRE) != val);
}

/* 带回读验证的16位原子写入（编译器原语，跨ISA） */
void atomic_write16_rdbk(volatile void *addr, uint16_t val)
{
    atomic_write16_wmb(addr, val);
    while (__atomic_load_n((volatile uint16_t *)addr, __ATOMIC_ACQUIRE) != val);
}

/* 带回读验证的32位原子写入（编译器原语，跨ISA） */
void atomic_write32_rdbk(volatile void *addr, uint32_t val)
{
    atomic_write32_wmb(addr, val);
    while (__atomic_load_n((volatile uint32_t *)addr, __ATOMIC_ACQUIRE) != val);
}

/* 带回读验证的64位原子写入（编译器原语，跨ISA） */
void atomic_write64_rdbk(volatile void *addr, uint64_t val)
{
    atomic_write64_wmb(addr, val);
    while (__atomic_load_n((volatile uint64_t *)addr, __ATOMIC_ACQUIRE) != val);
}





extern "C" void __cxa_pure_virtual()
{
    while (1) asm volatile("hlt");
}

