#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/mem_init.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
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

// fast_get_processor_id / fast_get_x2apic_id 已移至
// src/arch/x86_64/Processor/fast_get_ids.asm (NASM 裸汇编)

// ── 跨处理器 ID 翻译 ─────────────────────────────────────────────────
//
// gs_complex_t::slots[PROCESSOR_ID_GS_INDEX (=1)] 的编码规范:
//   bits [31: 0] = logical processor_id
//   bits [63:32] = x2APIC ID
//
// 两个视图:
//   conjucnt_GSs[processor_id]  → 按 processor_id 索引
//   g_gs_by_apicid[x2apic_id]   → 按 x2APIC ID 查表

extern "C" uint32_t tran_get_x2apic_id(uint32_t processor_id)
{
    if (processor_id >= logical_processor_count)
        return 0xFFFFFFFF;

    gs_complex_t* cx = (gs_complex_t*)(
        conjucnt_GSs.vbase() + processor_id * GS_COMPLEX_STRIDE);
    uint64_t slot = cx->slots[PROCESSOR_ID_GS_INDEX];
    return (uint32_t)(slot >> 32);
}

extern "C" uint32_t tran_get_processor_id(uint32_t x2apic_id)
{
    gs_complex_t* cx = g_gs_by_apicid[x2apic_id];
    if (!cx)
        return 0xFFFFFFFF;

    uint64_t slot = cx->slots[PROCESSOR_ID_GS_INDEX];
    return (uint32_t)(slot & 0xFFFFFFFF);
}