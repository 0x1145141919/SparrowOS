#pragma once
#include <stdint.h>
#include <efi.h>
#include "abi/os_error_definitions.h"
#include "abi/arch_code.h"
typedef  uint64_t miusecond_time_stamp_t;
struct tsc_complex{
    uint64_t is_tsc_ddline_avaliabe:1;
    uint64_t is_tsc_reliable:1;
    uint64_t tsc_fs_per_cycle;
};
struct hardware_time_base_token{
    uint64_t hpet_base_cycle;
    uint64_t tsc_base;
    
    uint64_t lapic_fs_per_cycle;
};
struct time_complex{
    
    hardware_time_base_token private_token;
};

constexpr uint64_t fs_per_ns=1000000ull;
constexpr uint64_t FS_per_mius=1000000000ull;
namespace ktime
{
    
    class macro_time
    {
        private:
        public:
        static int Init();
        static EFI_TIME GetTime_in_os();  
        static int modify_time(EFI_TIME time);
    };
    struct global_time_complex{
        uint32_t arch_code;
    };
    uint64_t get_microsecond_stamp();
    uint64_t get_nanosecond_stamp();
    void microsecond_polling_delay(uint64_t microseconds);
    enum backend_choose{
            lapic_normal,
            lapic_tscddline
    };
    class time_interrupt_generator
    {
        private:
        static backend_choose back_end_type;//全局采用全同时钟中断是刻意设计
        public:
        static void bsp_init();//默认只能由bsp初始化
        static void ap_init();
        static void set_clock_by_stamp(miusecond_time_stamp_t stamp);
        static void set_clock_by_offset(miusecond_time_stamp_t offset);
        static uint64_t get_current_clock(); //注意，此接口是根据后端选择而有所不同
        static void cancel_clock();
        static bool is_alarm_set();     
        static backend_choose get_backend_type();  
    };
}