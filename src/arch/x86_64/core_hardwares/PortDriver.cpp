#include <efi.h>
#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "Scheduler/per_processor_scheduler.h"
#include "util/arch/x86-64/cpuid_intel.h"
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
void uart_print_num(uint64_t raw, num_format_t format, numer_system_select radix);
void uart_runtime_p_num(uint64_t raw, num_format_t format, numer_system_select radix);
void uart_runtime_puts(const char* str, uint64_t len);
void* uart_runtime_service_thread_main(void* data);

namespace {
spintrylock_cpp_t g_uart_runtime_service_create_lock = {};
tc_uart_ring g_uart_runtime_ring = {};
uint64_t g_uart_runtime_service_tid = INVALID_TID;
bool g_uart_runtime_ready = false;

static inline uint32_t ring_next_index(uint32_t idx)
{
    return (idx + 1) & (UART_TC_RING_CAP - 1);
}

bool uart_runtime_submit_string(const char* str, uint64_t len, bool urgent)
{
    if (!g_uart_runtime_ready || g_uart_runtime_service_tid == INVALID_TID || !str || len == 0) {
        return false;
    }
    g_uart_runtime_ring.lock.lock();
    const uint32_t next_tail = ring_next_index(g_uart_runtime_ring.tail);
    if (next_tail == g_uart_runtime_ring.head) {
        g_uart_runtime_ring.drop_count++;
        g_uart_runtime_ring.lock.unlock();
        return false;
    }
    tc_uart_slot& slot = g_uart_runtime_ring.slots[g_uart_runtime_ring.tail];
    slot.head.type = tc_uart_msg_type::string;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? 1u : 0u;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = g_uart_runtime_ring.seq_gen++;
    slot.payload.s.string = str;
    slot.payload.s.len = len;
    g_uart_runtime_ring.tail = next_tail;
    g_uart_runtime_ring.push_count++;
    g_uart_runtime_ring.lock.unlock();
    wakeup_thread(g_uart_runtime_service_tid);
    return true;
}

bool uart_runtime_submit_num(uint64_t raw, num_format_t format, numer_system_select radix, bool urgent)
{
    if (!g_uart_runtime_ready || g_uart_runtime_service_tid == INVALID_TID) {
        return false;
    }
    g_uart_runtime_ring.lock.lock();
    const uint32_t next_tail = ring_next_index(g_uart_runtime_ring.tail);
    if (next_tail == g_uart_runtime_ring.head) {
        g_uart_runtime_ring.drop_count++;
        g_uart_runtime_ring.lock.unlock();
        return false;
    }
    tc_uart_slot& slot = g_uart_runtime_ring.slots[g_uart_runtime_ring.tail];
    slot.head.type = tc_uart_msg_type::num;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? 1u : 0u;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = g_uart_runtime_ring.seq_gen++;
    slot.payload.n.num_raw = raw;
    slot.payload.n.format = format;
    slot.payload.n.radix = radix;
    g_uart_runtime_ring.tail = next_tail;
    g_uart_runtime_ring.push_count++;
    g_uart_runtime_ring.lock.unlock();
    wakeup_thread(g_uart_runtime_service_tid);
    return true;
}
} // namespace

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
        .running_stage_write=uart_runtime_puts,
        .running_stage_num=uart_runtime_p_num,
        .panic_write=serial_puts,
        .early_write=serial_puts,
    };
    bsp_kout.register_backend(backend);
    g_uart_runtime_ring.head = 0;
    g_uart_runtime_ring.tail = 0;
    g_uart_runtime_ring.seq_gen = 0;
    g_uart_runtime_ring.drop_count = 0;
    g_uart_runtime_ring.push_count = 0;
    g_uart_runtime_ring.pop_count = 0;
    g_uart_runtime_ready = true;
    // 启用中断(可选)
   //outb(COM1_PORT + 1, 0x0F);
}
// 串口运行时发送服务线程（轮询发送，不使用串口中断）
int serial_init_stage2() {
    if (!g_uart_runtime_ready) return -1;
    g_uart_runtime_service_create_lock.lock();
    if (g_uart_runtime_service_tid != INVALID_TID) {
        g_uart_runtime_service_create_lock.unlock();
        return OS_SUCCESS;
    }
    KURD_t kurd = KURD_t();
    const uint64_t tid = create_kthread(uart_runtime_service_thread_main, nullptr, &kurd);
    if (error_kurd(kurd) || tid == INVALID_TID) {
        g_uart_runtime_service_create_lock.unlock();
        return -1;
    }
    g_uart_runtime_service_tid = tid;
    g_uart_runtime_service_create_lock.unlock();
    return OS_SUCCESS;
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

void uart_runtime_puts(const char* str, uint64_t len)
{
    if (!str || len == 0) return;
    if (!uart_runtime_submit_string(str, len, false)) {
        serial_puts(str, len);
    }
}

void uart_runtime_p_num(uint64_t raw, num_format_t format, numer_system_select radix)
{
    if (!uart_runtime_submit_num(raw, format, radix, false)) {
        uart_print_num(raw, format, radix);
    }
}

void* uart_runtime_service_thread_main(void* data)
{
    (void)data;
    tc_uart_service_local_batch local_batch{};
    char num_buf[70];
    for (;;) {
        local_batch.count = 0;
        g_uart_runtime_ring.lock.lock();
        while (g_uart_runtime_ring.head != g_uart_runtime_ring.tail &&
               local_batch.count < UART_TC_SERVICE_POP_BATCH) {
            local_batch.items[local_batch.count] = g_uart_runtime_ring.slots[g_uart_runtime_ring.head];
            g_uart_runtime_ring.head = ring_next_index(g_uart_runtime_ring.head);
            g_uart_runtime_ring.pop_count++;
            local_batch.count++;
        }
        g_uart_runtime_ring.lock.unlock();

        if (local_batch.count == 0) {
            kthread_self_blocked(task_blocked_reason_t::no_job);
            continue;
        }

        for (uint32_t i = 0; i < local_batch.count; ++i) {
            const tc_uart_slot& slot = local_batch.items[i];
            switch (slot.head.type) {
                case tc_uart_msg_type::string:
                    serial_puts(slot.payload.s.string, slot.payload.s.len);
                    break;
                case tc_uart_msg_type::single_character:
                    serial_putc(slot.payload.c.ch);
                    break;
                case tc_uart_msg_type::num: {
                    uint64_t n = format_num_to_buffer(
                        num_buf,
                        slot.payload.n.num_raw,
                        slot.payload.n.format,
                        slot.payload.n.radix
                    );
                    for (uint64_t j = 0; j < n; ++j) {
                        serial_putc(num_buf[j]);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}
// 端口输出函数(内联汇编)
