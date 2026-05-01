#include <arch/x86_64/core_hardwares/i8042.h>
#include <arch/x86_64/core_hardwares/ioapic.h>
#include <arch/x86_64/core_hardwares/lapic.h>
#include <arch/x86_64/Interrupt_system/loacl_processor.h>
#include <memory/AddresSpace.h>
#include <ktime.h>
#include <util/kout.h>
#include <panic.h>
#include <global_controls.h>
extern "C" char i8042_code_deal;
extern "C" char i8042_fault_deal;

u16ka i8042_event_tail_idx;
u64ka i8042_event_publish_seq;
u64ka i8042_ring_overflow_count;
u64ka i8042_parser_error_count;
u64ka i8042_event_generated_count;
const ps_2_keyboard_event* i8042_event_ring_readonly_view = nullptr;

namespace {
alignas(4096) ps_2_keyboard_event i8042_event_ring[i8042_buffer_max_size];
volatile uint64_t i8042_event_publish_seq_block_token = 0;

constexpr uint8_t MOD_SHIFT = 1 << 0;
constexpr uint8_t MOD_CTRL = 1 << 1;
constexpr uint8_t MOD_ALT = 1 << 2;
constexpr uint8_t MOD_CAPS = 1 << 3;
constexpr uint8_t MOD_NUM = 1 << 4;
constexpr uint8_t MOD_SCROLL = 1 << 5;

struct keyboard_parser_state {
    bool e0_prefix_pending;
    uint8_t e1_expected_index;
};

keyboard_parser_state parser_state{false, 0};
uint8_t keyboard_modifiers = 0;
uint8_t key_down_state[256] = {0};
constexpr uint8_t pause_sequence_after_e1[5] = {0x1D, 0x45, 0xE1, 0x9D, 0xC5};

static inline uint16_t ring_next_index(uint16_t idx)
{
    idx++;
    if(idx >= i8042_buffer_max_size){
        idx = 0;
    }
    return idx;
}

static inline void apply_key_state_to_modifiers(uint8_t key_code, bool is_break, uint16_t flags)
{
    const bool is_extended = (flags & ps2_event_flag_extended_e0) != 0;
    switch(key_code){
        case 0x2A:
        case 0x36:
        {
            if(is_break){
                keyboard_modifiers &= ~MOD_SHIFT;
            }else{
                keyboard_modifiers |= MOD_SHIFT;
            }
            break;
        }
        case 0x1D:
        {
            if(is_break){
                keyboard_modifiers &= ~MOD_CTRL;
            }else{
                keyboard_modifiers |= MOD_CTRL;
            }
            break;
        }
        case 0x38:
        {
            if(is_break){
                keyboard_modifiers &= ~MOD_ALT;
            }else{
                keyboard_modifiers |= MOD_ALT;
            }
            break;
        }
        case 0x3A:
        {
            if(!is_break && !is_extended){
                keyboard_modifiers ^= MOD_CAPS;
            }
            break;
        }
        case 0x45:
        {
            if(!is_break && (flags & ps2_event_flag_synthetic) == 0){
                keyboard_modifiers ^= MOD_NUM;
            }
            break;
        }
        case 0x46:
        {
            if(!is_break && !is_extended){
                keyboard_modifiers ^= MOD_SCROLL;
            }
            break;
        }
        default:
            break;
    }
}

static inline void publish_event(uint8_t key_code, uint8_t action, uint16_t flags)
{
    ps_2_keyboard_event event{};
    const uint8_t make_code = static_cast<uint8_t>(ps2_key_action::make);
    const uint8_t break_code = static_cast<uint8_t>(ps2_key_action::break_);
    if(action == make_code || action == break_code){
        const bool is_break = (action == break_code);
        apply_key_state_to_modifiers(key_code, is_break, flags);
    }
    event.key_code = key_code;
    event.action = action;
    event.modifiers = keyboard_modifiers;
    event.flags = flags;
    const uint64_t published_before = i8042_event_publish_seq.load(atomic_memory_order::relaxed);
    if(published_before >= i8042_buffer_max_size){
        event.flags |= ps2_event_flag_overflow_drop;
        i8042_ring_overflow_count.add_ka(1, atomic_memory_order::relaxed);
    }
    event.timestamp_us = ktime::get_microsecond_stamp();
    uint16_t tail = i8042_event_tail_idx.load(atomic_memory_order::relaxed);
    i8042_event_ring[tail] = event;
    i8042_event_tail_idx.store(ring_next_index(tail), atomic_memory_order::release);
    uint64_t old_seq = i8042_event_publish_seq.add_ka(1, atomic_memory_order::release);
    i8042_event_publish_seq_block_token = old_seq + 1;
    i8042_event_generated_count.add_ka(1, atomic_memory_order::relaxed);

    if(GlobalKernelStatus >= SCHEDUL_READY && i8042_analyzed_buffer_subscriber_queue){
        spinlock_interrupt_about_guard queue_guard(i8042_analyzed_buffer_subscriber_queue->lock);
        i8042_analyzed_buffer_subscriber_queue->wakeup_all();
    }
}

static inline void handle_parser_error(uint8_t key_code)
{
    parser_state.e0_prefix_pending = false;
    parser_state.e1_expected_index = 0;
    i8042_parser_error_count.add_ka(1, atomic_memory_order::relaxed);
    publish_event(
        key_code,
        static_cast<uint8_t>(ps2_key_action::error),
        ps2_event_flag_parser_error
    );
}

static inline void feed_scancode_byte(uint8_t byte)
{
    if(byte == 0xFA || byte == 0xFE || byte == 0x00){
        return;
    }

    if(parser_state.e1_expected_index != 0){
        const uint8_t expect = pause_sequence_after_e1[parser_state.e1_expected_index - 1];
        if(byte != expect){
            handle_parser_error(byte);
            return;
        }
        parser_state.e1_expected_index++;
        if(parser_state.e1_expected_index == 6){
            parser_state.e1_expected_index = 0;
            publish_event(
                0x45,
                static_cast<uint8_t>(ps2_key_action::make),
                ps2_event_flag_synthetic
            );
        }
        return;
    }

    if(byte == 0xE1){
        parser_state.e1_expected_index = 1;
        parser_state.e0_prefix_pending = false;
        return;
    }

    if(byte == 0xE0){
        if(parser_state.e0_prefix_pending){
            handle_parser_error(byte);
            return;
        }
        parser_state.e0_prefix_pending = true;
        return;
    }

    uint16_t flags = ps2_event_flag_none;
    if(parser_state.e0_prefix_pending){
        flags |= ps2_event_flag_extended_e0;
        parser_state.e0_prefix_pending = false;
    }
    const bool is_break = (byte & 0x80) != 0;
    const uint8_t key_code = byte & 0x7F;
    const uint8_t key_index = static_cast<uint8_t>(key_code | ((flags & ps2_event_flag_extended_e0) ? 0x80 : 0x00));

    uint8_t action = static_cast<uint8_t>(ps2_key_action::make);
    if(is_break){
        action = static_cast<uint8_t>(ps2_key_action::break_);
        key_down_state[key_index] = 0;
    }else{
        if(key_down_state[key_index] != 0){
            action = static_cast<uint8_t>(ps2_key_action::repeat);
        }else{
            key_down_state[key_index] = 1;
        }
    }

    publish_event(
        key_code,
        action,
        flags
    );
}
} // namespace

union led_status {
    uint8_t raw;
    struct {
        uint8_t scrolllock:1;
        uint8_t numlock:1;
        uint8_t capslock:1;
        uint8_t res:5;
    }field;
};
tid_wait_queue* i8042_scancode_buffer_subscriber_queue;
tid_wait_queue* i8042_analyzed_buffer_subscriber_queue;
led_status key_board_led;

void wait_until_in_buff_clear(){
    while(inb(0x64)&0x2);
}
void wait_until_out_buff_ready(){
    while((inb(0x64)&0x1)==0);
}
void led_set(){
    wait_until_in_buff_clear();
    outb(0xed,0x60);
    uint8_t ack;
    wait_until_out_buff_ready();
    ack=inb(0x60);
    if(ack!=0xfa){
        return;
    }
    wait_until_in_buff_clear();
    outb(key_board_led.raw,0x60);
    wait_until_out_buff_ready();
    ack=inb(0x60);
    if(ack!=0xfa){
        return;
    }
}
extern "C" void i8042_cpp_enter(x64_standard_context* frame){
    (void)frame;
    uint8_t scancode= inb(0x60);
    feed_scancode_byte(scancode);
    x2apic::x2apic_driver::write_eoi();
}

extern "C" bool i8042_read_event_by_seq(uint64_t seq, ps_2_keyboard_event* out_event)
{
    if(out_event==nullptr){
        return false;
    }
    const ps_2_keyboard_event* ring_view =
        (i8042_event_ring_readonly_view != nullptr) ? i8042_event_ring_readonly_view : i8042_event_ring;
    while(true){
        const uint64_t publish_seq_before = i8042_event_publish_seq.load(atomic_memory_order::acquire);
        if(seq >= publish_seq_before){
            return false;
        }
        if((publish_seq_before - seq) > i8042_buffer_max_size){
            return false;
        }
        const uint16_t idx = static_cast<uint16_t>(seq % i8042_buffer_max_size);
        ps_2_keyboard_event candidate = ring_view[idx];
        const uint64_t publish_seq_after = i8042_event_publish_seq.load(atomic_memory_order::acquire);
        if(publish_seq_before != publish_seq_after){
            continue;
        }
        if(seq >= publish_seq_after){
            return false;
        }
        if((publish_seq_after - seq) > i8042_buffer_max_size){
            return false;
        }
        *out_event = candidate;
        return true;
    }
}

extern "C" uint64_t i8042_get_publish_seq()
{
    return i8042_event_publish_seq.load(atomic_memory_order::acquire);
}

extern "C" void i8042_wait_event(uint64_t last_publish_seq)
{
    if(i8042_analyzed_buffer_subscriber_queue==nullptr){
        return;
    }
    block_if_equal(
        i8042_analyzed_buffer_subscriber_queue,
        (uint64_t*)&i8042_event_publish_seq_block_token,
        last_publish_seq
    );
}

void i8042_interrupt_enable(){
    // 只读缓冲区初始化：
    // 1) 通过 KspacePageTable::v_to_phyaddrtraslation 获取 i8042_event_ring 的物理基址
    // 2) 通过 phyaddr_direct_map 建立只读 + WB 的重映射视图给订阅者使用
    phyaddr_t ring_phybase = 0;
    KURD_t ring_phy_kurd = KspacePageTable::v_to_phyaddrtraslation(
        (vaddr_t)i8042_event_ring,
        ring_phybase
    );
    if(success_all_kurd(ring_phy_kurd)){
        vm_interval ro_view_interval{
            .vbase = 0,
            .pbase = ring_phybase,
            .size = sizeof(i8042_event_ring),
            .access = KspacePageTable::PG_R
        };
        KURD_t ro_map_kurd;
        vaddr_t ro_vbase = phyaddr_direct_map(&ro_view_interval, &ro_map_kurd);
        if(ro_vbase != 0 && success_all_kurd(ro_map_kurd)){
            i8042_event_ring_readonly_view = (const ps_2_keyboard_event*)ro_vbase;
        }
    }
    if(i8042_event_ring_readonly_view == nullptr){
        i8042_event_ring_readonly_view = i8042_event_ring;
    }
    x64_local_processor* manage=x86_smp_processors_container::get_processor_mgr_by_processor_id(++legacy_rotate_interrupt_alloc_id);
    uint32_t target_apicid=manage->get_apic_id();
    uint8_t vec= manage->handler_alloc((void*)&i8042_code_deal);
    if(vec==0xff){
        //panic
    }
    KURD_t kurd;
    ioapic_driver::compact_flag flag={
            .vec=vec,
            .trigger_mode=0,
            .polarity=0  
    };
    flag.target_apicid=target_apicid;
    kurd=main_router->irq_regist(1,flag);
    key_board_led.raw=0;
    led_set();
    while(inb(0x64)&0x3);
    outb(0x64,0x20);
    uint8_t command=inb(0x60);
    while(inb(0x64)&0x3);
    command|=1;
    outb(0x64,0x20);
    while(inb(0x64)&0x3);
    outb(0x60,command);
    while(inb(0x64)&0x3);
    i8042_scancode_buffer_subscriber_queue=new tid_wait_queue;
    i8042_analyzed_buffer_subscriber_queue=new tid_wait_queue;
}
