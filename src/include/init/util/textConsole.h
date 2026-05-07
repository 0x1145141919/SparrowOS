#pragma once
#include "stdint.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
struct TextCursor {
    int m;          // 列
    int n;          // 行
};
struct TextViewport {
    Vec2i pos;      // 文字区域左上角（像素）
    Vec2i size;     // 文字区域大小（像素）
    Vec2i cell;     // 字符宽高（像素）
    int cols;       // size.x / cell.x
    int rows;       // size.y / cell.y
};

namespace infrastructure_location_code{
    constexpr uint8_t location_code_init_textconsole=5;
    namespace init_textconsole_events{
        constexpr uint8_t textconsole_event_init=0;
        namespace init_results{
            namespace fail_reasons{
                constexpr uint8_t font_bitmap_is_null=0;
                constexpr uint8_t gfx_not_ready=1;
                constexpr uint8_t bad_cell_size=2;
                constexpr uint8_t zero_grid=3;
                constexpr uint8_t already_init=4;
            }
        }
    }
}

class init_textconsole {
public:
    static KURD_t Init(
        const unsigned char* font_bitmap,
        Vec2i cell_size,
        uint32_t font_color,
        uint32_t background_color
    );

    static bool Ready();
    static void PutChar(char ch);
    static void PutString(const char* s,uint64_t len);
    static void Clear();
    
private:
    static void render_glyph(int m, int n, unsigned char ch);
    static TextViewport view;
    static TextCursor cursor;
    static const unsigned char* font_bitmap;
    static uint32_t font_color;
    static uint32_t background_color;
    static bool ready;
    static uint16_t glyph_index[256];
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();
};
