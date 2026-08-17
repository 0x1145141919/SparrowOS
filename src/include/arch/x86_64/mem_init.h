#pragma once
#include "memory/memory_base.h"
#include "abi/boot.h"
extern phymem_segment *phymem_segments;
extern uint64_t phymem_segments_count; 
extern uint32_t logical_processor_count;
extern vm_interval Kspace_phyaddr_access_window;
extern phyaddr_t g_xsdt_base;
extern vm_interval conjucnt_GSs;
KURD_t mem_init();