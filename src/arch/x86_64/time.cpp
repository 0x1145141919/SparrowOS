#include "ktime.h"
#include "util/OS_utils.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "util/kout.h"

miusecond_time_stamp_t ktime::get_microsecond_stamp( )
{
    if(is_tsc_reliable){
        return (rdtsc()/FS_per_mius)*tsc_fs_per_cycle;
    }else{
        return readonly_timer->get_time_stamp_in_mius();
    }
}
uint64_t ktime::get_nanosecond_stamp()
{
    if(is_tsc_reliable){
        return (rdtsc()/fs_per_ns)*tsc_fs_per_cycle;
    }else{
        return 0;
    }
}
constexpr uint64_t calibrated_cycles_per_us = 5000;
void ktime::microsecond_polling_delay_by_hpet(uint64_t microseconds)
{
    uint64_t now=get_microsecond_stamp();
    while(get_microsecond_stamp()-now<microseconds){
        asm("pause");
    }
}
void ktime::heart_beat_alarm::processor_regist()
{
    time_complex* complex=new time_complex;
    gs_u64_write(TIME_COMPLEX_GS_INDEX,(uint64_t)complex);
    if(is_tsc_ddline_avaliabe){
        x2apic::lapic_timer_tsc_ddline::processor_regist();
    }else{
        x2apic::lapic_timer_one_shot::processor_regist();
    }
}
void ktime::heart_beat_alarm::set_clock_by_stamp(miusecond_time_stamp_t stamp)
{
    time_complex* complex=(time_complex*)read_gs_u64(TIME_COMPLEX_GS_INDEX);
    if(is_tsc_ddline_avaliabe){
        x2apic::lapic_timer_tsc_ddline::set_clock_by_stamp(stamp);
    }else{
        x2apic::lapic_timer_one_shot::set_clock_by_stamp(stamp);
    }
}
void ktime::heart_beat_alarm::set_clock_by_offset(miusecond_time_stamp_t stamp)
{
    time_complex* complex=(time_complex*)read_gs_u64(TIME_COMPLEX_GS_INDEX);
    if(is_tsc_ddline_avaliabe){
        x2apic::lapic_timer_tsc_ddline::set_clock_by_offset(stamp);
    }else{
        x2apic::lapic_timer_one_shot::set_clock_by_offset(stamp);
    }
}
void ktime::heart_beat_alarm::cancel_clock()
{
    time_complex* complex=(time_complex*)read_gs_u64(TIME_COMPLEX_GS_INDEX);
    if(is_tsc_ddline_avaliabe){
        x2apic::lapic_timer_tsc_ddline::cancel_clock();
    }else{
        x2apic::lapic_timer_one_shot::cancel_clock();
    }
}
bool ktime::heart_beat_alarm::is_alarm_set()
{
    time_complex* complex=(time_complex*)read_gs_u64(TIME_COMPLEX_GS_INDEX);
    if(is_tsc_ddline_avaliabe){
        return x2apic::lapic_timer_tsc_ddline::is_alarm_valid();
    }else{
        return x2apic::lapic_timer_one_shot::is_alarm_valid();
    }
}
