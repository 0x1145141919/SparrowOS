#include "abi/boot.h"
#include "init/kernel_mmu.h"
struct load_kernel_info_pack{
    kernel_mmu*kmmu;
    loaded_file_entry kernel_file_entry;
    uint64_t VM_entry_count;
    loaded_VM_interval* VM_entries;
    vaddr_t entry_vaddr;
    vaddr_t stack_bottom;
};
int kernel_load(load_kernel_info_pack&pak);
uint64_t va_alloc(uint64_t size,uint8_t align_log2);