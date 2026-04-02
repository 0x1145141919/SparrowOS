#pragma once
#include <stdint.h>
namespace MAIN_FUNID{
    
}
inline void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
extern "C" 
{uint8_t query_apicid();
uint32_t query_x2apicid();
bool is_x2apic_supported();
bool is_avx_supported();
uint64_t rdmsr(uint32_t offset);
void wrmsr_func(uint32_t offset,uint64_t value);
uint64_t rdtsc();
uint64_t read_gs_u64(uint64_t index);
void gs_u64_write(uint32_t index, uint64_t value);
uint32_t fast_get_processor_id();}