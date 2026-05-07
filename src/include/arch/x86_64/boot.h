#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "firmware/gSTResloveAPIs.h"
struct x86_specify_init_to_kernel_info{
    vm_interval hpet_mmio;
    GlobalBasicGraphicInfoType gop_info;
    vaddr_t Gop_vbase; 
    phyaddr_t XSDT_base;
};