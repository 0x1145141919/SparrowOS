#pragma once
#include <stdint.h> 
#include <sys/io.h>
#include "util/OS_utils.h"
#include <Scheduler/per_processor_scheduler.h>
namespace i8042_regs{
    constexpr uint8_t STATUS_OUT_BUFF_MASK = 0x01;
    constexpr uint8_t STATUS_IN_BUFF_MASK = 0x02;
    constexpr uint8_t STATUS_SYSTEM_FLAG_MASK = 0x1<<2;
    constexpr uint8_t STATUS_IS_COMMAND_MASK = 0x1<<3;
    constexpr uint8_t STATUS_KEYBOARD_ENABLE_MASK = 0x1<<4;
    constexpr uint8_t STATUS_OUT_TIMEOUT_MASK = 0x1<<5;
    constexpr uint8_t STATUS_GET_TIMEOUT_MASK = 0x1<<6;
    constexpr uint8_t CONTROL_KEYBOARD_INTERRUPT_MASK = 0x1<<0;
    constexpr uint8_t CONTROL_MOUSE_INTERRUPT_MASK = 0x1<<1;
    constexpr uint8_t CONTROL_KEYBOARD_DENY_MASK = 0x1<<4;
    constexpr uint8_t CONTROL_MOUSE_DENY_MASK = 0x1<<5;
}
extern void i8042_interrupt_enable();
const char* scancode_to_key(uint8_t scancode);
constexpr uint16_t i8042_buffer_max_size = 4096;
extern uint8_t*readonly_buff;
extern u16ka scan_code_buffer_tail_idx;
extern tid_wait_queue* i8042_scancode_buffer_subscriber_queue;
extern tid_wait_queue* i8042_analyzed_buffer_subscriber_queue;
// 扫描码集 2 的部分映射表（可继续扩展）
