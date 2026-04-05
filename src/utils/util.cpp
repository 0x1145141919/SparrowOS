#include "util/OS_utils.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
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

int strcmp_in_kernel(const char *str1, const char *str2, uint32_t max_strlen)
{
    for (uint32_t i = 0; i < max_strlen; i++) {
        if (str1[i] != str2[i]) {
            return (unsigned char)str1[i] - (unsigned char)str2[i];
        }
        if (str1[i] == '\0') {
            break;
        }
    }
    return 0;
}

int strlen_in_kernel(const char *s)
{
    int len = 0;
    while (*s++)
        len++;
    return len;
}

int strncmp(const char* str1, const char* str2, size_t n) { 
    while (n-- && *str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    if (n == (size_t)-1) return 0; // 比较了n个字符且都相等
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

void ksetmem_8(void* ptr, uint8_t value, uint64_t size_in_byte) {
    if (size_in_byte == 0) return;
    
    uint8_t* p = static_cast<uint8_t*>(ptr);
    
    // 使用 rep stos 指令（最快）
    __asm__ volatile (
        "cld\n\t"                   // 清除方向标志（向前）
        "rep stosb\n\t"            // 重复存储字节
        : "+D" (p), "+c" (size_in_byte), "+a" (value)  // 输入/输出
        :                           // 无额外输入
        : "memory", "cc"           // 可能修改内存和标志寄存器
    );
}

void ksetmem_16(void* ptr, uint16_t value, uint64_t size_in_byte) {
    if (size_in_byte == 0) return;
    uint64_t count = size_in_byte / 2;
    uint16_t* p = static_cast<uint16_t*>(ptr);
    if (count) {
        __asm__ volatile (
            "cld\n\t"
            "rep stosw\n\t"
            : "+D" (p), "+c" (count), "+a" (value)
            :
            : "memory", "cc"
        );
    }
    if (size_in_byte & 1) {
        ksetmem_8(reinterpret_cast<uint8_t*>(p), static_cast<uint8_t>(value), 1);
    }
}

void ksetmem_32(void* ptr, uint32_t value, uint64_t size_in_byte) {
    if (size_in_byte == 0) return;
    uint64_t count = size_in_byte / 4;
    uint32_t* p = static_cast<uint32_t*>(ptr);
    if (count) {
        __asm__ volatile (
            "cld\n\t"
            "rep stosl\n\t"
            : "+D" (p), "+c" (count), "+a" (value)
            :
            : "memory", "cc"
        );
    }
    uint64_t tail = size_in_byte & 3;
    if (tail) {
        ksetmem_8(reinterpret_cast<uint8_t*>(p), static_cast<uint8_t>(value), tail);
    }
}

void ksetmem_64(void* ptr, uint64_t value, uint64_t size_in_byte) {
    if (size_in_byte == 0) return;
    uint64_t count = size_in_byte / 8;
    uint64_t* p = static_cast<uint64_t*>(ptr);
    if (count) {
        __asm__ volatile (
            "cld\n\t"
            "rep stosq\n\t"
            : "+D" (p), "+c" (count), "+a" (value)
            :
            : "memory", "cc"
        );
    }
    uint64_t tail = size_in_byte & 7;
    if (tail) {
        ksetmem_8(reinterpret_cast<uint8_t*>(p), static_cast<uint8_t>(value), tail);
    }
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
void ksystemramcpy(void*src,void*dest,size_t length)
//最好用于内核内存空间内的内存拷贝，不然会出现未定义行为
{
    // 如果源地址和目标地址相同或者长度为0，无需复制
    if (src == dest || length == 0) {
        return;
    }

    // 检查内存区域是否有重叠
    bool overlap = (uint8_t*)src < (uint8_t*)dest + length && (uint8_t*)dest < (uint8_t*)src + length;

    if (!overlap) {
        // 没有重叠，可以直接使用rep movsb
        asm volatile (
            "rep movsb"
            : "=&D"(dest), "=&S"(src), "=&c"(length)
            : "0"(dest), "1"(src), "2"(length)
            : "memory"
        );
    } else {
        // 存在重叠，需要根据src和dest的相对位置决定复制方向
        if (src < dest) {
            // 从后往前复制，避免覆盖未复制的数据
            src = (uint8_t*)src + length - 1;
            dest = (uint8_t*)dest + length - 1;
            
            asm volatile (
                "std\n\t"           // 设置方向标志位，使地址递减
                "rep movsb\n\t"     // 执行复制
                "cld"               // 清除方向标志位，恢复默认递增
                : "=&D"(dest), "=&S"(src), "=&c"(length)
                : "0"(dest), "1"(src), "2"(length)
                : "memory"
            );
        } else {
            // 从前往后复制
            asm volatile (
                "rep movsb"
                : "=&D"(dest), "=&S"(src), "=&c"(length)
                : "0"(dest), "1"(src), "2"(length)
                : "memory"
            );
        }
    }
}
/**
 * 此函数只会对物理内存描述符表中的物理内存起始地址按照低到高排序
 * 一般来说这个表会是分段有序的，采取插入排序
 * 不过前面会有一大段有序的表，所以直接遍历到第一个有序子表结束后再继续遍历
 */

// 带参数的构造函数（示例）
void linearTBSerialDelete(//这是一个对于线性表删除一段连续项,起始索引a,结束索引b的函数
    uint64_t*TotalEntryCount,
    uint64_t a,
    uint64_t b,
    void*linerTbBase,
    uint32_t entrysize
)
{ 
    char*bs=(char*)linerTbBase;
    char*srcadd=bs+entrysize*(b+1);
    char*destadd=bs+entrysize*a;
    uint64_t deletedEntryCount=b-a+1;
    ksystemramcpy((void*)srcadd,(void*)destadd,entrysize*(*TotalEntryCount-b-1));
    *TotalEntryCount-=deletedEntryCount;
}
/**
 * 在描述符表中插入一个或多个连续条目
 * 
 * @param TotalEntryCount 表项总数的指针（插入后会更新）
 * @param insertIndex     插入位置的起始索引（0-based）
 * @param newEntry        要插入的新条目（单个或多个连续条目）
 * @param linerTbBase     描述符表基地址
 * @param entrysize       单个条目的大小
 * @param entryCount      要插入的条目数量（默认为1）
 */
void linearTBSerialInsert(
    uint64_t* TotalEntryCount,
    uint64_t insertIndex,
    void* newEntry,
    void* linerTbBase,
    uint32_t entrysize,
    uint64_t entryCount
) {
    if (insertIndex > *TotalEntryCount) {
        // 插入位置超出当前表范围，直接追加到末尾
        insertIndex = *TotalEntryCount;
    }
    
    char* base = (char*)linerTbBase;
    char* src = (char*)newEntry;
    
    // 计算需要移动的数据量（从插入点到表尾）
    uint64_t moveCount = *TotalEntryCount - insertIndex;
    uint64_t moveSize = moveCount * entrysize;
    
    if (moveSize > 0) {
        // 向后移动现有条目（使用内存安全拷贝）
        char* srcStart = base + insertIndex * entrysize;
        char* destStart = srcStart + entryCount * entrysize;
        ksystemramcpy(srcStart, destStart, moveSize);
    }
    
    // 插入新条目
    for (uint64_t i = 0; i < entryCount; i++) {
        char* dest = base + (insertIndex + i) * entrysize;
        ksystemramcpy(src + i * entrysize, dest, entrysize);
    }
    
    // 更新表项总数
    *TotalEntryCount += entryCount;
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
/* 带写屏障的8位原子写入 */
 void atomic_write8_wmb(volatile void *addr, uint8_t val)
{
    asm volatile("sfence\n\t"
                 "movb %1, %0"
                 : "=m" (*(volatile uint8_t *)addr)
                 : "q" (val)
                 : "memory");
}

/* 带写屏障的16位原子写入 */
  void atomic_write16_wmb(volatile void *addr, uint16_t val)
{
    asm volatile("sfence\n\t"
                 "movw %1, %0"
                 : "=m" (*(volatile uint16_t *)addr)
                 : "r" (val)
                 : "memory");
}

/* 带写屏障的32位原子写入 */
 void atomic_write32_wmb(volatile void *addr, uint32_t val)
{
    asm volatile("sfence\n\t"
                 "movl %1, %0"
                 : "=m" (*(volatile uint32_t *)addr)
                 : "r" (val)
                 : "memory");
}
/* 带写屏障的64位原子写入 */
  void atomic_write64_wmb(volatile void *addr, uint64_t val)
{
    asm volatile("sfence\n\t"
                 "movq %1, %0"
                 : "=m" (*(volatile uint64_t *)addr)
                 : "r" (val)
                 : "memory");
}
/* 带读屏障的8位原子读取 */
  uint8_t atomic_read8_rmb(volatile void *addr)
{
    uint8_t val;
    asm volatile("movb %1, %0\n\t"
                 "lfence"
                 : "=q" (val)
                 : "m" (*(volatile uint8_t *)addr)
                 : "memory");
    return val;
}

/* 带读屏障的16位原子读取 */
 uint16_t atomic_read16_rmb(volatile void *addr)
{
    uint16_t val;
    asm volatile("movw %1, %0\n\t"
                 "lfence"
                 : "=r" (val)
                 : "m" (*(volatile uint16_t *)addr)
                 : "memory");
    return val;
}

/* 带读屏障的32位原子读取 */
  uint32_t atomic_read32_rmb(volatile void *addr)
{
    uint32_t val;
    asm volatile("movl %1, %0\n\t"
                 "lfence"
                 : "=r" (val)
                 : "m" (*(volatile uint32_t *)addr)
                 : "memory");
    return val;
}
/* 带读屏障的64位原子读取 */
  uint64_t atomic_read64_rmb(volatile void *addr)
{
    uint64_t val;
    asm volatile("movq %1, %0\n\t"
                 "lfence"
                 : "=r" (val)
                 : "m" (*(volatile uint64_t *)addr)
                 : "memory");
    return val;
}

/* 带回读验证的8位原子写入 */
 void atomic_write8_rdbk(volatile void *addr, uint8_t val)
{
    *(volatile uint8_t *)addr = val;
    uint8_t read_val;
    do {
        read_val = *(volatile uint8_t *)addr;
    } while(read_val != val);  // 等待直到回读值与写入值一致
}

/* 带回读验证的16位原子写入 */
 void atomic_write16_rdbk(volatile void *addr, uint16_t val)
{
    *(volatile uint16_t *)addr = val;
    uint16_t read_val;
    do {
        read_val = *(volatile uint16_t *)addr;
    } while(read_val != val);  // 等待直到回读值与写入值一致
}

/* 带回读验证的32位原子写入 */
 void atomic_write32_rdbk(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    uint32_t read_val;
    do {
        read_val = *(volatile uint32_t *)addr;
    } while(read_val != val);  // 等待直到回读值与写入值一致
}

/* 带回读验证的64位原子写入 */
void atomic_write64_rdbk(volatile void *addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
    uint64_t read_val;
    do {
        read_val = *(volatile uint64_t *)addr;
    } while(read_val != val);  // 等待直到回读值与写入值一致
}

