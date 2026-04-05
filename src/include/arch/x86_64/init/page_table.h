#include "arch/x86_64/abi/pgtable45.h"
#include "memory/memory_base.h"
/**
 * 此为init.elf的特制页表结构，在x86_64下为恒等映射但是由于x86_64的特性而采用页表做MAMU(memory access manage unit)
 * [0~0xffffffff]为低4GB区域，统一到_low_4gb_pte_arr数据结构控制每个页表的访问权限，缓存策略
 * [0xffffffff~0x40000000000]为高4GB区域，统一到pdpte_arr数据结构的idx>=4的表项控制每个页表的访问权限，缓存策略
 */
extern "C" PageTableEntryUnion _low_4gb_pte_arr[0x100000];
extern "C" PageTableEntryUnion pdpte_arr[0x1000];
int modify_access(phymem_segment seg,pgaccess access);