#include <efi.h>
#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#define COM1_PORT 0x3F8
static inline void outb(UINT16 port, UINT8 value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// 端口输入函数(内联汇编)
static inline UINT8 inb(UINT16 port) {
    UINT8 ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
// 初始化串口
void uart_print_num(uint64_t raw,num_format_t format,numer_system_select radix);
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
        .running_stage_write=serial_puts,
        .running_stage_num=uart_print_num,
        .panic_write=serial_puts,
        .early_write=serial_puts,
    };
    bsp_kout.register_backend(backend);
    // 启用中断(可选)
   //outb(COM1_PORT + 1, 0x0F);
}
int serial_init_stage2() {//第二阶段的要配置中断

}
// 检查发送缓冲区是否为空
int serial_is_transmit_empty() {
    return inb(COM1_PORT + 5) & 0x20;
}

// 发送一个字符
void serial_putc(char c) {
    while (serial_is_transmit_empty() == 0);
    outb(COM1_PORT, c);
}

// 发送字符串
void serial_puts(const char* str,uint64_t len) {
    for(uint64_t i=0;i<len;i++)
    {
        if(str[i])serial_putc(str[i]);
    }
}
void uart_print_num(uint64_t raw,num_format_t format,numer_system_select radix){
    char buf[70];
    uint64_t len=format_num_to_buffer(buf,raw,format,radix);
    for(uint64_t i=0;i<len;i++){
        serial_putc(buf[i]);
    }
}
// 端口输出函数(内联汇编)
