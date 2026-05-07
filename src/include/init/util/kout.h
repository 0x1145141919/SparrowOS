#pragma once
#include "abi/os_error_definitions.h"
#include "util/OS_utils.h"

namespace kio {

class endl {
public:
    endl() {}
};

enum radix_shift_t : uint8_t {
    BIN_shift = 0,
    DEC_shift = 1,
    HEX_shift = 2
};

struct kout_backend {
    char name[64];
    uint64_t is_masked : 1;
    uint64_t reserved : 63;
    void (*write)(const char* buf, uint64_t len);
};

class kout {
public:
    struct kout_statistics_t {
        uint64_t total_printed_chars;
        uint64_t calls_str;
        uint64_t calls_char;
        uint64_t calls_ptr;
        uint64_t calls_u8;
        uint64_t calls_s8;
        uint64_t calls_u16;
        uint64_t calls_s16;
        uint64_t calls_u32;
        uint64_t calls_s32;
        uint64_t calls_u64;
        uint64_t calls_s64;
        uint64_t calls_KURD;
        uint64_t calls_shift_bin;
        uint64_t calls_shift_dec;
        uint64_t calls_shift_hex;
        uint64_t explicit_endl;
    };

protected:
    kout_statistics_t statistics;
    numer_system_select curr_numer_system;
    
public:
    static constexpr char hex_chars[16] = {
        '0','1','2','3','4','5','6','7',
        '8','9','A','B','C','D','E','F'
    };
    
protected:
    static constexpr uint16_t MAX_STRING_LEN = 4096;
    static constexpr uint16_t MAX_BACKEND_COUNT = 2;
    
    kout_backend* backends[MAX_BACKEND_COUNT] = {0};
    
    void print_numer(
        uint64_t* num_ptr,
        numer_system_select numer_system,
        uint8_t len_in_bytes,
        bool is_signed);
    
    void uniform_puts(const char* str, uint64_t len);
    
    void __print_level_code(KURD_t value);
    void __print_module_code(KURD_t value);
    void __print_result_code(KURD_t value);
    void __print_err_domain(KURD_t value);

public:
    kout& operator<<(KURD_t info);
    kout& operator<<(const char* str);
    kout& operator<<(char c);
    kout& operator<<(const void* ptr);
    kout& operator<<(uint64_t num);
    kout& operator<<(int64_t num);
    kout& operator<<(uint32_t num);
    kout& operator<<(int32_t num);
    kout& operator<<(endl end);
    kout& operator<<(uint16_t num);
    kout& operator<<(int16_t num);
    kout& operator<<(uint8_t num);
    kout& operator<<(int8_t num); 
    kout& operator<<(numer_system_select radix);   
    uint64_t register_backend(kout_backend backend);
    bool unregister_backend(uint64_t index);
    bool mask_backend(uint64_t index);
    
    void shift_bin();
    void shift_dec();
    void shift_hex();
    
    kout_statistics_t get_statistics();
    void Init();
    ~kout() {}
};



} // namespace kio
extern kio::kout bsp_kout;
extern kio::endl kendl;
