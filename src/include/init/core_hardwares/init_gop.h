#pragma once
#include "stdint.h"
#include "arch/x86_64/core_hardwares/primitive_gop_types.h"

// ════════════════════════════════════════════════════════════════
// InitGop — init.elf 侧 GOP 初始化原语
//
// 从 primitive_gop.h 拆出（原 4 号污染链）。init 侧无 KURD 语义，
// Init() 返回 bool 表示成败；失败由调用方决定打印与"收尸"。
// ════════════════════════════════════════════════════════════════

class InitGop {
public: 
    struct Info {
        uint32_t width;
        uint32_t height;
        uint32_t pitch_pixels;
        uint32_t format;
        uint32_t fb_bytes;
        uintptr_t fb_paddr;
        uintptr_t fb_vaddr;
    };

    static bool Init(GlobalBasicGraphicInfoType* metainf);
    static bool Ready();
    static const Info GetInfo();
    static void* FrameBuffer();

    static void Flush();
    static void FlushRect(Vec2i pos, Vec2i size);

    static void PutPixelUnsafe(Vec2i pos, uint32_t color);
    static void PutPixel(Vec2i pos, uint32_t color);
    static void DrawHLine(Vec2i pos, int len, uint32_t color);
    static void DrawVLine(Vec2i pos, int len, uint32_t color);
    static void FillRect(Vec2i pos, Vec2i size, uint32_t color);
    static void MoveUp(Vec2i pos, Vec2i size, int dy, uint32_t fill_color);
    static void Blit(Vec2i pos, const GfxImage* img);
    
private:
    static Info s_info;
    static bool s_ready;
};
