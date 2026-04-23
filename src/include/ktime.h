#pragma once
#include <stdint.h>
#include <efi.h>
#include "abi/os_error_definitions.h"
#include "abi/arch_code.h"
typedef  uint64_t miusecond_time_stamp_t;
constexpr uint64_t fs_per_ns=1000000ull;
constexpr uint64_t FS_per_mius=1000000000ull;
namespace ktime
{
    EFI_TIME GetTime_in_os();  
    int modify_time(EFI_TIME time);
    uint64_t get_microsecond_stamp();
    uint64_t get_nanosecond_stamp();
    void microsecond_polling_delay_by_hpet(uint64_t microseconds);
    namespace heart_beat_alarm
    {
    void processor_regist();//在x86_64上必须要先tsc_regist后再运行
    void set_clock_by_stamp(miusecond_time_stamp_t stamp);
    void set_clock_by_offset(miusecond_time_stamp_t offset);
    void cancel_clock();
    bool is_alarm_set(); 
    }    
}