#include "KImage_Introspection.h"
#include "abi/asset_names.h"
#include "boot/asset_table.h"
vm_interval running_code;
vm_interval running_rodata;
vm_interval running_data;
vm_interval running_bss;
void self_introspection_init()
{
    asset_table_entry*tmp;
    tmp=(asset_table_entry*)g_asset_table->read(asset_names::kernel_code);
    running_code=*(vm_interval*)tmp->data;
    tmp=(asset_table_entry*)g_asset_table->read(asset_names::kernel_bss);
    running_bss=*(vm_interval*)tmp->data;
    tmp=(asset_table_entry*)g_asset_table->read(asset_names::kernel_data);
    running_data=*(vm_interval*)tmp->data;
    tmp=(asset_table_entry*)g_asset_table->read(asset_names::kernel_rodata);
    running_rodata=*(vm_interval*)tmp->data;
}

phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr)
{
    if(running_bss.vaddr_belong(vaddr)){
        return running_bss.pbase()+(vaddr-running_bss.vbase());
    }else{
        return ~0ull;
    };
}
