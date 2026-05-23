#pragma once
#include "memory/memory_base.h"
#include "abi/boot.h"
extern uint64_t VM_intervals_count;
extern phymem_segment *phymem_segments;
extern uint64_t phymem_segments_count; 
extern uint32_t logical_processor_count;
extern vm_interval kIMG_self_window;//内核文件映像瞬态端 VA 窗口（仅供自省）
extern uint64_t kIMG_size;
extern vm_interval pages_arr;//直接用pages_allocator里面的页框数组但是清0,复用不转生
extern vm_interval FPA_bitmaps;//从phymem_segment* memory_map;解析可分配内存数目上限，一个页框2bit数据，使用available_meminterval_probe_keep
extern vm_interval log_buffer;//日志缓冲区，使用available_meminterval_probe
extern movable_file_entry_t symtable_file;//符号表文件，使用available_meminterval_probe
extern movable_file_entry_t initramfs_file;//initramfs文件，使用available_meminterval_probe
extern vm_interval Kspace_phyaddr_access_window;
extern vm_interval hpet_mmio;
extern vm_interval gop_buffer;
extern vm_interval conjucnt_GSs;
extern vm_interval hw_stacks;
extern phyaddr_t g_xsdt_base;
extern phymem_segment legacy_mmu_interval;
KURD_t mem_init();