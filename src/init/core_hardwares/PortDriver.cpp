#include "init/core_hardwares/PortDriver.h"
#include "init/util/kout.h"
#include <sys/io.h>
#define COM1_PORT 0x3F8
static int uart_is_stuck = 0;  // 串口不通就不重试了

void uart_write(const char* buf, uint64_t len)
{
    if (!buf || len == 0) return;
    if (uart_is_stuck) return;
    for (uint64_t i = 0; i < len; ++i) {
        int timeout = 100000;
        while ((inb(COM1_PORT + 5) & 0x20) == 0) {
            if (--timeout == 0) {
                uart_is_stuck = 1;
                return;
            }
        }
        outb(buf[i], COM1_PORT);
    }
}
// 初始化串口
void serial_init_stage1() {
    // 禁用中断
    outb(0x00, COM1_PORT + 1);
    
    // 设置波特率(115200)
    outb(0x80, COM1_PORT + 3);    // 启用DLAB(除数锁存访问位)
    outb(0x01, COM1_PORT + 0);     // 设置除数为1 (低位)
    outb(0x00, COM1_PORT + 1);     // 设置除数为1 (高位)
    
    // 8位数据，无奇偶校验，1位停止位
    outb(0x03, COM1_PORT + 3);
    
    // 启用FIFO，清除接收/发送FIFO缓冲区
    outb(0xC7, COM1_PORT + 2);
    kio::kout_backend backend={
        .name="COM1",
        .is_masked=0,
        .reserved=0,
        .write=&uart_write
    };
    bsp_kout.register_backend(backend);
    // 启用中断(可选)
   //outb(0x0F, COM1_PORT + 1);
}
