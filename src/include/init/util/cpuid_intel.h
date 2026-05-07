#pragma once
#include <stdint.h>
namespace MAIN_FUNID{
    
}
inline void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
uint8_t query_apicid();
uint32_t query_x2apicid();
bool is_x2apic_supported();
bool is_avx_supported();