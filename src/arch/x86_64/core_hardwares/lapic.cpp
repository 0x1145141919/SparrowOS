#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "util/OS_utils.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "firmware/gSTResloveAPIs.h"
#include "memory/all_pages_arr.h"
#include "memory/AddresSpace.h"
#include "util/kout.h"
#include "ktime.h"
void x2apic::x2apic_driver::raw_config_timer(timer_lvt_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_TIMER,entry.raw);
}

void x2apic::x2apic_driver::raw_config_timer_init_count(initcout_reg_t count)
{
    wrmsr_func(msr::apic::IA32_X2APIC_TIMER_INITIAL_COUNT,(uint64_t)count);
}

void x2apic::x2apic_driver::raw_config_timer_divider(devide_reg_t reg)
{
    wrmsr_func(msr::apic::IA32_X2APIC_TIMER_DIVIDE_CONFIG,reg.raw);
}

x2apic::current_reg_t x2apic::x2apic_driver::get_timer_current_count()
{
    return current_reg_t(rdmsr(msr::apic::IA32_X2APIC_TIMER_CURRENT_COUNT));
}


void x2apic::x2apic_driver::raw_send_ipi(x2apic_icr_t icr)
{
    wrmsr_func(msr::apic::IA32_X2APIC_ICR,icr.raw);
}

void x2apic::x2apic_driver::broadcast_exself_fixed_ipi(void (*ipi_handler)())
{
    global_ipi_handler=ipi_handler;
    asm volatile (
        "sfence"    
    );
    raw_send_ipi(broadcast_exself_icr);
}

void x2apic::x2apic_driver::raw_error_lvt_config(lvt_error_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_ERROR, entry.raw);
}

void x2apic::x2apic_driver::raw_lint0_lvt_config(lvt_lint_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_LINT0, entry.raw);
}

void x2apic::x2apic_driver::raw_lint1_lvt_config(lvt_lint_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_LINT1, entry.raw);
}

void x2apic::x2apic_driver::raw_perf_lvt_config(lvt_general_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_PMI, entry.raw);
}

void x2apic::x2apic_driver::raw_thermal_lvt_config(lvt_general_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_THERMAL, entry.raw);
}

void x2apic::x2apic_driver::raw_cmci_lvt_config(lvt_general_entry entry)
{
    wrmsr_func(msr::apic::IA32_X2APIC_LVT_CMCI, entry.raw);
}
void x2apic::x2apic_driver::write_eoi()
{
    wrmsr_func(msr::apic::IA32_X2APIC_EOI,0);
}
void x2apic::lapic_timer_one_shot::processor_regist()
{
    timer_lvt_entry timer_config={
        .param{
            .vector =ivec::LAPIC_TIMER,// 此处暂不写具体动作
        .reserved1 = 0,
        .deliver_status = LAPIC_PARAMS_ENUM::DELIVERY_STATUS_T::IDLE,
        .reserved2 = 0,
        .masked = LAPIC_PARAMS_ENUM::MASK_T::NO,
        .timermode = LAPIC_PARAMS_ENUM::TIMER_MODE_T::ONE_SHOT,        
        .reserved3 = 0,
        .reserved4 = 0,
        .reserved5 = 0
        }
    };
    devide_reg_t devide_config={
        .param{
            .param_low2bit=3,
            .reserved1=0,
            .param_high1bit=1,
            .reserved2=0,
            .reserved3=0
        }
    };
    x2apic_driver::raw_config_timer(timer_config);
    x2apic_driver::raw_config_timer_divider(devide_config);
    x2apic_driver::raw_config_timer_init_count(0xFFFFFFFF);
    ktime::microsecond_polling_delay_by_hpet(10000);//这里是刻意假设不会把~0跑光
    uint64_t current=x2apic_driver::get_timer_current_count();
    time_complex*complex=(time_complex*)read_gs_u64(TIME_COMPLEX_GS_INDEX);
    complex->lapic_fs_per_cycle=(__uint128_t)(10000*(__uint128_t)FS_per_mius)/(0xFFFFFFFF-current);
    x2apic_driver::raw_config_timer_init_count(0);
    gs_u64_write(TIME_COMPLEX_GS_INDEX,(uint64_t)complex);
}
void x2apic::lapic_timer_tsc_ddline::processor_regist()
{
    x2apic_driver::raw_config_timer(ddline_timer);
}
void x2apic::lapic_timer_one_shot::set_clock_by_stamp(uint64_t stamp_mius)
{
    miusecond_time_stamp_t current=ktime::get_microsecond_stamp();
    miusecond_time_stamp_t offset=stamp_mius-current;
    return set_clock_by_offset(offset);
}
void x2apic::lapic_timer_one_shot::set_clock_by_offset(uint64_t offset_mius)
{
    time_complex*complex=(time_complex*)read_gs_u64(TIME_COMPLEX_GS_INDEX);
    uint32_t lapic_fs_per_cycle=complex->lapic_fs_per_cycle;
    uint64_t init_count=(__uint128_t)offset_mius*FS_per_mius/lapic_fs_per_cycle;
    x2apic_driver::raw_config_timer_init_count(init_count);
}
uint32_t x2apic::lapic_timer_one_shot::get_current_clock()
{
    return x2apic_driver::get_timer_current_count();
}
void x2apic::lapic_timer_one_shot::cancel_clock()
{
    x2apic_driver::raw_config_timer_init_count(0);
}
bool x2apic::lapic_timer_one_shot::is_alarm_valid()
{
    return (get_current_clock()==0)||rdmsr(msr::apic::IA32_X2APIC_TIMER_INITIAL_COUNT);
}
void x2apic::lapic_timer_tsc_ddline::set_clock_by_offset(uint64_t offset_mius)
{
    uint64_t target_tsc=rdtsc()+(__uint128_t)offset_mius*FS_per_mius/tsc_fs_per_cycle;
    wrmsr_func(msr::timer::IA32_TSC_DEADLINE,target_tsc);
}
void x2apic::lapic_timer_tsc_ddline::set_clock_by_stamp(uint64_t stamp_mius)
{
    miusecond_time_stamp_t current=ktime::get_microsecond_stamp();
    miusecond_time_stamp_t offset=stamp_mius-current;
    return set_clock_by_offset(offset);
}
void x2apic::lapic_timer_tsc_ddline::cancel_clock()
{
    wrmsr_func(msr::timer::IA32_TSC_DEADLINE,0);
}
bool x2apic::lapic_timer_tsc_ddline::is_alarm_valid()
{
    return !rdmsr(msr::timer::IA32_TSC_DEADLINE);
}
