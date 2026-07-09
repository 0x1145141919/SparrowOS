#include <efi.h>
#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "Scheduler/per_processor_scheduler.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include <sys/io.h>
#define COM1_PORT 0x3F8
void uart_print_num(uint64_t raw, num_format_t format, numer_system_select radix);
void uart_runtime_p_num(uint64_t raw, num_format_t format, numer_system_select radix);
void uart_runtime_puts(const char* str, uint64_t len);
void uart_runtime_putc(char c);
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

bool uart_runtime_submit_char(char ch, bool urgent)
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
    slot.head.type = tc_uart_msg_type::single_character;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? 1u : 0u;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = g_uart_runtime_ring.seq_gen++;
    slot.payload.c.ch = ch;
    g_uart_runtime_ring.tail = next_tail;
    g_uart_runtime_ring.push_count++;
    g_uart_runtime_ring.lock.unlock();
    wakeup_thread(g_uart_runtime_service_tid);
    return true;
}
} // namespace

// === Software TX buffer (16 bytes, matches 16550 XMIT FIFO depth) ===
// Only accessed by the runtime service thread; no lock needed.
static char g_tx_buf[16];
static uint8_t g_tx_buf_count = 0;

static void serial_tx_buf_flush(void)
{
    if (g_tx_buf_count == 0) return;
    // Wait THRE (XMIT FIFO empty). Yield to let other threads run.
    while (!(inb(COM1_PORT + 5) & 0x20))
        kthread_yield();

    uint8_t n = g_tx_buf_count;
    for (uint8_t i = 0; i < n; i++)
        outb(g_tx_buf[i], COM1_PORT);
    g_tx_buf_count = 0;

    // Sleep for estimated drain time:
    // 115200 8N1: 1 byte = 10 bits / 115200 = ~87 μs
    // n bytes × 87 μs ≈ hardware has drained by wakeup
    kthread_sleep((uint64_t)n * 87);
}

static void serial_tx_buf_putc(char c)
{
    g_tx_buf[g_tx_buf_count++] = c;
    if (g_tx_buf_count == 16 || c == '\n')
        serial_tx_buf_flush();
}

static void serial_tx_buf_puts(const char* str, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        if (str[i])
            serial_tx_buf_putc(str[i]);
    }
}

static void serial_tx_buf_p_num(uint64_t raw, num_format_t format, numer_system_select radix)
{
    char buf[70];
    uint64_t n = format_num_to_buffer(buf, raw, format, radix);
    for (uint64_t i = 0; i < n; i++)
        serial_tx_buf_putc(buf[i]);
}

void serial_init_stage1() {
    // === [1] Disable all UART interrupts ===
    // IER @ +1, Datasheet §8.6.6
    outb(0x00, COM1_PORT + 1);

    // === [2] Set baud rate divisor = 1 (115200), DLAB=1 ===
    // LCR @ +3 bit 7 (DLAB), Datasheet §8.6.2
    outb(0x80, COM1_PORT + 3);      // LCR DLAB=1
    outb(0x01, COM1_PORT + 0);       // DLL = 0x01
    outb(0x00, COM1_PORT + 1);       // DLM = 0x00

    // === [3] Line control: 8N1, DLAB=0 ===
    // bits[1:0]=11 (8-bit), bit2=0 (1 stop), bit3=0 (no parity)
    outb(0x03, COM1_PORT + 3);

    // === [4] FIFO Control ===
    // FCR @ +2 (write-only), Datasheet §8.6.4
    // bit0=1 (FIFO enable), bit1=1 (RCVR reset), bit2=1 (XMIT reset)
    // bits[7:6]=00 (1-byte trigger), bit3=0 (DMA mode 0)
    outb(0x07, COM1_PORT + 2);

    // === [5] Modem Control ===
    // MCR @ +4, Datasheet §8.6.7
    // bit0=1 (DTR), bit1=1 (RTS), bit3=0 (OUT2, polled mode)
    outb(0x03, COM1_PORT + 4);

    // === [6] Clear stale residual ===
    (void)inb(COM1_PORT + 5);       // LSR: clear error bits
    (void)inb(COM1_PORT + 0);       // RBR: flush stale byte

    // === [7] Register kout backend ===
    kio::kout_backend backend={
        .name="COM1",
        .is_masked=0,
        .reserved=0,
        .running_stage_write=uart_runtime_puts,
        .running_stage_putchar=uart_runtime_putc,
        .running_stage_num=uart_runtime_p_num,
        .panic_write=polling_puts,
        .early_write=polling_puts,
    };
    bsp_kout.register_backend(backend);
    g_uart_runtime_ring.head = 0;
    g_uart_runtime_ring.tail = 0;
    g_uart_runtime_ring.seq_gen = 0;
    g_uart_runtime_ring.drop_count = 0;
    g_uart_runtime_ring.push_count = 0;
    g_uart_runtime_ring.pop_count = 0;
    g_uart_runtime_ready = true;
}
// 串口运行时发送服务线程（轮询发送，不使用串口中断）
int serial_init_stage2() {
    if (!g_uart_runtime_ready) return -1;
    g_uart_runtime_service_create_lock.lock();
    if (g_uart_runtime_service_tid != INVALID_TID) {
        g_uart_runtime_service_create_lock.unlock();
        return OS_SUCCESS;
    }
    kthread_creating_package pkg = {};
    pkg.func_raw  = (uint64_t)uart_runtime_service_thread_main;
    pkg.args[0]   = (uint64_t)nullptr;
    pkg.launch_pid = 0;
    KURD_t kurd = KURD_t();
    const uint64_t tid = creat_kthread(&pkg, &kurd);
    if (error_kurd(kurd) || tid == INVALID_TID) {
        g_uart_runtime_service_create_lock.unlock();
        return -1;
    }
    g_uart_runtime_service_tid = tid;
    g_uart_runtime_service_create_lock.unlock();
    return OS_SUCCESS;
}
// 发送一个字符 (polling, 逐字节等 THRE)
void polling_putc(char c) {
    while (!(inb(COM1_PORT + 5) & 0x20));  // wait THRE
    outb(c, COM1_PORT);
}

// 发送字符串
void polling_puts(const char* str,uint64_t len) {
    for(uint64_t i=0;i<len;i++)
    {
        if(str[i])polling_putc(str[i]);
    }
}

void uart_print_num(uint64_t raw,num_format_t format,numer_system_select radix){
    char buf[70];
    uint64_t len=format_num_to_buffer(buf,raw,format,radix);
    for(uint64_t i=0;i<len;i++){
        polling_putc(buf[i]);
    }
}

void uart_runtime_puts(const char* str, uint64_t len)
{
    if (!str || len == 0) return;
    if (!uart_runtime_submit_string(str, len, false)) {
        polling_puts(str, len);
    }
}

void uart_runtime_putc(char c)
{
    if (!uart_runtime_submit_char(c, false)) {
        polling_putc(c);
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
                    serial_tx_buf_puts(slot.payload.s.string, slot.payload.s.len);
                    break;
                case tc_uart_msg_type::single_character:
                    serial_tx_buf_putc(slot.payload.c.ch);
                    break;
                case tc_uart_msg_type::num:
                    serial_tx_buf_p_num(
                        slot.payload.n.num_raw,
                        slot.payload.n.format,
                        slot.payload.n.radix
                    );
                    break;
                default:
                    break;
            }
        }
        // Flush tail bytes remaining in buffer
        serial_tx_buf_flush();
    }
}
// 端口输出函数(内联汇编)
