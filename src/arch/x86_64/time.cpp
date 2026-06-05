#include "ktime.h"
#include "util/OS_utils.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "arch/x86_64/core_hardwares/rtc.h"
#include "util/kout.h"
#include "exec_env_detect.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"

// ── RTC 锚定状态 ─────────────────────────────────────────────
static bool        g_rtc_ready        = false;
static uint64_t    g_anchor_us;             // get_microsecond_stamp() at anchor
static uint64_t    g_anchor_epoch_us;       // RTC epoch microseconds at anchor
static int64_t     g_calib_offset_us;       // modify_time 校准偏移

// ── TSC → 时间 换算 ──────────────────────────────────────────
//
// tsc_fs_per_cycle = TSC 每 tick 的 femtoseconds
//   microsecond = ( tsc_ticks * tsc_fs_per_cycle ) / FS_per_mius
//   nanosecond  = ( tsc_ticks * tsc_fs_per_cycle ) / fs_per_ns
//
// 用 __uint128_t 避免乘法溢出。

static inline uint64_t tsc_to_us(uint64_t ticks)
{
    return (uint64_t)((__uint128_t)ticks * tsc_fs_per_cycle / FS_per_mius);
}

static inline uint64_t tsc_to_ns(uint64_t ticks)
{
    return (uint64_t)((__uint128_t)ticks * tsc_fs_per_cycle / fs_per_ns);
}

/*
 * 时间戳分派：
 *
 *   TCG        → HPET（readonly_timer->get_time_stamp_in_us()）
 *   KVM/BARE   → TSC（tsc_to_us）
 *                 注意：KVM 下 guest TSC = host_TSC×ratio+offset，从 0 开始单调增长，
 *                        tsx_fs_per_cycle 来自 pvclock，包含 scaling 校正。
 *                        BARE_METAL 下 tsc_fs_per_cycle 来自 CPUID 0x15 或 HPET 校准。
 */
miusecond_time_stamp_t ktime::get_microsecond_stamp()
{
    if (g_env == ENV_TCG)
        return readonly_timer->get_time_stamp_in_us();

    // KVM / Bare metal
    return tsc_to_us(rdtsc());
}

uint64_t ktime::get_nanosecond_stamp()
{
    if (g_env == ENV_TCG)
        return 0;

    // KVM / Bare metal
    return tsc_to_ns(rdtsc());
}

// ── 忙等 ─────────────────────────────────────────────────────
void ktime::microsecond_polling(uint64_t microseconds)
{
    uint64_t deadline = get_microsecond_stamp() + microseconds;
    while (get_microsecond_stamp() < deadline)
        asm volatile("pause");
}

// ── RTC 时间 ─────────────────────────────────────────────────
macro_tm ktime::GetTime_in_os()
{
    macro_tm t = {};
    t.utc_offset = 0x7FFF;

    if (!g_rtc_ready) {
        rtc_read_time(&t);
        g_anchor_us = get_microsecond_stamp();
        g_anchor_epoch_us = macro_tm_to_epoch_sec(&t) * 1000000ULL;
        g_rtc_ready = true;
        return t;
    }

    uint64_t now_us   = get_microsecond_stamp();
    uint64_t elapsed  = (now_us >= g_anchor_us) ? (now_us - g_anchor_us) : 0;
    int64_t  cur_epoch_us = (int64_t)g_anchor_epoch_us
                          + (int64_t)elapsed
                          + g_calib_offset_us;
    if (cur_epoch_us < 0)
        cur_epoch_us = 0;

    uint64_t sec  = (uint64_t)cur_epoch_us / 1000000ULL;
    uint64_t us_f = (uint64_t)cur_epoch_us % 1000000ULL;
    epoch_sec_to_macro_tm(sec, (uint16_t)(us_f / 1000), &t);
    return t;
}

int ktime::modify_time(macro_tm time)
{
    uint64_t given_epoch_us = macro_tm_to_epoch_sec(&time) * 1000000ULL;

    if (!g_rtc_ready) {
        g_anchor_us       = get_microsecond_stamp();
        g_anchor_epoch_us = given_epoch_us;
        g_calib_offset_us = 0;
        g_rtc_ready = true;
        return 0;
    }

    uint64_t now_us   = get_microsecond_stamp();
    uint64_t elapsed  = (now_us >= g_anchor_us) ? (now_us - g_anchor_us) : 0;
    int64_t  cur_epoch_us = (int64_t)g_anchor_epoch_us + (int64_t)elapsed;
    g_calib_offset_us = (int64_t)given_epoch_us - cur_epoch_us;
    return 0;
}

// ── Heart beat alarm ─────────────────────────────────────────
//
// TCG        → lapic_timer_one_shot（虚拟时钟）
// KVM/BARE   → lapic_timer_tsc_ddline（硬件 TSC deadline）

void ktime::heart_beat_alarm::processor_regist()
{
    if (g_env == ENV_TCG)
        x2apic::lapic_timer_one_shot::processor_regist();
    else
        x2apic::lapic_timer_tsc_ddline::processor_regist();
}

void ktime::heart_beat_alarm::set_clock_by_stamp(miusecond_time_stamp_t stamp)
{
    if (g_env == ENV_TCG)
        x2apic::lapic_timer_one_shot::set_clock_by_stamp(stamp);
    else
        x2apic::lapic_timer_tsc_ddline::set_clock_by_stamp(stamp);
}

void ktime::heart_beat_alarm::set_clock_by_offset(miusecond_time_stamp_t offset)
{
    if (g_env == ENV_TCG)
        x2apic::lapic_timer_one_shot::set_clock_by_offset(offset);
    else
        x2apic::lapic_timer_tsc_ddline::set_clock_by_offset(offset);
}

void ktime::heart_beat_alarm::cancel_clock()
{
    if (g_env == ENV_TCG)
        x2apic::lapic_timer_one_shot::cancel_clock();
    else
        x2apic::lapic_timer_tsc_ddline::cancel_clock();
}

bool ktime::heart_beat_alarm::is_alarm_set()
{
    if (g_env == ENV_TCG)
        return x2apic::lapic_timer_one_shot::is_alarm_valid();
    else
        return x2apic::lapic_timer_tsc_ddline::is_alarm_valid();
}
