#ifndef PORT_DRIVER_H
#define PORT_DRIVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void serial_puts(const char* str,uint64_t len);
void serial_init_stage1();
void serial_putc(char c);
#ifdef __cplusplus
}
#endif

#endif // PORT_DRIVER_H