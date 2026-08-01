#pragma once
#include "stdint.h"
#include "init/core_hardwares/init_gop.h"
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

class init_textconsole {
public:
    static bool Init(
        const unsigned char* font_bitmap,
        Vec2i cell_size,
        uint32_t font_color,
        uint32_t background_color
    );

    static bool Ready();
    static void PutChar(char ch);
    static void PutString(const char* s,uint64_t len);
    static void Clear();
    static uint64_t FrameBufferBackendIndex() { return fb_backend_index; }
    
private:
    static void render_glyph(int m, int n, unsigned char ch);
    static TextViewport view;
    static TextCursor cursor;
    static const unsigned char* font_bitmap;
    static uint32_t font_color;
    static uint32_t background_color;
    static bool ready;
    static uint16_t glyph_index[256];
    static uint64_t fb_backend_index;
};
