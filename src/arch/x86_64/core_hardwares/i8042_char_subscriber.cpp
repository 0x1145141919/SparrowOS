#include <arch/x86_64/core_hardwares/i8042.h>
#include <memory/AddresSpace.h>
#include <panic.h>
#include <util/kptrace.h>
#include <util/kout.h>
u16ka i8042_char_event_tail_idx;
u64ka i8042_char_publish_seq;
u64ka i8042_char_drop_count;
const kbd_char_event* i8042_char_ring_readonly_view = nullptr;
tid_wait_queue* i8042_char_buffer_subscriber_queue;

namespace {
alignas(4096) kbd_char_event i8042_char_ring[i8042_char_buffer_max_size];
volatile uint64_t i8042_char_publish_seq_block_token = 0;
spinlock_cpp_t i8042_char_subscriber_init_lock;
uint64_t i8042_char_subscriber_tid = INVALID_TID;

constexpr uint8_t MOD_SHIFT = 1 << 0;
constexpr uint8_t MOD_CAPS = 1 << 3;
constexpr uint8_t MOD_NUM = 1 << 4;

static inline uint16_t ring_next_index(uint16_t idx)
{
    idx++;
    if(idx >= i8042_char_buffer_max_size){
        idx = 0;
    }
    return idx;
}

static inline bool map_non_letter(uint8_t key_code, bool shift, char& out)
{
    switch(key_code){
        case 0x02: out = shift ? '!' : '1'; return true;
        case 0x03: out = shift ? '@' : '2'; return true;
        case 0x04: out = shift ? '#' : '3'; return true;
        case 0x05: out = shift ? '$' : '4'; return true;
        case 0x06: out = shift ? '%' : '5'; return true;
        case 0x07: out = shift ? '^' : '6'; return true;
        case 0x08: out = shift ? '&' : '7'; return true;
        case 0x09: out = shift ? '*' : '8'; return true;
        case 0x0A: out = shift ? '(' : '9'; return true;
        case 0x0B: out = shift ? ')' : '0'; return true;
        case 0x0C: out = shift ? '_' : '-'; return true;
        case 0x0D: out = shift ? '+' : '='; return true;
        case 0x1A: out = shift ? '{' : '['; return true;
        case 0x1B: out = shift ? '}' : ']'; return true;
        case 0x27: out = shift ? ':' : ';'; return true;
        case 0x28: out = shift ? '"' : '\''; return true;
        case 0x29: out = shift ? '~' : '`'; return true;
        case 0x2B: out = shift ? '|' : '\\'; return true;
        case 0x33: out = shift ? '<' : ','; return true;
        case 0x34: out = shift ? '>' : '.'; return true;
        case 0x35: out = shift ? '?' : '/'; return true;
        case 0x39: out = ' '; return true;
        case 0x0F: out = '\t'; return true;
        case 0x1C: out = '\n'; return true;
        case 0x0E: out = '\b'; return true;
        default:
            return false;
    }
}

static inline bool map_letter(uint8_t key_code, bool upper, char& out)
{
    char base = 0;
    switch(key_code){
        case 0x10: base = 'q'; break;
        case 0x11: base = 'w'; break;
        case 0x12: base = 'e'; break;
        case 0x13: base = 'r'; break;
        case 0x14: base = 't'; break;
        case 0x15: base = 'y'; break;
        case 0x16: base = 'u'; break;
        case 0x17: base = 'i'; break;
        case 0x18: base = 'o'; break;
        case 0x19: base = 'p'; break;
        case 0x1E: base = 'a'; break;
        case 0x1F: base = 's'; break;
        case 0x20: base = 'd'; break;
        case 0x21: base = 'f'; break;
        case 0x22: base = 'g'; break;
        case 0x23: base = 'h'; break;
        case 0x24: base = 'j'; break;
        case 0x25: base = 'k'; break;
        case 0x26: base = 'l'; break;
        case 0x2C: base = 'z'; break;
        case 0x2D: base = 'x'; break;
        case 0x2E: base = 'c'; break;
        case 0x2F: base = 'v'; break;
        case 0x30: base = 'b'; break;
        case 0x31: base = 'n'; break;
        case 0x32: base = 'm'; break;
        default:
            return false;
    }
    out = upper ? static_cast<char>(base - ('a' - 'A')) : base;
    return true;
}

static inline bool map_keypad(uint8_t key_code, bool num_lock, char& out)
{
    if(num_lock){
        switch(key_code){
            case 0x52: out = '0'; return true;
            case 0x4F: out = '1'; return true;
            case 0x50: out = '2'; return true;
            case 0x51: out = '3'; return true;
            case 0x4B: out = '4'; return true;
            case 0x4C: out = '5'; return true;
            case 0x4D: out = '6'; return true;
            case 0x47: out = '7'; return true;
            case 0x48: out = '8'; return true;
            case 0x49: out = '9'; return true;
            case 0x53: out = '.'; return true;
            default:
                break;
        }
    }
    switch(key_code){
        case 0x4A: out = '-'; return true;
        case 0x4E: out = '+'; return true;
        case 0x37: out = '*'; return true;
        default:
            return false;
    }
}

static inline bool translate_keyboard_event(
    uint64_t seq,
    const ps_2_keyboard_event& in,
    kbd_char_event& out
)
{
    const uint8_t action = in.action;
    const uint8_t make_code = static_cast<uint8_t>(ps2_key_action::make);
    const uint8_t repeat_code = static_cast<uint8_t>(ps2_key_action::repeat);
    if(action != make_code && action != repeat_code){
        return false;
    }
    const bool is_extended = (in.flags & ps2_event_flag_extended_e0) != 0;
    if(is_extended){
        return false;
    }

    const bool shift = (in.modifiers & MOD_SHIFT) != 0;
    const bool caps = (in.modifiers & MOD_CAPS) != 0;
    const bool num_lock = (in.modifiers & MOD_NUM) != 0;
    char ch = 0;

    const bool upper_letter = shift ^ caps;
    if(!map_letter(in.key_code, upper_letter, ch)){
        if(!map_non_letter(in.key_code, shift, ch)){
            if(!map_keypad(in.key_code, num_lock, ch)){
                return false;
            }
            out.flags = kbd_char_flag_from_keypad;
        }else{
            out.flags = kbd_char_flag_none;
        }
    }else{
        out.flags = kbd_char_flag_none;
    }

    if(action == repeat_code){
        out.flags |= kbd_char_flag_repeat;
    }
    out.ch = ch;
    out.reserved0 = 0;
    out.source_key = static_cast<uint32_t>(in.key_code) |
        (is_extended ? (1u << 8) : 0u);
    out.decisive_event_seq = seq;
    return true;
}

static inline void publish_char_event(const kbd_char_event& input)
{
    kbd_char_event out = input;
    const uint64_t published_before = i8042_char_publish_seq.load(atomic_memory_order::relaxed);
    if(published_before >= i8042_char_buffer_max_size){
        out.flags |= kbd_char_flag_drop_hint;
        i8042_char_drop_count.add_ka(1, atomic_memory_order::relaxed);
    }
    const uint16_t tail = i8042_char_event_tail_idx.load(atomic_memory_order::relaxed);
    i8042_char_ring[tail] = out;
    i8042_char_event_tail_idx.store(ring_next_index(tail), atomic_memory_order::release);
    const uint64_t old_seq = i8042_char_publish_seq.add_ka(1, atomic_memory_order::release);
    i8042_char_publish_seq_block_token = old_seq + 1;

    if(GlobalKernelStatus >= SCHEDUL_READY && i8042_char_buffer_subscriber_queue){
        spinlock_interrupt_about_guard queue_guard(i8042_char_buffer_subscriber_queue->lock);
        i8042_char_buffer_subscriber_queue->wakeup_all();
    }
}

static void init_char_readonly_view()
{
    phyaddr_t ring_phybase = 0;
    KURD_t ring_phy_kurd = KspacePageTable::v_to_phyaddrtraslation(
        (vaddr_t)i8042_char_ring,
        ring_phybase
    );
    if(success_all_kurd(ring_phy_kurd)){
        vm_interval ro_view_interval{
            .vbase = 0,
            .pbase = ring_phybase,
            .size = sizeof(i8042_char_ring),
            .access = KspacePageTable::PG_R
        };
        KURD_t ro_map_kurd;
        vaddr_t ro_vbase = phyaddr_direct_map(&ro_view_interval, &ro_map_kurd);
        if(ro_vbase != 0 && success_all_kurd(ro_map_kurd)){
            i8042_char_ring_readonly_view = (const kbd_char_event*)ro_vbase;
        }
    }
    if(i8042_char_ring_readonly_view == nullptr){
        i8042_char_ring_readonly_view = i8042_char_ring;
    }
}

void* i8042_char_subscriber_main(void* not_use)
{
    (void)not_use;
    uint64_t read_seq = i8042_get_publish_seq();
    while(true){
        uint64_t publish_seq = i8042_get_publish_seq();
        if(read_seq == publish_seq){
            i8042_wait_event(read_seq);
            continue;
        }
        if((publish_seq - read_seq) > i8042_buffer_max_size){
            read_seq = publish_seq - i8042_buffer_max_size;
        }
        while(read_seq < publish_seq){
            ps_2_keyboard_event key_event{};
            if(i8042_read_event_by_seq(read_seq, &key_event)){
                kbd_char_event char_event{};
                if(translate_keyboard_event(read_seq, key_event, char_event)){
                    publish_char_event(char_event);
                }
            }
            read_seq++;
        }
    }
}
} // namespace

extern "C" bool i8042_char_read_event_by_seq(uint64_t seq, kbd_char_event* out_event)
{
    if(out_event == nullptr){
        return false;
    }
    const kbd_char_event* ring_view =
        (i8042_char_ring_readonly_view != nullptr) ? i8042_char_ring_readonly_view : i8042_char_ring;
    while(true){
        const uint64_t publish_seq_before = i8042_char_publish_seq.load(atomic_memory_order::acquire);
        if(seq >= publish_seq_before){
            return false;
        }
        if((publish_seq_before - seq) > i8042_char_buffer_max_size){
            return false;
        }
        const uint16_t idx = static_cast<uint16_t>(seq % i8042_char_buffer_max_size);
        kbd_char_event candidate = ring_view[idx];
        const uint64_t publish_seq_after = i8042_char_publish_seq.load(atomic_memory_order::acquire);
        if(publish_seq_before != publish_seq_after){
            continue;
        }
        if(seq >= publish_seq_after){
            return false;
        }
        if((publish_seq_after - seq) > i8042_char_buffer_max_size){
            return false;
        }
        *out_event = candidate;
        return true;
    }
}

extern "C" uint64_t i8042_char_get_publish_seq()
{
    return i8042_char_publish_seq.load(atomic_memory_order::acquire);
}

extern "C" void i8042_char_wait_event(uint64_t last_publish_seq)
{
    if(i8042_char_buffer_subscriber_queue == nullptr){
        return;
    }
    block_if_equal(
        i8042_char_buffer_subscriber_queue,
        (uint64_t*)&i8042_char_publish_seq_block_token,
        last_publish_seq
    );
}

extern "C" void i8042_char_subscriber_init()
{
    spinlock_interrupt_about_guard init_guard(i8042_char_subscriber_init_lock);
    if(i8042_char_buffer_subscriber_queue == nullptr){
        i8042_char_buffer_subscriber_queue = new tid_wait_queue;
    }
    if(i8042_char_ring_readonly_view == nullptr){
        init_char_readonly_view();
    }
    if(i8042_char_subscriber_tid != INVALID_TID){
        return;
    }
    KURD_t kurd{};
    i8042_char_subscriber_tid = create_kthread(i8042_char_subscriber_main, nullptr, &kurd);
    if(error_kurd(kurd) || i8042_char_subscriber_tid == INVALID_TID){
        i8042_char_subscriber_tid = INVALID_TID;
    }
}

extern "C" void i8042_blockable_keyboard_listening(char* buffer)
{
    if(buffer == nullptr){
        return;
    }
    buffer[0] = '\0';

    constexpr uint32_t kListenLimit = 4096;
    constexpr char kEscapeChar = '\n';

    // 这个接口设计为“只允许在内核线程上下文里阻塞使用”。
    if(!kptrace_current_stack_has_kthread_entry()){
        return;
    }

    uint32_t out_idx = 0;
    uint64_t read_seq = i8042_char_get_publish_seq();
    while(out_idx < kListenLimit){
        uint64_t publish_seq = i8042_char_get_publish_seq();
        if(read_seq == publish_seq){
            i8042_char_wait_event(read_seq);
            continue;
        }
        if((publish_seq - read_seq) > i8042_char_buffer_max_size){
            read_seq = publish_seq - i8042_char_buffer_max_size;
        }

        while(read_seq < publish_seq && out_idx < kListenLimit){
            kbd_char_event ev{};
            if(i8042_char_read_event_by_seq(read_seq, &ev)){
                // 检查是否为终止字符（回车或换行）
                if(ev.ch == kEscapeChar || ev.ch == '\r'){
                    // 终止字符不写入buffer，直接结束
                    buffer[out_idx] = '\0';
                    return;
                }
                // 普通字符写入buffer
                buffer[out_idx++] = ev.ch;
                bsp_kout<<(char)ev.ch;
            }
            read_seq++;
        }
    }
    buffer[out_idx] = '\0';
}
