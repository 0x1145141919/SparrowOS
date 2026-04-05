#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#ifdef USER_MODE
#include <sched.h>
#endif
inline void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx){
    asm volatile(
        "cpuid"
        : "=a"(*eax),
          "=b"(*ebx),
          "=c"(*ecx),
          "=d"(*edx)
        : "a"(*eax),
          "b"(*ebx),
          "c"(*ecx),
          "d"(*edx)
    );
}
uint8_t query_apicid(){
    uint32_t eax=1, ebx, ecx, edx;
    cpuid(&eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF;
}
uint32_t query_x2apicid(){
    uint32_t eax=0xB, ebx, ecx=1, edx;
    cpuid(&eax, &ebx, &ecx, &edx);
    return edx;
}
bool is_x2apic_supported(){
    uint32_t eax=1, ebx, ecx=0, edx;
    cpuid(&eax, &ebx, &ecx, &edx);
    return ecx & (1 << 21);
}
bool is_avx_supported(){
    uint32_t eax=1, ebx, ecx=0, edx;
    cpuid(&eax, &ebx, &ecx, &edx);
    return ecx & (1 << 28);
}

uint64_t rdmsr(uint32_t offset)
{
    uint32_t value_high, value_low; 
    asm volatile("rdmsr"
                 : "=a" (value_low),
                   "=d" (value_high)
                 : "c" (offset));
    return ((uint64_t)value_high << 32) | value_low;
}
void wrmsr_func(uint32_t offset, uint64_t value)
{
    uint32_t value_high=(value>>32)&0xffffffff, value_low=value&0xffffffff; 
    asm volatile("wrmsr"
                 :
                 : "c" (offset),
                   "a" (value_low),
                   "d" (value_high));
}
uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}
uint64_t read_gs_u64(uint64_t index)
{
    uint64_t value = 0;
    uint64_t offset = index * sizeof(uint64_t);
    
    asm volatile(
        "movq %%gs:(%[offset]), %[value]"
        : [value] "=r" (value)
        : [offset] "r" (offset)
        : "memory"
    );
    return value;
}

void gs_u64_write(uint32_t index, uint64_t value)
{
    uint64_t offset = index * sizeof(uint64_t);
    
    asm volatile(
        "movq %[val], %%gs:(%[offset])"
        :
        : [offset] "r" (offset),
          [val] "r" (value)
        : "memory"
    );
}

uint32_t fast_get_processor_id()
{
#if defined(USER_MODE)
    return sched_getcpu();
#elif defined(KERNEL_MODE)
    uint32_t processor_id;
    asm volatile(
        "movl %%gs:%P1, %0"    // 从GS段选择器的指定偏移读取
        : "=r" (processor_id)         // 输出操作数
        : "i" (PROCESSOR_ID_GS_INDEX*8)  // 输入操作数，PROCESSOR_ID_GS_OFFSET通常是常数
    );
    return processor_id;
#else
    // Default fallback for unknown mode
    return 0;
#endif
}