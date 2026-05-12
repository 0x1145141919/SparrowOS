#pragma once
#include "memory/memory_base.h"
extern uint64_t VM_intervals_count;
extern phymem_segment *phymem_segments;
extern uint64_t phymem_segments_count; 
extern uint32_t logical_processor_count;
extern vm_interval kIMG_self_window;//内核自身连续物理地址需要被映射，使用available_meminterval_probe_keep
extern vm_interval kBSS_interval;//内核唯一的bss区连续，使用available_meminterval_probe_keep
extern vm_interval pages_arr;//直接用pages_allocator里面的页框数组但是清0,复用不转生
extern vm_interval FPA_bitmaps;//从phymem_segment* memory_map;解析可分配内存数目上限，一个页框2bit数据，使用available_meminterval_probe_keep
extern vm_interval log_buffer;//日志缓冲区，使用available_meminterval_probe
extern vm_interval kernel_entry_stack;//BSP初始化栈，使用available_meminterval_probe
extern vm_interval symtable_file;//符号表文件，使用available_meminterval_probe
extern vm_interval initramfs_file;//initramfs文件，使用available_meminterval_probe
extern vm_interval identity_map_window;//不但要[0,dram_top)进行va_alloc进行映射，而且要对于[16k,dram_top)进行WB+RWX的恒等映射
extern vm_interval hpet_mmio;
extern vm_interval gop_buffer;
extern phymem_segment legacy_mmu_interval;
KURD_t mem_init();