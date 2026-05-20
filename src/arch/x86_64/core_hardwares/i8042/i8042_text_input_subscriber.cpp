#include <arch/x86_64/core_hardwares/i8042.h>
#include <memory/AddresSpace.h>
#include <panic.h>
#include <util/kptrace.h>
#include <util/kout.h>
#include <string.h>

// ============================================================
// 全局变量定义
// ============================================================
u16ka text_input_event_tail_idx;
u64ka text_input_publish_seq;
u64ka text_input_drop_count;
const text_input_event* text_input_ring_readonly_view = nullptr;
tid_wait_queue* text_input_subscribers_queue;

// ============================================================
// 内部状态
// ============================================================
namespace {

constexpr uint16_t TEXT_INPUT_RING_SIZE = 1024;
alignas(4096) text_input_event
    text_input_ring[TEXT_INPUT_RING_SIZE];

volatile uint64_t text_input_publish_seq_block_token = 0;
spinlock_cpp_t text_input_subscriber_init_lock;
uint64_t text_input_subscriber_tid = INVALID_TID;

// modifiers 位定义（与 ps_2_keyboard_event.modifiers 一致）
constexpr uint8_t MOD_SHIFT = 1 << 0;
constexpr uint8_t MOD_CAPS  = 1 << 3;
constexpr uint8_t MOD_NUM   = 1 << 4;

// ============================================================
// Ring 辅助函数
// ============================================================
static inline uint16_t ring_next(uint16_t idx) {
    idx++;
    if (idx >= TEXT_INPUT_RING_SIZE) idx = 0;
    return idx;
}

// ============================================================
// 字符映射（复用 char_subscriber 的映射表）
// ============================================================
static inline bool map_non_letter(uint8_t key_code, bool shift, char& out) {
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
        default:   return false;
    }
}

static inline bool map_letter(uint8_t key_code, bool upper, char& out) {
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
        default:   return false;
    }
    out = upper ? static_cast<char>(base - ('a' - 'A')) : base;
    return true;
}

static inline bool map_keypad(uint8_t key_code, bool num_lock, char& out) {
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
            default:   break;
        }
    }
    switch(key_code){
        case 0x4A: out = '-'; return true;
        case 0x4E: out = '+'; return true;
        case 0x37: out = '*'; return true;
        default:   return false;
    }
}

// ============================================================
// 翻译：一个 ps_2_keyboard_event → text_input_event(s)
// ============================================================
struct translated_events {
    text_input_event events[2];  // 最多同时产出 2 个事件（type=0 + type=1）
    uint32_t count;
};

static bool is_control_key(uint8_t key_code, bool is_extended,
                           uint16_t& out_ctrl)
{
    if (is_extended) {
        switch (key_code) {
        case 0x48: out_ctrl = TEXT_CTRL_UP;     return true;
        case 0x50: out_ctrl = TEXT_CTRL_DOWN;   return true;
        case 0x4B: out_ctrl = TEXT_CTRL_LEFT;   return true;
        case 0x4D: out_ctrl = TEXT_CTRL_RIGHT;  return true;
        case 0x47: out_ctrl = TEXT_CTRL_HOME;   return true;
        case 0x4F: out_ctrl = TEXT_CTRL_END;    return true;
        case 0x49: out_ctrl = TEXT_CTRL_PGUP;   return true;
        case 0x51: out_ctrl = TEXT_CTRL_PGDN;   return true;
        case 0x52: out_ctrl = TEXT_CTRL_INSERT; return true;
        case 0x53: out_ctrl = TEXT_CTRL_DELETE; return true;
        case 0x1C: out_ctrl = TEXT_CTRL_ENTER;  return true;
        default:   return false;
        }
    } else {
        switch (key_code) {
        case 0x01: out_ctrl = TEXT_CTRL_ESCAPE; return true;
        default:   return false;
        }
    }
}

// 检查是否是需要"同时产 type=0 + type=1"的键
static bool is_dual_type_key(uint8_t key_code, bool is_extended)
{
    if (is_extended) return false;  // 上面已处理 E0 的 Enter
    switch (key_code) {
    case 0x1C: return true;  // Enter
    case 0x0F: return true;  // Tab
    case 0x0E: return true;  // Backspace
    default:   return false;
    }
}

static void produce_text_events(uint64_t seq,
                                const ps_2_keyboard_event& raw,
                                translated_events& out)
{
    out.count = 0;

    const uint8_t action = raw.action;
    const uint8_t make_code   = static_cast<uint8_t>(ps2_key_action::make);
    const uint8_t repeat_code = static_cast<uint8_t>(ps2_key_action::repeat);
    if (action != make_code && action != repeat_code)
        return;

    const bool is_extended = (raw.flags & ps2_event_flag_extended_e0) != 0;
    const bool shift    = (raw.modifiers & MOD_SHIFT) != 0;
    const bool caps     = (raw.modifiers & MOD_CAPS) != 0;
    const bool num_lock = (raw.modifiers & MOD_NUM)  != 0;
    const bool is_repeat = (action == repeat_code);

    // ---- 步骤 A：纯控制键 ----
    uint16_t ctrl_code = 0;
    if (is_control_key(raw.key_code, is_extended, ctrl_code)) {
        out.events[0].event_type = 1;
        out.events[0].reserved0 = 0;
        out.events[0].data = ctrl_code;
        out.events[0].decisive_event_seq = seq;
        out.count = 1;
        return;
    }

    // ---- 步骤 B：双类型键 (type=0 char + type=1 control) ----
    if (is_dual_type_key(raw.key_code, is_extended)) {
        char ch = 0;
        switch (raw.key_code) {
        case 0x1C: ch = '\n'; ctrl_code = TEXT_CTRL_ENTER;    break;
        case 0x0F: ch = '\t'; ctrl_code = TEXT_CTRL_TAB;      break;
        case 0x0E: ch = '\b'; ctrl_code = TEXT_CTRL_BACKSPACE; break;
        }

        // type=0 先发
        out.events[0].event_type = 0;
        out.events[0].reserved0 = 0;
        out.events[0].data = static_cast<uint16_t>(ch);
        out.events[0].decisive_event_seq = seq;

        // type=1 后发
        out.events[1].event_type = 1;
        out.events[1].reserved0 = 0;
        out.events[1].data = ctrl_code;
        out.events[1].decisive_event_seq = seq;

        out.count = 2;
        return;
    }

    // ---- 步骤 C：普通字符翻译 ----
    char ch = 0;
    uint16_t flags = 0;
    bool found = false;

    bool upper = shift ^ caps;
    if (map_letter(raw.key_code, upper, ch)) {
        found = true;
    } else if (map_non_letter(raw.key_code, shift, ch)) {
        found = true;
    } else if (map_keypad(raw.key_code, num_lock, ch)) {
        found = true;
        flags |= kbd_char_flag_from_keypad;
    }

    if (!found) return;  // 无法映射的键，静默跳过

    out.events[0].event_type = 0;
    out.events[0].reserved0 = 0;
    out.events[0].data = static_cast<uint16_t>(ch);
    out.events[0].decisive_event_seq = seq;
    out.count = 1;
}

// ============================================================
// 发布到中心 ring
// ============================================================
static void publish_one(const text_input_event& ev)
{
    uint64_t published_before = text_input_publish_seq.load(
        atomic_memory_order::relaxed);
    if (published_before >= TEXT_INPUT_RING_SIZE) {
        text_input_drop_count.add_ka(1, atomic_memory_order::relaxed);
    }

    uint16_t tail = text_input_event_tail_idx.load(atomic_memory_order::relaxed);
    text_input_ring[tail] = ev;
    text_input_event_tail_idx.store(ring_next(tail), atomic_memory_order::release);
    uint64_t old_seq = text_input_publish_seq.add_ka(1, atomic_memory_order::release);
    text_input_publish_seq_block_token = old_seq + 1;

    if (GlobalKernelStatus >= SCHEDUL_READY && text_input_subscribers_queue) {
        spinlock_interrupt_about_guard guard(text_input_subscribers_queue->lock);
        text_input_subscribers_queue->wakeup_all();
    }
}

// ============================================================
// 只读视图初始化
// ============================================================
static void init_readonly_view()
{
    phyaddr_t ring_phybase = 0;
    KURD_t kurd = KspacePageTable::v_to_phyaddrtraslation(
        (vaddr_t)text_input_ring, ring_phybase);
    if (success_all_kurd(kurd)) {
        vm_interval view{
            .vpn    = 0,
            .ppn    = ring_phybase >> 12,
            .npages = (sizeof(text_input_ring) + 0xFFF) >> 12,
            .access = KspacePageTable::PG_R
        };
        vaddr_t vbase = Kspace_pinterval_alloc_and_map(view, &kurd);
        if (vbase != 0 && success_all_kurd(kurd)) {
            text_input_ring_readonly_view = (const text_input_event*)vbase;
        }
    }
    if (text_input_ring_readonly_view == nullptr) {
        text_input_ring_readonly_view = text_input_ring;
    }
}

// ============================================================
// 订阅线程
// ============================================================
void* i8042_text_input_subscriber_main(void*)
{
    uint64_t read_seq = i8042_get_publish_seq();

    while (true) {
        uint64_t pub = i8042_get_publish_seq();
        if (read_seq == pub) {
            i8042_wait_event(read_seq);
            continue;
        }
        if ((pub - read_seq) > i8042_buffer_max_size)
            read_seq = pub - i8042_buffer_max_size;

        while (read_seq < pub) {
            ps_2_keyboard_event raw{};
            if (i8042_read_event_by_seq(read_seq, &raw)) {
                translated_events out{};
                produce_text_events(read_seq, raw, out);
                for (uint32_t i = 0; i < out.count; i++) {
                    publish_one(out.events[i]);
                }
            }
            read_seq++;
        }
    }
}

} // anonymous namespace

// ============================================================
// 对外接口实现
// ============================================================

extern "C" bool text_input_read_event_by_seq(uint64_t seq, text_input_event* out)
{
    if (!out) return false;
    const text_input_event* view =
        text_input_ring_readonly_view ? text_input_ring_readonly_view : text_input_ring;

    while (true) {
        uint64_t pub_before = text_input_publish_seq.load(atomic_memory_order::acquire);
        if (seq >= pub_before) return false;
        if ((pub_before - seq) > TEXT_INPUT_RING_SIZE) return false;

        uint16_t idx = static_cast<uint16_t>(seq % TEXT_INPUT_RING_SIZE);
        text_input_event candidate = view[idx];

        uint64_t pub_after = text_input_publish_seq.load(atomic_memory_order::acquire);
        if (pub_before != pub_after) continue;
        if (seq >= pub_after) return false;
        if ((pub_after - seq) > TEXT_INPUT_RING_SIZE) return false;

        *out = candidate;
        return true;
    }
}

extern "C" uint64_t text_input_get_publish_seq()
{
    return text_input_publish_seq.load(atomic_memory_order::acquire);
}

extern "C" void text_input_wait_event(uint64_t last_publish_seq)
{
    if (!text_input_subscribers_queue) return;
    block_if_equal(
        text_input_subscribers_queue,
        (uint64_t*)&text_input_publish_seq_block_token,
        last_publish_seq);
}

extern "C" uint32_t text_input_batch_read(
    uint64_t start_seq,
    text_input_event* out_events,
    uint32_t max_count)
{
    if (!out_events || max_count == 0) return 0;

    uint64_t pub = text_input_publish_seq.load(atomic_memory_order::acquire);
    if (start_seq >= pub) return 0;

    uint64_t avail = pub - start_seq;
    if (avail > TEXT_INPUT_RING_SIZE) {
        start_seq = pub - TEXT_INPUT_RING_SIZE;
        avail = TEXT_INPUT_RING_SIZE;
    }

    uint32_t to_read = (avail < max_count) ? (uint32_t)avail : max_count;
    const text_input_event* view =
        text_input_ring_readonly_view ? text_input_ring_readonly_view : text_input_ring;

    uint32_t idx = (uint32_t)(start_seq % TEXT_INPUT_RING_SIZE);
    uint32_t first_seg = TEXT_INPUT_RING_SIZE - idx;
    if (first_seg > to_read) first_seg = to_read;

    memcpy(out_events, &view[idx], first_seg * sizeof(text_input_event));
    if (first_seg < to_read) {
        uint32_t second_seg = to_read - first_seg;
        memcpy(out_events + first_seg, view,
               second_seg * sizeof(text_input_event));
    }
    return to_read;
}

extern "C" void text_input_subscriber_init()
{
    spinlock_interrupt_about_guard guard(text_input_subscriber_init_lock);
    if (!text_input_subscribers_queue)
        text_input_subscribers_queue = new tid_wait_queue;

    if (!text_input_ring_readonly_view)
        init_readonly_view();

    if (text_input_subscriber_tid != INVALID_TID)
        return;

    KURD_t kurd{};
    text_input_subscriber_tid = create_kthread(
        i8042_text_input_subscriber_main, nullptr, &kurd);
    if (error_kurd(kurd) || text_input_subscriber_tid == INVALID_TID) {
        text_input_subscriber_tid = INVALID_TID;
    }
}
