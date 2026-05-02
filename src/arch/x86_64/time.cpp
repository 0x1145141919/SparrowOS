#include "ktime.h"
#include "util/OS_utils.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "arch/x86_64/core_hardwares/rtc.h"
#include "util/kout.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
// ── RTC 锚定状态 ─────────────────────────────────────────────
// 第一次调用 GetTime_in_os() 时锚定 RTC + TSC/HPET 时间戳，
// 后续调用根据自由运行时钟推算，不再读取 RTC。

static bool        g_rtc_anchored = false;
static uint64_t    g_rtc_anchor_us;        // get_microsecond_stamp() at anchor
static uint64_t    g_rtc_anchor_epoch_us;  // RTC epoch microseconds at anchor
static int64_t     g_calibration_offset_us; // modify_time 校准偏移

/**
 * @brief 将 rtc_time_t 转换为 EFI_TIME
 */



// ── RTC 锚定状态 ─────────────────────────────────────────────
static bool        g_rtc_ready = false;
static uint64_t    g_anchor_us;
static uint64_t    g_anchor_epoch_us;
static int64_t     g_calib_offset_us;

macro_tm ktime::GetTime_in_os() {
    macro_tm t;
    t.year = t.month = t.day = 0;
    t.hour = t.minute = t.second = 0;
    t.millisecond = 0;
    t.utc_offset = 0x7FFF;

    if (!g_rtc_ready) {
        // 首次调用：读 RTC 锚定
        rtc_read_time(&t);
        g_anchor_us = get_microsecond_stamp();
        g_anchor_epoch_us = macro_tm_to_epoch_sec(&t) * 1000000ULL;
        g_rtc_ready = true;
        return t;
    }

    // 后续：锚点 + 经过时间 + 校准偏移
    uint64_t now_us = get_microsecond_stamp();
    uint64_t elapsed = (now_us >= g_anchor_us) ? (now_us - g_anchor_us) : 0;
    int64_t cur_epoch_us = (int64_t)g_anchor_epoch_us
                         + (int64_t)elapsed
                         + g_calib_offset_us;
    if (cur_epoch_us < 0) cur_epoch_us = 0;
    uint64_t sec  = (uint64_t)cur_epoch_us / 1000000ULL;
    uint64_t us_f = (uint64_t)cur_epoch_us % 1000000ULL;
    epoch_sec_to_macro_tm(sec, (uint16_t)(us_f / 1000), &t);
    return t;
}

int ktime::modify_time(macro_tm time) {
    if (!g_rtc_ready) {
        // 还未锚定，直接锚定到给定时间
        g_anchor_us = get_microsecond_stamp();
        g_anchor_epoch_us = macro_tm_to_epoch_sec(&time) * 1000000ULL;
        g_calib_offset_us = 0;
        g_rtc_ready = true;
        return 0;
    }
    uint64_t given_epoch_us = macro_tm_to_epoch_sec(&time) * 1000000ULL;
    uint64_t now_us = get_microsecond_stamp();
    uint64_t elapsed = (now_us >= g_anchor_us) ? (now_us - g_anchor_us) : 0;
    int64_t current_epoch_us = (int64_t)g_anchor_epoch_us + (int64_t)elapsed;
    g_calib_offset_us = (int64_t)given_epoch_us - current_epoch_us;
    return 0;
}

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
