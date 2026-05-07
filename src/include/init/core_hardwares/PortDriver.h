#include <stdint.h>
extern "C" void serial_init_stage1();
extern "C" void uart_write(const char* buf, uint64_t len);