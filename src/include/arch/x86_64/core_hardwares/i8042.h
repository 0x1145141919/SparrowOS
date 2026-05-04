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

enum class ps2_key_action : uint8_t {
    make = 1,
    break_ = 2,
    repeat = 3,
    error = 255
};

enum ps2_event_flags : uint16_t {
    ps2_event_flag_none          = 0,
    ps2_event_flag_extended_e0  = 1 << 0,
    ps2_event_flag_synthetic    = 1 << 1,
    ps2_event_flag_overflow_drop= 1 << 2,
    ps2_event_flag_parser_error = 1 << 3
};

struct ps_2_keyboard_event{
    uint8_t key_code;
    uint8_t action;
    uint8_t modifiers;
    uint8_t reserved0;
    uint16_t flags;
    uint16_t reserved1;
    uint64_t timestamp_us;
};
static_assert(sizeof(ps_2_keyboard_event)==16,"ps_2_keyboard_event should be 16 bytes");
constexpr uint16_t i8042_buffer_max_size = 256;
constexpr uint16_t i8042_char_buffer_max_size = 1024;

enum kbd_char_flags : uint16_t {
    kbd_char_flag_none         = 0,
    kbd_char_flag_repeat       = 1 << 0,
    kbd_char_flag_from_keypad  = 1 << 1,
    kbd_char_flag_drop_hint    = 1 << 2
};

struct kbd_char_event {
    char ch;
    uint8_t reserved0;
    uint16_t flags;
    uint32_t source_key;
    uint64_t decisive_event_seq;
};
struct text_input_event{
    uint8_t event_type;//0:普通字符事件；1：文本控制事件（上下左右del,PgUP,PgDn,esc)
    uint8_t reserved0;
    uint16_t data;
    uint64_t decisive_event_seq;
};
static_assert(sizeof(kbd_char_event)==16,"kbd_char_event should be 16 bytes");
static_assert(sizeof(text_input_event)==16,"text_input_event should be 16 bytes");

extern u16ka i8042_event_tail_idx;
extern u64ka i8042_event_publish_seq;
extern u64ka i8042_ring_overflow_count;
extern u64ka i8042_parser_error_count;
extern u64ka i8042_event_generated_count;
extern const ps_2_keyboard_event* i8042_event_ring_readonly_view;
extern u16ka i8042_char_event_tail_idx;
extern u64ka i8042_char_publish_seq;
extern u64ka i8042_char_drop_count;
extern const kbd_char_event* i8042_char_ring_readonly_view;

extern tid_wait_queue* i8042_scancode_buffer_subscriber_queue;
extern tid_wait_queue* i8042_analyzed_buffer_subscriber_queue;
extern tid_wait_queue* i8042_char_buffer_subscriber_queue;

extern "C" bool i8042_read_event_by_seq(uint64_t seq, ps_2_keyboard_event* out_event);
extern "C" uint64_t i8042_get_publish_seq();
extern "C" void i8042_wait_event(uint64_t last_publish_seq);
extern "C" bool i8042_char_read_event_by_seq(uint64_t seq, kbd_char_event* out_event);
extern "C" uint64_t i8042_char_get_publish_seq();
extern "C" void i8042_char_wait_event(uint64_t last_publish_seq);
extern "C" void i8042_char_subscriber_init();

typedef struct {
    char data[1024];
    uint16_t len;
} buff_t;

extern "C" void i8042_blockable_keyboard_listening(buff_t* buf);

// ============================================================
// text_input_event 控制事件码
// ============================================================
constexpr uint16_t TEXT_CTRL_UP       = 0x0001;
constexpr uint16_t TEXT_CTRL_DOWN     = 0x0002;
constexpr uint16_t TEXT_CTRL_LEFT     = 0x0003;
constexpr uint16_t TEXT_CTRL_RIGHT    = 0x0004;
constexpr uint16_t TEXT_CTRL_HOME     = 0x0005;
constexpr uint16_t TEXT_CTRL_END      = 0x0006;
constexpr uint16_t TEXT_CTRL_PGUP     = 0x0007;
constexpr uint16_t TEXT_CTRL_PGDN     = 0x0008;
constexpr uint16_t TEXT_CTRL_INSERT   = 0x0009;
constexpr uint16_t TEXT_CTRL_DELETE   = 0x000A;
constexpr uint16_t TEXT_CTRL_ESCAPE   = 0x000B;
constexpr uint16_t TEXT_CTRL_TAB      = 0x000C;
constexpr uint16_t TEXT_CTRL_ENTER    = 0x000D;
constexpr uint16_t TEXT_CTRL_BACKSPACE= 0x000E;

// text_input_event ring 全局变量
extern u16ka text_input_event_tail_idx;
extern u64ka text_input_publish_seq;
extern u64ka text_input_drop_count;
extern const text_input_event* text_input_ring_readonly_view;
extern tid_wait_queue* text_input_subscribers_queue;

// text_input_event Level 1：单事件
extern "C" bool   text_input_read_event_by_seq(uint64_t seq, text_input_event* out);
extern "C" uint64_t text_input_get_publish_seq();
extern "C" void   text_input_wait_event(uint64_t last_publish_seq);
extern "C" void   text_input_subscriber_init();

// text_input_event Level 2：批量读取
extern "C" uint32_t text_input_batch_read(
    uint64_t start_seq,
    text_input_event* out_events,
    uint32_t max_count);

#ifdef __cplusplus
extern void register_i8042_kshell_commands();
#endif