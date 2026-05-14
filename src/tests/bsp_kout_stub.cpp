// ════════════════════════════════════════════════════════════════
// bsp_kout_stub.cpp — 用户态 kout (kio::kout) 垫片
//
// 匹配原生 kout.h 声明，只实现 FreePagesAllocator_BCB.cpp 实际调用的操作
// 输出重定向到 stderr
// ════════════════════════════════════════════════════════════════

#include "util/kout.h"
#include <cstdio>
#include <unistd.h>

void kio::kout::Init() {}
void kio::kout::shift_dec() { curr_numer_system = DEC; }
void kio::kout::shift_bin()  { curr_numer_system = BIN; }
void kio::kout::shift_hex()  { curr_numer_system = HEX; }

void kio::kout::raw_puts_and_count(const char* str, uint64_t len) {
    write(2, str, len);
}

void kio::kout::uniform_puts(const char* str, uint64_t len) {
    raw_puts_and_count(str, len);
}

kio::kout& kio::kout::operator<<(const char* str) {
    if (str) raw_puts_and_count(str, __builtin_strlen(str));
    return *this;
}
kio::kout& kio::kout::operator<<(char c) { raw_puts_and_count(&c,1); return *this; }
kio::kout& kio::kout::operator<<(const void* ptr) {
    char buf[32]; int n=snprintf(buf,32,"%p",ptr); if(n>0) raw_puts_and_count(buf,n); return *this;
}
kio::kout& kio::kout::operator<<(uint64_t num) {
    char buf[32]; int n=snprintf(buf,32,"%lu",(unsigned long)num); if(n>0) raw_puts_and_count(buf,n); return *this;
}
kio::kout& kio::kout::operator<<(int64_t num) {
    char buf[32]; int n=snprintf(buf,32,"%ld",(long)num); if(n>0) raw_puts_and_count(buf,n); return *this;
}
kio::kout& kio::kout::operator<<(uint32_t n) { return operator<<((uint64_t)n); }
kio::kout& kio::kout::operator<<(int32_t n)  { return operator<<((int64_t)n); }
kio::kout& kio::kout::operator<<(uint16_t n) { return operator<<((uint64_t)n); }
kio::kout& kio::kout::operator<<(int16_t n)  { return operator<<((int64_t)n); }
kio::kout& kio::kout::operator<<(uint8_t n)  { return operator<<((uint64_t)n); }
kio::kout& kio::kout::operator<<(int8_t n)   { return operator<<((int64_t)n); }
kio::kout& kio::kout::operator<<(kio::endl)    { raw_puts_and_count("\n",1); return *this; }
kio::kout& kio::kout::operator<<(numer_system_select r) { curr_numer_system=r; return *this; }
kio::kout& kio::kout::operator<<(kio::now_time) { return *this; }
kio::kout& kio::kout::operator<<(KURD_t) { return *this; }
kio::kout& kio::kout::operator<<(kio::tmp_buff&) { return *this; }

void kio::kout::print_numer(uint64_t*,numer_system_select,uint8_t,bool) {}
kio::kout::kout_statistics_t kio::kout::get_statistics() { return statistics; }
uint64_t kio::kout::register_backend(kout_backend) { return ~0ULL; }
bool kio::kout::unregister_backend(uint64_t) { return false; }
bool kio::kout::mask_backend(uint64_t) { return false; }
void kio::kout::__print_level_code(KURD_t) {}
void kio::kout::__print_module_code(KURD_t) {}
void kio::kout::__print_result_code(KURD_t) {}
void kio::kout::__print_err_domain(KURD_t) {}
void kio::kout::__print_event_hex(uint8_t) {}
void kio::kout::__print_memmodule_kurd(KURD_t) {}
void kio::defalut_KURD_module_interpator(KURD_t) {}

kio::kout bsp_kout;
kio::endl kendl{};
kio::now_time now{};

// ===== 内核全局符号桩 =====
uint64_t base_kernel_address = 0x100000;
uint64_t logical_processor_count = 4;
static thread_local uint32_t g_test_proc_id = 0;
extern "C" uint32_t fast_get_processor_id() { return g_test_proc_id; }
void test_set_processor_id(uint32_t id) { g_test_proc_id = id; }

