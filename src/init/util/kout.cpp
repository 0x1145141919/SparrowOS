#include "init/util/kout.h"
#include "init/util/textConsole.h"
#include "init/core_hardwares/PortDriver.h"
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