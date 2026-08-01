#include "init/init_fatal.h"
// ════════════════════════════════════════════════════════════════
// init_fatal.cpp — 收尸者实现
//
// 自包含原则：不依赖 kout / PortDriver / 堆。首错可能发生在
// kout.Init() 之前，因此唯一可靠的输出通道是 UART(COM1) 裸写。
// ════════════════════════════════════════════════════════════════

namespace {

constexpr uint16_t COM1_PORT = 0x3F8;

// 内联 asm outb（不依赖 <sys/io.h>，freestanding 自包含）
inline void uart_outb(uint8_t val, uint16_t port)
{
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// 忙等发送保持寄存器为空
inline void uart_wait_thr()
{
    for (int i = 0; i < 100000; i++) {
        uint8_t lsr = 0;
        asm volatile("inb %1, %0" : "=a"(lsr) : "Nd"(uint16_t(COM1_PORT + 5)));
        if (lsr & 0x20) return;  // THR 空，可写
    }
}

inline void uart_putc(char c)
{
    uart_wait_thr();
    uart_outb((uint8_t)c, COM1_PORT);
}

inline void uart_puts(const char* s)
{
    while (*s) uart_putc(*s++);
}

// 打印一个 u64 的 16 位十六进制（不依赖 kout / format_num_to_buffer）
void uart_puthex(uint64_t v)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(hex_digits[(v >> shift) & 0xF]);
    }
}

} // namespace

namespace init_fatal {

void halt(loc_code_t loc)
{
    uart_puts("\r\n[INIT FATAL] LOC=0x");
    uart_puthex(loc);
    uart_puts("\r\n");
    halt_raw();
}

}
