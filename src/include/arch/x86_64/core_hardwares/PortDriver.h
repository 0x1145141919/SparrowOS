#ifndef PORT_DRIVER_H
#define PORT_DRIVER_H
#include <stdint.h>
#include "util/lock.h"
#include "util/OS_utils.h"
#ifdef __cplusplus
extern "C" {
#endif

void serial_puts(const char* str,uint64_t len);
void serial_init_stage1();
int serial_init_stage2();
void serial_putc(char c);
#ifdef __cplusplus
}
#endif

#endif // PORT_DRIVER_H
enum class tc_uart_msg_type : uint8_t { string,single_character,num };
struct tc_uart_msg_frame_head{
    tc_uart_msg_type type;
    uint8_t reserved;
    uint16_t flags; 
    uint32_t producer_cpu;
    uint64_t seq;
};
constexpr uint32_t UART_TC_RING_CAP = 1024;
constexpr uint32_t UART_TC_SERVICE_POP_BATCH = 64;
struct tc_uart_slot {
    tc_uart_msg_frame_head head;
    union {
        struct { const char* string; uint64_t len; } s;
        struct { char ch; } c;
        struct { uint64_t num_raw; num_format_t format; numer_system_select radix; } n;
    } payload;
};
struct tc_uart_ring {
    spintrylock_cpp_t lock;
    tc_uart_slot slots[UART_TC_RING_CAP];
    uint32_t head;
    uint32_t tail;
    uint64_t seq_gen;
    uint64_t drop_count;
    uint64_t push_count;
    uint64_t pop_count;
};
struct tc_uart_service_local_batch {
    uint32_t count;
    tc_uart_slot items[UART_TC_SERVICE_POP_BATCH];
};
