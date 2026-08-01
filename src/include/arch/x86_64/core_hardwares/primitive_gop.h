#pragma once
#include <stdint.h>
#include "abi/os_error_definitions.h"
#include "abi/boot.h"
#include "efi.h"
#include "arch/x86_64/core_hardwares/primitive_gop_types.h"

// ════════════════════════════════════════════════════════════════
// primitive_gop — GfxPrim（kernel 侧图形原语）
//
// init.elf 侧不再使用本头：InitGop 已拆至 init/core_hardwares/init_gop.h，
// 共享类型拆至 primitive_gop_types.h。本头仅保留 kernel 侧 GfxPrim 及其
// KURD 位置树。
// ════════════════════════════════════════════════════════════════

namespace COREHARDWARES_LOCATIONS{
    constexpr uint8_t LOCATION_CODE_GOP_PRIMITIVE=0x02;
    namespace GOP_PRIMITIVE_DRIVERS_EVENTS {
        constexpr uint8_t INIT=0;
        namespace INIT_RESULTS {
            namespace FAIL_REASONS{
                constexpr uint16_t PARAM_METAINF_NULLPTR = 0x01;
                constexpr uint16_t BAD_PARAM = 0x02;
                constexpr uint16_t ALLREADE_INIT = 0x03;
            }
        }
    }
};

class GfxPrim {
    //只画合法像素，非法越界的部分静默失败
public: 
    struct Info {
        uint32_t width;              // horizentalResolution
        uint32_t height;             // verticalResolution
        uint32_t pitch_pixels;       // PixelsPerScanLine
        uint32_t format;             // PixelBlueGreenRedReserved8BitPerColor
        uint32_t fb_bytes;           // FrameBufferSize
        uintptr_t fb_paddr;          // TFG 提供的物理地址
        uintptr_t fb_vaddr;          // 经过重映射的虚拟地址（WC）
        void* backbuffer;            // DRAM 后备缓冲区（WB），大小 == fb_bytes
    };

    // 唯一入口
    static KURD_t Init(GlobalBasicGraphicInfoType* metainf,vm_interval interval);
    static bool Ready();
    static const Info GetInfo();
    static void* BackBuffer();

    // Flush
    static void Flush();
    static void FlushRect(Vec2i pos, Vec2i size);

    // Pixel / Geometry
    static void PutPixelUnsafe(Vec2i pos, uint32_t color);//不校验，直接根据pos计算内存位置写入
    static void PutPixel(Vec2i pos, uint32_t color);
    static void DrawHLine(Vec2i pos, int len, uint32_t color);
    static void DrawVLine(Vec2i pos, int len, uint32_t color);
    static void FillRect(Vec2i pos, Vec2i size, uint32_t color);
    // Screen move (有backbuffer时只在backbuffer上，无backbuffer时在 only)
    static void MoveUp(Vec2i pos, Vec2i size, int dy, uint32_t fill_color);

    // Image blit
    // Blit：底层用ksysramcopy复制，调用者应该知晓像素格式合适渲染
    static void Blit(Vec2i pos, const GfxImage* img);
    static KURD_t enable_ram_buffer();
private:
    static Info s_info;
    static bool s_ready;
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();
};
