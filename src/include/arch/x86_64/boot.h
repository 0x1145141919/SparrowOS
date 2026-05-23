#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "firmware/gSTResloveAPIs.h"
struct x86_specify_init_to_kernel_info{
    vm_interval hpet_mmio;
    vm_interval conjunc_GSs;          // 每个处理器一块 GS 槽区
    phyaddr_t hdstacks_interval_pbase;//必须4k对齐
    uint64_t hdstacks_4kbpgs_count;
    vaddr_t hdstacks_interval_vbase;
    GlobalBasicGraphicInfoType gop_info;
    vaddr_t Gop_vbase;
    phyaddr_t XSDT_base;
};