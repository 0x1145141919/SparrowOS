#pragma once
#include "memory/memory_base.h"
#include "abi/boot.h"
#include "abi/bcb_handoff.h"
extern phymem_segment *phymem_segments;
extern uint64_t phymem_segments_count; 
extern uint32_t logical_processor_count;
extern vm_interval Kspace_phyaddr_access_window;
extern phyaddr_t g_xsdt_base;
// BCB 交接（exec_env_prepare 从信息包复制到堆，焚包后仍有效；收养路径消费）
extern bcb_desc_v2_t* g_bcbs;
extern uint64_t g_bcbs_count;
KURD_t mem_init();