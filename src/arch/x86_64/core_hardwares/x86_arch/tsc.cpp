#include "arch/x86_64/core_hardwares/tsc.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/textConsole.h"
#include "ktime.h"
#include "util/kout.h"
#include <global_controls.h>
uint32_t tsc_fs_per_cycle;
bool is_tsc_ddline_avaliabe;
bool is_tsc_reliable;
void tsc_regist()
{
    time_complex*complex=new time_complex;
    gs_u64_write(TIME_COMPLEX_GS_INDEX,(uint64_t)complex);
    complex->lapic_fs_per_cycle=0;
    cpuid_tmp querier(0x80000007,0);
    is_tsc_reliable=!!(querier.edx&(1<<8));
    querier.update(1,0);
    is_tsc_ddline_avaliabe=!!(querier.edx&(1<<24));
    querier.update(0x15,0);
    if(is_tsc_reliable&&is_tsc_ddline_avaliabe){
        if(querier.eax){
            uint64_t tsc_frequency=(uint64_t)querier.ecx*querier.ebx/querier.eax;
            tsc_fs_per_cycle=((__uint128_t)1000000*FS_per_mius)/tsc_frequency;
        }else{
            is_tsc_reliable=false;
            uint64_t tsc_stamp1=rdtsc();
        ktime::microsecond_polling_delay_by_hpet(10000);
        uint64_t tsc_stamp2=rdtsc();
        tsc_fs_per_cycle=((uint64_t)10000*FS_per_mius)/(tsc_stamp2-tsc_stamp1);
        }
    }else{
        uint64_t tsc_stamp1=rdtsc();
        ktime::microsecond_polling_delay_by_hpet(10000);
        uint64_t tsc_stamp2=rdtsc();
        tsc_fs_per_cycle=((uint64_t)10000*FS_per_mius)/(tsc_stamp2-tsc_stamp1);
    }
    if(!try_tsc_ddline){
        is_tsc_ddline_avaliabe=false;
    }
    // 打印 TSC 初始化信息
    
    bsp_kout << "[INFO] TSC registration completed:\n";
    bsp_kout.shift_hex();
    bsp_kout << "  is_tsc_reliable: " << is_tsc_reliable << "\n";
    bsp_kout << "  is_tsc_deadline_available: " << is_tsc_ddline_avaliabe << "\n";
    bsp_kout << "  tsc_fs_per_cycle: " << tsc_fs_per_cycle << "\n";
    bsp_kout << "  lapic_fs_per_cycle: " << (uint64_t)complex->lapic_fs_per_cycle << "\n";
    bsp_kout.shift_dec();
}
