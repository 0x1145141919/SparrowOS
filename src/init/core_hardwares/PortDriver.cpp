#include "init/core_hardwares/PortDriver.h"
#include "init/util/kout.h"
#define COM1_PORT 0x3F8
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// 端口输入函数(内联汇编)
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
void uart_write(const char* buf, uint64_t len)
{
    if (!buf || len == 0) return;
    for (uint64_t i = 0; i < len; ++i) {
        while ((inb(COM1_PORT + 5) & 0x20) == 0);
        outb(COM1_PORT, buf[i]);
    }
}
// 初始化串口
void serial_init_stage1() {
    // 禁用中断
    outb(COM1_PORT + 1, 0x00);
    
    // 设置波特率(115200)
    outb(COM1_PORT + 3, 0x80);    // 启用DLAB(除数锁存访问位)
    outb(COM1_PORT + 0, 0x01);     // 设置除数为1 (低位)
    outb(COM1_PORT + 1, 0x00);     // 设置除数为1 (高位)
    
    // 8位数据，无奇偶校验，1位停止位
    outb(COM1_PORT + 3, 0x03);
    
    // 启用FIFO，清除接收/发送FIFO缓冲区
    outb(COM1_PORT + 2, 0xC7);
    kio::kout_backend backend={
        .name="COM1",
        .is_masked=0,
        .reserved=0,
        .write=&uart_write
    };
    bsp_kout.register_backend(backend);
    // 启用中断(可选)
   //outb(COM1_PORT + 1, 0x0F);
}
