#include "../init/include/util/kout.h"
#include "../init/include/util/textConsole.h"
#include "../init/include/core_hardwares/PortDriver.h"
#include "util/OS_utils.h"

// 全局 hex_chars 表，避免 protected 访问问题
namespace {
static constexpr char hex_chars_table[16] = {
    '0','1','2','3','4','5','6','7',
    '8','9','A','B','C','D','E','F'
};
}

namespace kio {





// ============================================================================
// kout 类实现
// ============================================================================

void kout::__print_level_code(KURD_t value)
{
    switch (value.level) {
        case level_code::INVALID: *this << "[level:INVALID]"; break;
        case level_code::INFO: *this << "[level:INFO]"; break;
        case level_code::NOTICE: *this << "[level:NOTICE]"; break;
        case level_code::WARNING: *this << "[level:WARNING]"; break;
        case level_code::ERROR: *this << "[level:ERROR]"; break;
        case level_code::FATAL: *this << "[level:FATAL]"; break;
        default: *this << "[level:unknown]"; break;
    }
}

void kout::__print_result_code(KURD_t value)
{
    switch (value.result) {
        case result_code::SUCCESS: *this << "[result:SUCCESS]"; break;
        case result_code::SUCCESS_BUT_SIDE_EFFECT: *this << "[result:SUCCESS_BUT_SIDE_EFFECT]"; break;
        case result_code::PARTIAL_SUCCESS: *this << "[result:PARTIAL_SUCCESS]"; break;
        case result_code::FAIL: *this << "[result:FAIL]"; break;
        case result_code::RETRY: *this << "[result:RETRY]"; break;
        case result_code::FATAL: *this << "[result:FATAL]"; break;
        default: *this << "[result:unknown]"; break;
    }
}

void kout::__print_err_domain(KURD_t value)
{
    switch (value.domain) {
        case err_domain::INVALID: *this << "[err_domain:INVALID]"; break;
        case err_domain::CORE_MODULE: *this << "[err_domain:CORE_MODULE]"; break;
        case err_domain::ARCH: *this << "[err_domain:ARCH]"; break;
        case err_domain::USER: *this << "[err_domain:USER]"; break;
        case err_domain::HYPERVISOR: *this << "[err_domain:HYPERVISOR]"; break;
        case err_domain::OUT_MODULES: *this << "[err_domain:OUT_MODULES]"; break;
        case err_domain::FILE_SYSTEM: *this << "[err_domain:FILE_SYSTEM]"; break;
        case err_domain::HARDWARE: *this << "[err_domain:HARDWARE]"; break;
        default: *this << "[err_domain:unknown]"; break;
    }
}

void kout::__print_module_code(KURD_t value)
{
    switch (value.module_code) {
        case module_code::INVALID: *this << "[module_code:INVALID]"; break;
        case module_code::MEMORY: *this << "[module_code:MEMORY]"; break;
        case module_code::SCHEDULER: *this << "[module_code:SCHEDULER]"; break;
        case module_code::INTERRUPT: *this << "[module_code:INTERRUPT]"; break;
        case module_code::FIRMWARE: *this << "[module_code:FIRMWARE]"; break;
        case module_code::VFS: *this << "[module_code:VFS]"; break;
        case module_code::VMM: *this << "[module_code:VMM]"; break;
        case module_code::INFRA: *this << "[module_code:INFRA]"; break;
        case module_code::DEVICE: *this << "[module_code:DEVICES]"; break;
        case module_code::DEVICES_CORE: *this << "[module_code:DEVICES_CORE]"; break;
        case module_code::HARDWARE_DEBUG: *this << "[module_code:HARDWARE_DEBUG]"; break;
        case module_code::USER_KERNEL_ABI: *this << "[module_code:USER_KERNEL_ABI]"; break;
        case module_code::TIME: *this << "[module_code:TIME]"; break;
        case module_code::PANIC: *this << "[module_code:PANIC]"; break;
        default: *this << "[module_code:unknown]"; break;
    }
}

void kout::print_numer(
    uint64_t* num_ptr,
    numer_system_select numer_system,
    uint8_t len_in_bytes,
    bool is_signed)
{
    char buf[70];
    char out[70];
    uint32_t idx = 0;
    
    uint64_t value = 0;
    switch (len_in_bytes) {
        case 1: value = *(uint8_t*)num_ptr; break;
        case 2: value = *(uint16_t*)num_ptr; break;
        case 4: value = *(uint32_t*)num_ptr; break;
        case 8: value = *(uint64_t*)num_ptr; break;
        default: return;
    }
    
    bool negative = false;
    if (numer_system == DEC && is_signed) {
        int64_t signed_val = 0;
        switch (len_in_bytes) {
            case 1: signed_val = *(int8_t*)num_ptr; break;
            case 2: signed_val = *(int16_t*)num_ptr; break;
            case 4: signed_val = *(int32_t*)num_ptr; break;
            case 8: signed_val = *(int64_t*)num_ptr; break;
        }
        if (signed_val < 0) {
            negative = true;
            value = (uint64_t)(-signed_val);
        }
    }
    
    if (value == 0) {
        buf[idx++] = '0';
    } else {
        uint32_t base = 10;
        if (numer_system == BIN) base = 2;
        else if (numer_system == HEX) base = 16;
        
        while (value > 0) {
            uint32_t digit = value % base;
            value /= base;
            if (base == 16)
                buf[idx++] = hex_chars_table[digit];
            else
                buf[idx++] = '0' + digit;
        }
    }
    
    if (negative) {
        buf[idx++] = '-';
    }
    
    for (uint32_t i = 0; i < idx; ++i) {
        out[i] = buf[idx - 1 - i];
    }
    
    uniform_puts(out, idx);
    statistics.total_printed_chars += idx;
}

void kout::uniform_puts(const char* str, uint64_t len)
{
    if (!str || len == 0) return;
    
    for (uint64_t i = 0; i < MAX_BACKEND_COUNT; i++) {
        kout_backend* backend = backends[i];
        if (!backend || backend->is_masked) continue;
        if (backend->write) {
            backend->write(str, len);
        }
    }
}
kio::kout &kio::kout::operator<<(endl end)
{
    statistics.explicit_endl++;
    *this<<'\n';
    return *this;
}
kout& kout::operator<<(KURD_t info)
{
    __print_level_code(info);
    __print_result_code(info);
    __print_err_domain(info);
    __print_module_code(info);
    return *this;
}

kout& kout::operator<<(const char* str)
{
    uint64_t strlength = strlen_in_kernel(str);
    uniform_puts(str, strlength);
    statistics.calls_str++;
    statistics.total_printed_chars += strlength;
    return *this;
}

kout& kout::operator<<(char c)
{
    uniform_puts(&c, 1);
    statistics.calls_char++;
    statistics.total_printed_chars++;
    return *this;
}

kout& kout::operator<<(const void* ptr)
{
    uniform_puts("0x", 2);
    uint64_t address = (uint64_t)ptr;
    print_numer(&address, HEX, sizeof(void*), false);
    statistics.calls_ptr++;
    return *this;
}

kout& kout::operator<<(uint64_t num)
{
    statistics.calls_u64++;
    print_numer(&num, curr_numer_system, 8, false);
    return *this;
}

kout& kout::operator<<(int64_t num)
{
    statistics.calls_s64++;
    print_numer((uint64_t*)&num, curr_numer_system, 8, true);
    return *this;
}

kout& kout::operator<<(uint32_t num)
{
    statistics.calls_u32++;
    print_numer((uint64_t*)&num, curr_numer_system, 4, false);
    return *this;
}

kout& kout::operator<<(int32_t num)
{
    statistics.calls_s32++;
    print_numer((uint64_t*)&num, curr_numer_system, 4, true);
    return *this;
}

kout& kout::operator<<(uint16_t num)
{
    statistics.calls_u16++;
    print_numer((uint64_t*)&num, curr_numer_system, 2, false);
    return *this;
}

kout& kout::operator<<(int16_t num)
{
    statistics.calls_s16++;
    print_numer((uint64_t*)&num, curr_numer_system, 2, true);
    return *this;
}

kout& kout::operator<<(uint8_t num)
{
    statistics.calls_u8++;
    print_numer((uint64_t*)&num, curr_numer_system, 1, false);
    return *this;
}

kout& kout::operator<<(int8_t num)
{
    statistics.calls_s8++;
    print_numer((uint64_t*)&num, curr_numer_system, 1, true);
    return *this;
}

void kout::shift_bin()
{
    curr_numer_system = BIN;
    statistics.calls_shift_bin++;
}

void kout::shift_dec()
{
    curr_numer_system = DEC;
    statistics.calls_shift_dec++;
}

void kout::shift_hex()
{
    curr_numer_system = HEX;
    statistics.calls_shift_hex++;
}

kout::kout_statistics_t kout::get_statistics()
{
    return statistics;
}

void kout::Init()
{
    ksetmem_8(&statistics, 0, sizeof(statistics));
    curr_numer_system = DEC;
}

uint64_t kout::register_backend(kout_backend backend)
{
    for (uint64_t i = 0; i < MAX_BACKEND_COUNT; i++) {
        if (!backends[i]) {
            backends[i] = new kout_backend;
            *backends[i] = backend;
            return i;
        }
    }
    return ~0ULL;
}

bool kout::unregister_backend(uint64_t index)
{
    if (index >= MAX_BACKEND_COUNT) return false;
    if (backends[index]) {
        delete backends[index];
        backends[index] = nullptr;
        return true;
    }
    return false;
}

bool kout::mask_backend(uint64_t index)
{
    if (index >= MAX_BACKEND_COUNT) return false;
    if (backends[index]) {
        backends[index]->is_masked = !backends[index]->is_masked;
        return true;
    }
    return false;
}
kout &kout::operator<<(numer_system_select radix)
{   
     switch (radix) {
        case BIN: shift_bin(); break;
        case DEC: shift_dec(); break;
        case HEX: shift_hex(); break;
        default: shift_dec(); break;
    }
    return *this;
}
} // namespace kio
kio::kout bsp_kout;
kio::endl kendl;