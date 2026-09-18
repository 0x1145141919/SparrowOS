#pragma once
#include <stdint.h>
#include "efi.h"

// ════════════════════════════════════════════════════════════════
// primitive_gop_types — GOP 图形原语共享类型
//
// 从 primitive_gop.h 拆出：仅数据类型，无任何错误语义（KURD）依赖。
// init.elf 与 kernel.elf 共用，各自经 InitGop / GfxPrim 头文件引入。
// ════════════════════════════════════════════════════════════════

typedef struct{ 
    UINT32 horizentalResolution;
    UINT32 verticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT pixelFormat;
    UINT32 PixelsPerScanLine;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINT32 FrameBufferSize;
}GlobalBasicGraphicInfoType;

// gop_info 资产名（asset_names::gop_info = "gop_info blob 0x20"）把 desc 尺寸写死为
// 0x20；结构变更会在此处被拦下（改了结构就同步改 asset_names.h 的十六进制串）。
static_assert(sizeof(GlobalBasicGraphicInfoType) == 0x20,
              "gop_info 资产名写死 0x20，改结构请同步 asset_names::gop_info");

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const void* pixels;
} GfxImage;

// 统一二维向量：用于像素坐标与平面向量
struct Vec2i {
    int x;
    int y;
};
