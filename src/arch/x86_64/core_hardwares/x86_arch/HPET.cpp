#include "arch/x86_64/core_hardwares/HPET.h"
#include "memory/all_pages_arr.h"
#include "ktime.h"
#include "memory/AddresSpace.h"
#include "util/OS_utils.h"
HPET_driver_only_read_time_stamp*readonly_timer=nullptr;
HPET_driver_only_read_time_stamp::HPET_driver_only_read_time_stamp(loaded_VM_interval* entry)
{
    if(entry->VM_interval_specifyid!=VM_ID_HPET_MMIO){
        //panic
    }
    phy_reg_base=entry->pbase;
    virt_reg_base=entry->vbase;
    hpet_timer_period_fs=atomic_read32_rmb((void*)(virt_reg_base+HPET::regs::offset_General_Capabilities_and_ID+4));

}
KURD_t HPET_driver_only_read_time_stamp::default_kurd()
{
    return KURD_t(0,0,module_code::DEVICES_CORE,COREHARDWARES_LOCATIONS::LOCATION_CODE_HPET,0,0,err_domain::CORE_MODULE);
}
KURD_t HPET_driver_only_read_time_stamp::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=result_code::SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}
HPET_driver_only_read_time_stamp::~HPET_driver_only_read_time_stamp()
{
    KURD_t status = KURD_t();
    pgaccess access=KspacePageTable::PG_RW;
    access.cache_strategy=UC;
    int result= kspace_vm_table->remove(virt_reg_base);
    if(result!=OS_SUCCESS){
        return;
    }
    vm_interval interval={
        .vbase=virt_reg_base,
        .pbase=phy_reg_base,
        .size=4096,
        .access=access
    };
    status=KspacePageTable::disable_VMentry(interval);
    if(error_kurd(status)){
        //
        return;
    }
    uint64_t gen_config_reg=atomic_read64_rmb((void*)(virt_reg_base+HPET::regs::offset_General_Config));
    gen_config_reg &= ~HPET::regs::GCONFIG_ENABLE_BIT;
    atomic_write64_rdbk((void*)(virt_reg_base+HPET::regs::offset_General_Config), gen_config_reg);
    phy_reg_base = 0;
    virt_reg_base = 0;
    hpet_timer_period_fs = 0;
    comparator_count = 0;   
}

uint64_t HPET_driver_only_read_time_stamp::get_time_stamp_in_mius()
{
    uint64_t tmp_count=atomic_read64_rmb((void*)(virt_reg_base+HPET::regs::offset_main_counter_value));
    __uint128_t result=__uint128_t(tmp_count)*hpet_timer_period_fs/FS_per_mius;
    return uint64_t(result);
}
