#include "init/util/textConsole.h"
#include "util/OS_utils.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "init/util/kout.h"

TextViewport init_textconsole::view = {};
TextCursor init_textconsole::cursor = {};
const unsigned char* init_textconsole::font_bitmap = nullptr;
uint32_t init_textconsole::font_color = 0;
uint32_t init_textconsole::background_color = 0;
bool init_textconsole::ready = false;
uint16_t init_textconsole::glyph_index[256] = {};
uint64_t init_textconsole::fb_backend_index = ~0ULL;

// kout backend wrapper: PutString → fb
static void textconsole_write(const char* buf, uint64_t len) {
    init_textconsole::PutString(buf, len);
}

KURD_t init_textconsole::default_kurd()
{
    return KURD_t(
        0,
        0,
        module_code::INFRA,
        infrastructure_location_code::location_code_init_textconsole,
        0,
        0,
        err_domain::CORE_MODULE
    );
}

KURD_t init_textconsole::default_success()
{
    KURD_t kurd = default_kurd();
    kurd.result = result_code::SUCCESS;
    kurd.level = level_code::INFO;
    return kurd;
}

KURD_t init_textconsole::default_fail()
{
    KURD_t kurd = default_kurd();
    kurd = set_result_fail_and_error_level(kurd);
    return kurd;
}

KURD_t init_textconsole::default_fatal()
{
    KURD_t kurd = default_kurd();
    kurd = set_fatal_result_level(kurd);
    return kurd;
}

void init_textconsole::render_glyph(int m, int n, unsigned char ch)
{
    uint16_t idx = glyph_index[ch];
    const int glyph_width = 16;
    const int glyph_height = 32;
    const int bytes_per_row = 2;
    
    Vec2i pos = {
        view.pos.x + m * view.cell.x,
        view.pos.y + n * view.cell.y
    };
    
    const uint8_t* glyph_bitmap = font_bitmap + idx * glyph_height * bytes_per_row;
    
    for (int row = 0; row < glyph_height; ++row) {
        uint8_t b0 = glyph_bitmap[row * bytes_per_row + 0];
        uint8_t b1 = glyph_bitmap[row * bytes_per_row + 1];
        
        for (int bit = 0; bit < 8; ++bit) {
            if (b0 & (0x80 >> bit)) {
                InitGop::PutPixel({pos.x + bit, pos.y + row}, font_color);
            }
        }
        for (int bit = 0; bit < 8; ++bit) {
            if (b1 & (0x80 >> bit)) {
                InitGop::PutPixel({pos.x + 8 + bit, pos.y + row}, font_color);
            }
        }
    }
}

KURD_t init_textconsole::Init(
    const unsigned char* font_bitmap_param,
    Vec2i cell_size,
    uint32_t font_color_param,
    uint32_t background_color_param
)
{
    KURD_t success = default_success();
    KURD_t fail = default_fail();
    success.event_code = infrastructure_location_code::init_textconsole_events::textconsole_event_init;
    fail.event_code = infrastructure_location_code::init_textconsole_events::textconsole_event_init;

    if (ready) {
        fail.reason = infrastructure_location_code::init_textconsole_events::init_results::fail_reasons::already_init;
        return fail;
    }
    if (font_bitmap_param == nullptr) {
        fail.reason = infrastructure_location_code::init_textconsole_events::init_results::fail_reasons::font_bitmap_is_null;
        return fail;
    }
    if (!InitGop::Ready()) {
        fail.reason = infrastructure_location_code::init_textconsole_events::init_results::fail_reasons::gfx_not_ready;
        return fail;
    }
    if (cell_size.x != 16 || cell_size.y != 32) {
        fail.reason = infrastructure_location_code::init_textconsole_events::init_results::fail_reasons::bad_cell_size;
        return fail;
    }

    const InitGop::Info info = InitGop::GetInfo();
    view.pos = {0, 0};
    view.size = {static_cast<int>(info.width), static_cast<int>(info.height)};
    view.cell = cell_size;
    view.cols = view.size.x / view.cell.x;
    view.rows = view.size.y / view.cell.y;
    if (view.cols <= 0 || view.rows <= 0) {
        fail.reason = infrastructure_location_code::init_textconsole_events::init_results::fail_reasons::zero_grid;
        return fail;
    }

    font_bitmap = font_bitmap_param;
    font_color = font_color_param;
    background_color = background_color_param;
    for (uint32_t ch = 0; ch < 256; ++ch) {
        glyph_index[ch] = ch;
    }

    cursor = {0, 0};
    ready = true;

    // 清屏：填充背景色 + 复位光标
    Clear();

    return success;
}

bool init_textconsole::Ready()
{
    return ready;
}

void init_textconsole::PutChar(char ch)
{
    if (!ready) return;

    switch (ch) {
        case '\0':
            return;
        case '\n':
            cursor.m = 0;
            cursor.n += 1;
            break;
        case '\r':
            cursor.m = 0;
            break;
        case '\t': {
            static constexpr int kTabWidth = 4;
            cursor.m += kTabWidth;
            break;
        }
        case '\b': {
            if (cursor.m > 0) {
                cursor.m -= 1;
            } else if (cursor.n > 0) {
                cursor.n -= 1;
                cursor.m = view.cols - 1;
            } else {
                break;
            }
            Vec2i pos = {
                view.pos.x + cursor.m * view.cell.x,
                view.pos.y + cursor.n * view.cell.y
            };
            InitGop::FillRect(pos, view.cell, background_color);
            break;
        }
        default: {
            unsigned char uch = static_cast<unsigned char>(ch);
            render_glyph(cursor.m, cursor.n, uch);
            cursor.m += 1;
            break;
        }
    }

    if (cursor.m >= view.cols) {
        cursor.m = 0;
        cursor.n += 1;
    }
    if (cursor.n >= view.rows) {
        InitGop::MoveUp(view.pos, view.size, view.cell.y, background_color);
        cursor.n = view.rows - 1;
        InitGop::Flush(); 
    }
}

void init_textconsole::PutString(const char* s,uint64_t len)
{
    if (!ready || s == nullptr) return;
    uint64_t left=len;
    for (const char* p = s; *p&& len; ++p,len--) {
        PutChar(*p);
    }
}

void init_textconsole::Clear()
{
    if (!ready) return;
    InitGop::FillRect(view.pos, view.size, background_color);
    cursor.m = 0;
    cursor.n = 0;
    InitGop::Flush();
}
