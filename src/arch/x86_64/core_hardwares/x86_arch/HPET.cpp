#include "arch/x86_64/core_hardwares/HPET.h"
#include "memory/all_pages_arr.h"
#include "ktime.h"
#include "memory/AddresSpace.h"
#include "util/OS_utils.h"
HPET_driver_only_read_time_stamp*readonly_timer=nullptr;
HPET_driver_only_read_time_stamp::HPET_driver_only_read_time_stamp(vm_interval* entry)
{
    phy_reg_base=entry->pbase();
    virt_reg_base=entry->vbase();
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
uint64_t HPET_driver_only_read_time_stamp::get_time_stamp_in_mius()
{
    uint64_t tmp_count=atomic_read64_rmb((void*)(virt_reg_base+HPET::regs::offset_main_counter_value));
    __uint128_t result=__uint128_t(tmp_count)*hpet_timer_period_fs/FS_per_mius;
    return uint64_t(result);
}
