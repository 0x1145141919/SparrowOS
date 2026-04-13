#pragma once 
#include "base.h"
#include "util/Ktemplats.h"
#include "memory/AddresSpace.h"
#include "firmware/ACPI_MCFG.h"
constexpr pgaccess ecam_mem_access=
#ifdef KERNEL_MODE
{
    .is_kernel=true,
    .is_writeable=true,
    .is_readable=true,
    .is_executable=false,  
    .is_global=true,
    .cache_strategy=UC,
};
#else
{
    .is_kernel=false,
    .is_writeable=false,
    .is_readable=true,
    .is_executable=false,  
    .is_global=false,
    .cache_strategy=UC,
};
#endif
struct ecam_node_t{
    uint16_t seg_group_number;
    uint8_t start_bus_num;
    uint16_t bus_count;
    vm_interval vminterval;
};
class ecams_container_t:public Ktemplats::list_doubly<ecam_node_t>
{
    public:
    ecams_container_t(MCFG_Table* mcfg);
};
struct prased_pcie{
    
};
extern ecams_container_t*global_container;
void pcie_text_praser();