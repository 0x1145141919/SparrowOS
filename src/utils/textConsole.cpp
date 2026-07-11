#include "util/textConsole.h"
#include "util/OS_utils.h"
#include "memory/FreePagesAllocator.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "util/kout.h"
#include "Scheduler/kthread_abi.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "panic.h"
namespace {
static void textconsole_backend_write(const char* buf, uint64_t len)
{
    if (!buf || len == 0) return;
    for (uint64_t i = 0; i < len; ++i) {
        textconsole_GoP::PutChar(buf[i]);
    }
    //if(GlobalKernelStatus==kernel_state::PANIC)GfxPrim::Flush();
}

static void textconsole_backend_running_write(const char* buf, uint64_t len)
{
    if (!buf || len == 0) return;
    textconsole_GoP::RuntimeSubmitString(buf, len, false);
}

static void textconsole_backend_running_putchar(char c)
{
    textconsole_GoP::RuntimeSubmitChar(c, false);
}

static void textconsole_backend_running_num(uint64_t raw, num_format_t format, numer_system_select radix)
{
    textconsole_GoP::RuntimeSubmitNum(raw, format, radix, false);
}
    
} // namespace

GfxImage textconsole_GoP::template_character = {};
TextViewport textconsole_GoP::view = {};
TextCursor textconsole_GoP::cursor = {};
const unsigned char* textconsole_GoP::font_bitmap = nullptr;
uint32_t textconsole_GoP::font_color = 0;
uint32_t textconsole_GoP::background_color = 0;
void* textconsole_GoP::glyph_cache = nullptr;
spintrylock_cpp_t textconsole_GoP::runtime_service_create_lock = {};
tc_ring textconsole_GoP::runtime_ring = {};
bool textconsole_GoP::ready = false;
uint16_t textconsole_GoP::glyph_index[256] = {};
bool textconsole_GoP::is_font_rendered = false;
uint64_t textconsole_GoP::runtime_service_tid=INVALID_TID;
uint32_t tid_to_idx(uint64_t tid)
{
    return tid>>32;
}
KURD_t textconsole_GoP::default_kurd()
{
    return KURD_t(
        0,
        0,
        module_code::INFRA,
        infrastructure_location_code::location_code_textconsole_GoP,
        0,
        0,
        err_domain::CORE_MODULE
    );
}

KURD_t textconsole_GoP::default_success()
{
    KURD_t kurd = default_kurd();
    kurd.result = result_code::SUCCESS;
    kurd.level = level_code::INFO;
    return kurd;
}

KURD_t textconsole_GoP::default_fail()
{
    KURD_t kurd = default_kurd();
    kurd = set_result_fail_and_error_level(kurd);
    return kurd;
}

KURD_t textconsole_GoP::default_fatal()
{
    KURD_t kurd = default_kurd();
    kurd = set_fatal_result_level(kurd);
    return kurd;
}

KURD_t textconsole_GoP::Init(
    const unsigned char* font_bitmap_param,
    Vec2i cell_size,
    uint32_t font_color_param,
    uint32_t background_color_param
)
{
    KURD_t success = default_success();
    KURD_t fail = default_fail();
    success.event_code = infrastructure_location_code::textconsole_GoP_events::textconsole_GoP_event_init;
    fail.event_code = infrastructure_location_code::textconsole_GoP_events::textconsole_GoP_event_init;

    if (ready) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::already_init;
        return fail;
    }
    if (font_bitmap_param == nullptr) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::font_bitmap_is_null;
        return fail;
    }
    if (!GfxPrim::Ready()) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::gfx_not_ready;
        return fail;
    }
    if (cell_size.x != 16 || cell_size.y != 32) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::bad_cell_size;
        return fail;
    }

    kio::kout_backend backend = {
        .name = "textconsole_gop",
        .is_masked = 0,
        .reserved = 0,
        .running_stage_write = &textconsole_backend_running_write,
        .running_stage_putchar = &textconsole_backend_running_putchar,
        .running_stage_num = &textconsole_backend_running_num,
        .panic_write = &textconsole_backend_write,
        .early_write = &textconsole_backend_write,
    };
    if (bsp_kout.register_backend(backend) == ~0ULL) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::backend_register_fail;
        return fail;
    }

    const GfxPrim::Info info = GfxPrim::GetInfo();
    view.pos = {0, 0};
    view.size = {static_cast<int>(info.width), static_cast<int>(info.height)};
    view.cell = cell_size;
    view.cols = view.size.x / view.cell.x;
    view.rows = view.size.y / view.cell.y;
    if (view.cols <= 0 || view.rows <= 0) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::zero_grid;
        return fail;
    }

    // 初始化直接位图渲染模式所需的参数
    font_bitmap = font_bitmap_param;
    font_color = font_color_param;
    background_color = background_color_param;

    // 初始化 glyph_index 表（用于后续 enable_font_render）
    static constexpr uint32_t kFirstPrintable = 0x20;
    static constexpr uint32_t kLastPrintable = 0x7E;
    static constexpr uint32_t kPrintableCount = kLastPrintable - kFirstPrintable + 1;
    static constexpr uint32_t kPlaceholderIndex = 0;
    static constexpr uint32_t kPrintableBaseIndex = 1;
    const uint32_t glyph_count = kPrintableCount + 1;
    
    for (uint32_t ch = 0; ch < 256; ++ch) {
        if (ch >= kFirstPrintable && ch <= kLastPrintable) {
            glyph_index[ch] = static_cast<uint16_t>(kPrintableBaseIndex + (ch - kFirstPrintable));
        } else {
            glyph_index[ch] = static_cast<uint16_t>(kPlaceholderIndex);
        }
    }

    // 初始化状态：直接位图渲染模式
    glyph_cache = nullptr;
    template_character.width = 0;
    template_character.height = 0;
    template_character.stride_bytes = 0;
    template_character.pixels = nullptr;

    cursor = {0, 0};
    runtime_ring.head = 0;
    runtime_ring.tail = 0;
    runtime_ring.seq_gen = 0;
    runtime_ring.drop_count = 0;
    runtime_ring.push_count = 0;
    runtime_ring.pop_count = 0;
    ready = true;
    return success;
}
KURD_t textconsole_GoP::enable_font_render()
{
    KURD_t success = default_success();
    KURD_t fail = default_fail();
    success.event_code = infrastructure_location_code::textconsole_GoP_events::textconsole_GoP_event_init;
    fail.event_code = infrastructure_location_code::textconsole_GoP_events::textconsole_GoP_event_init;

    if (!ready) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::gfx_not_ready;
        return fail;
    }

    if (is_font_rendered) {
        // 已经启用过字体渲染，直接返回成功
        return success;
    }

    // 申请 glyph_cache 内存
    static constexpr uint32_t kFirstPrintable = 0x20;
    static constexpr uint32_t kLastPrintable = 0x7E;
    static constexpr uint32_t kPrintableCount = kLastPrintable - kFirstPrintable + 1;
    static constexpr uint32_t kPlaceholderIndex = 0;
    static constexpr uint32_t kPrintableBaseIndex = 1;
    const uint32_t glyph_count = kPrintableCount + 1;
    const uint64_t glyph_bytes = static_cast<uint64_t>(view.cell.x) * view.cell.y * 4;
    const uint64_t total_bytes = glyph_bytes * glyph_count;
    const uint64_t alloc_bytes = align_up(total_bytes, 0x1000);
    const uint64_t page_count = alloc_bytes / 0x1000;

    KURD_t kurd = empty_kurd;
    void* new_glyph_cache = __wrapped_pgs_valloc(&kurd, page_count, page_state_t::kernel_pinned, 12);
    if (new_glyph_cache == nullptr || error_kurd(kurd)) {
        fail.reason = infrastructure_location_code::textconsole_GoP_events::init_results::fail_reasons::glyph_cache_alloc_fail;
        return error_kurd(kurd) ? kurd : fail;
    }

    // 设置 template_character
    template_character.width = static_cast<uint32_t>(view.cell.x);
    template_character.height = static_cast<uint32_t>(view.cell.y);
    template_character.stride_bytes = static_cast<uint32_t>(view.cell.x) * 4;
    template_character.pixels = new_glyph_cache;

    // 预渲染所有字符到 glyph_cache
    const uint8_t* bitmap = font_bitmap;
    const uint32_t bytes_per_row = 2;
    const uint32_t row_bytes = template_character.stride_bytes;
    const uint64_t color64_bg = (static_cast<uint64_t>(background_color) << 32) | background_color;

    for (uint32_t idx = 0; idx < glyph_count; ++idx) {
        uint32_t render_idx = (idx == kPlaceholderIndex)
            ? 2
            : (kFirstPrintable + (idx - kPrintableBaseIndex));
        uint8_t* glyph_base = static_cast<uint8_t*>(new_glyph_cache) + glyph_bytes * idx;
        const uint8_t* glyph_bitmap = bitmap + render_idx * view.cell.y * bytes_per_row;

        for (int row = 0; row < view.cell.y; ++row) {
            uint8_t* dst_row = glyph_base + static_cast<uint64_t>(row) * row_bytes;
            ksetmem_64(dst_row, color64_bg, row_bytes);

            uint8_t b0 = glyph_bitmap[row * bytes_per_row + 0];
            uint8_t b1 = glyph_bitmap[row * bytes_per_row + 1];
            for (int bit = 0; bit < 8; ++bit) {
                if (b0 & (0x80 >> bit)) {
                    reinterpret_cast<uint32_t*>(dst_row)[bit] = font_color;
                }
            }
            for (int bit = 0; bit < 8; ++bit) {
                if (b1 & (0x80 >> bit)) {
                    reinterpret_cast<uint32_t*>(dst_row)[8 + bit] = font_color;
                }
            }
        }
    }

    glyph_cache = new_glyph_cache;
    is_font_rendered = true;
    return success;
}
bool textconsole_GoP::Ready()
{
    return ready;
}

void textconsole_GoP::PutChar(char ch)
{
    if (!ready) return;

    switch (ch) {
        case '\0':
            return;
        case '\n':
            cursor.m = 0;
            cursor.n += 1;
            break;
        case '\r'://整行绘制为空
            cursor.m = 0;
            break;
        case '\t': {
            static constexpr int kTabWidth = 4;
            cursor.m += kTabWidth;
            break;
        }
        case '\b': {
            // 标准终端行为：\b 仅移动光标，不擦除字符
            if (cursor.m > 0) {
                cursor.m -= 1;
            } else if (cursor.n > 0) {
                cursor.n -= 1;
                cursor.m = view.cols - 1;
            }
            break;
        }
        default: {
            unsigned char uch = static_cast<unsigned char>(ch);
            
            if (is_font_rendered) {
                // 缓存渲染模式：使用 glyph_cache
                uint16_t idx = glyph_index[uch];
                const uint64_t glyph_bytes = static_cast<uint64_t>(view.cell.x) * view.cell.y * 4;
                template_character.pixels = static_cast<uint8_t*>(glyph_cache) + glyph_bytes * idx;

                Vec2i pos = {
                    view.pos.x + cursor.m * view.cell.x,
                    view.pos.y + cursor.n * view.cell.y
                };
                GfxPrim::Blit(pos, &template_character);
            } else {
                // 直接位图渲染模式：逐 bit调用 PutPixel
                const uint32_t bytes_per_row = 2;
                const uint8_t* char_bitmap = font_bitmap + uch * view.cell.y * bytes_per_row;
                
                Vec2i base_pos = {
                    view.pos.x + cursor.m * view.cell.x,
                    view.pos.y + cursor.n * view.cell.y
                };
                
                // 先用背景色填充字符区域
                GfxPrim::FillRect(base_pos, view.cell, background_color);
                
                // 逐行逐 bit 绘制前景像素
                for (int row = 0; row < view.cell.y; ++row) {
                    const uint8_t bitmap_byte = char_bitmap[row * bytes_per_row];
                    for (int bit = 0; bit < 8; ++bit) {
                        if (bitmap_byte & (0x80 >> bit)) {
                            Vec2i pixel_pos = {
                                base_pos.x + bit,
                                base_pos.y + row
                            };
                            GfxPrim::PutPixel(pixel_pos, font_color);
                        }
                    }
                    // 处理第二个字节（如果有）
                    if (bytes_per_row > 1) {
                        const uint8_t bitmap_byte2 = char_bitmap[row * bytes_per_row + 1];
                        for (int bit = 0; bit < 8; ++bit) {
                            if (bitmap_byte2 & (0x80 >> bit)) {
                                Vec2i pixel_pos = {
                                    base_pos.x + 8 + bit,
                                    base_pos.y + row
                                };
                                GfxPrim::PutPixel(pixel_pos, font_color);
                            }
                        }
                    }
                }
            }

            cursor.m += 1;
            break;
        }
    }

    if (cursor.m >= view.cols) {
        cursor.m = 0;
        cursor.n += 1;
    }
    if (cursor.n >= view.rows) {
        GfxPrim::MoveUp(view.pos, view.size, view.cell.y, background_color);
        cursor.n = view.rows - 1;
        GfxPrim::Flush(); 
    }
}

void textconsole_GoP::PutString(const char* s,uint64_t len)
{
    if (!ready || s == nullptr) return;
    uint64_t left=len;
    for (const char* p = s; *p&&left; ++p,left--) {
        PutChar(*p);
    }
}

void textconsole_GoP::Clear()
{
    if (!ready) return;
    GfxPrim::FillRect(view.pos, view.size, background_color);
    cursor.m = 0;
    cursor.n = 0;
    GfxPrim::Flush();
}

bool textconsole_GoP::RuntimeInitServiceThread()
{
    if (!ready) return false;
    runtime_service_create_lock.lock();
    if (runtime_service_tid!=INVALID_TID) {
        runtime_service_create_lock.unlock();
        return true;
    }
    kthread_creating_package pkg;
    pkg.func_raw = (uint64_t)textconsole_GoP::RuntimeServiceThreadMain;
    pkg.args[0] = 0;
    pkg.args[1] = 0;
    pkg.args[2] = 0;
    pkg.args[3] = 0;
    pkg.args[4] = 0;
    pkg.launch_pid = fast_get_processor_id();
    KURD_t kurd = KURD_t();
    runtime_service_tid = creat_kthread(&pkg, &kurd);
    runtime_service_create_lock.unlock();
    return true;
}

void textconsole_GoP::RuntimeWakeServiceThread()
{
    wakeup_thread(runtime_service_tid);
}

bool textconsole_GoP::RuntimeSubmitString(const char* s, uint64_t len, bool urgent)
{
    if (!ready || runtime_service_tid==INVALID_TID || !s || len == 0) return false;

    runtime_ring.lock.lock();
    const uint32_t next_tail = (runtime_ring.tail + 1) & (TC_RING_CAP - 1);
    if (next_tail == runtime_ring.head) {
        runtime_ring.drop_count++;
        runtime_ring.lock.unlock();
        return false;
    }
    tc_slot& slot = runtime_ring.slots[runtime_ring.tail];
    slot.head.type = tc_msg_type::string;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? tc_msg_flag_urgent : tc_msg_flag_none;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = runtime_ring.seq_gen++;
    slot.payload.s.string = s;
    slot.payload.s.len = len;
    runtime_ring.tail = next_tail;
    runtime_ring.push_count++;
    runtime_ring.lock.unlock();

    RuntimeWakeServiceThread();
    return true;
}

bool textconsole_GoP::RuntimeSubmitChar(char ch, bool urgent)
{
    if (!ready || runtime_service_tid==INVALID_TID) return false;

    runtime_ring.lock.lock();
    const uint32_t next_tail = (runtime_ring.tail + 1) & (TC_RING_CAP - 1);
    if (next_tail == runtime_ring.head) {
        runtime_ring.drop_count++;
        runtime_ring.lock.unlock();
        return false;
    }
    tc_slot& slot = runtime_ring.slots[runtime_ring.tail];
    slot.head.type = tc_msg_type::single_character;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? tc_msg_flag_urgent : tc_msg_flag_none;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = runtime_ring.seq_gen++;
    slot.payload.c.ch = ch;
    runtime_ring.tail = next_tail;
    runtime_ring.push_count++;
    runtime_ring.lock.unlock();

    RuntimeWakeServiceThread();
    return true;
}

bool textconsole_GoP::RuntimeSubmitNum(uint64_t raw, num_format_t format, numer_system_select radix, bool urgent)
{
    if (!ready || runtime_service_tid==INVALID_TID) return false;

    runtime_ring.lock.lock();
    const uint32_t next_tail = (runtime_ring.tail + 1) & (TC_RING_CAP - 1);
    if (next_tail == runtime_ring.head) {
        runtime_ring.drop_count++;
        runtime_ring.lock.unlock();
        return false;
    }
    tc_slot& slot = runtime_ring.slots[runtime_ring.tail];
    slot.head.type = tc_msg_type::num;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? tc_msg_flag_urgent : tc_msg_flag_none;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = runtime_ring.seq_gen++;
    slot.payload.n.num_raw = raw;
    slot.payload.n.format = format;
    slot.payload.n.radix = radix;
    runtime_ring.tail = next_tail;
    runtime_ring.push_count++;
    runtime_ring.lock.unlock();

    RuntimeWakeServiceThread();
    return true;
}

bool textconsole_GoP::RuntimeSubmitFlush(bool urgent)
{
    if (!ready || runtime_service_tid==INVALID_TID) return false;

    runtime_ring.lock.lock();
    const uint32_t next_tail = (runtime_ring.tail + 1) & (TC_RING_CAP - 1);
    if (next_tail == runtime_ring.head) {
        runtime_ring.drop_count++;
        runtime_ring.lock.unlock();
        return false;
    }
    tc_slot& slot = runtime_ring.slots[runtime_ring.tail];
    slot.head.type = tc_msg_type::flush;
    slot.head.reserved = 0;
    slot.head.flags = urgent ? tc_msg_flag_urgent : tc_msg_flag_none;
    slot.head.producer_cpu = fast_get_processor_id();
    slot.head.seq = runtime_ring.seq_gen++;
    runtime_ring.tail = next_tail;
    runtime_ring.push_count++;
    runtime_ring.lock.unlock();

    RuntimeWakeServiceThread();
    return true;
}

void* textconsole_GoP::RuntimeServiceThreadMain(void* data)
{
    (void)data;
    tc_service_local_batch local_batch{};
    char num_buf[70];

    auto next_index = [](uint32_t idx) -> uint32_t {
        return (idx + 1) & (TC_RING_CAP - 1);
    };
    for (;;) {
        local_batch.count = 0;
        runtime_ring.lock.lock();
        while (runtime_ring.head != runtime_ring.tail &&
               local_batch.count < TC_SERVICE_POP_BATCH) {
            local_batch.items[local_batch.count] = runtime_ring.slots[runtime_ring.head];
            runtime_ring.head = next_index(runtime_ring.head);
            runtime_ring.pop_count++;
            local_batch.count++;
        }
        runtime_ring.lock.unlock();

        if (local_batch.count == 0) {
            kthread_self_blocked(task_blocked_reason_t::no_job);
            continue;
        }

        bool need_flush = false;
        for (uint32_t i = 0; i < local_batch.count; ++i) {
            const tc_slot& slot = local_batch.items[i];
            switch (slot.head.type) {
                case tc_msg_type::string:
                    for (uint64_t j = 0; j < slot.payload.s.len; ++j) {
                        PutChar(slot.payload.s.string[j]);
                    }
                    break;
                case tc_msg_type::single_character:
                    PutChar(slot.payload.c.ch);
                    break;
                case tc_msg_type::num: {
                    uint64_t n = format_num_to_buffer(
                        num_buf,
                        slot.payload.n.num_raw,
                        slot.payload.n.format,
                        slot.payload.n.radix
                    );
                    for (uint64_t j = 0; j < n; ++j) {
                        PutChar(num_buf[j]);
                    }
                    break;
                }
                case tc_msg_type::flush:
                    need_flush = true;
                    break;
                default:
                    break;
            }
            if (slot.head.flags & tc_msg_flag_urgent) {
                need_flush = true;
            }
        }
        if (need_flush) {
            GfxPrim::Flush();
        }
    }
}
