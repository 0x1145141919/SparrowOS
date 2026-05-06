#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "util/OS_utils.h"


namespace {
static constexpr uint64_t kPageSize = 0x1000;

static bool is_4k_aligned(uint64_t value) {
    return (value & (kPageSize - 1)) == 0;
}
} // namespace

GfxPrim::Info GfxPrim::s_info = {};
bool GfxPrim::s_ready = false;

KURD_t GfxPrim::default_kurd()
{
    return KURD_t(
        0,
        0,
        module_code::DEVICES_CORE,
        COREHARDWARES_LOCATIONS::LOCATION_CODE_GOP_PRIMITIVE,
        0,
        0,
        err_domain::CORE_MODULE
    );
}

KURD_t GfxPrim::default_success()
{
    KURD_t kurd = default_kurd();
    kurd.result = result_code::SUCCESS;
    kurd.level = level_code::INFO;
    return kurd;
}

KURD_t GfxPrim::default_fail()
{
    KURD_t kurd = default_kurd();
    kurd = set_result_fail_and_error_level(kurd);
    return kurd;
}

KURD_t GfxPrim::default_fatal()
{
    KURD_t kurd = default_kurd();
    kurd = set_fatal_result_level(kurd);
    return kurd;
}

KURD_t GfxPrim::Init(GlobalBasicGraphicInfoType *metainf, loaded_VM_interval interval)
{
    s_info.format = metainf->pixelFormat;
    s_info.width=metainf->horizentalResolution;
    s_info.height=metainf->verticalResolution;
    s_info.pitch_pixels = metainf->PixelsPerScanLine;
    s_info.fb_bytes=metainf->FrameBufferSize;
    s_info.backbuffer = nullptr;
    s_info.fb_vaddr=interval.vbase;
    s_info.fb_paddr=metainf->FrameBufferBase;
    KURD_t success=default_success();
    success.event_code=COREHARDWARES_LOCATIONS::GOP_PRIMITIVE_DRIVERS_EVENTS::INIT;
    s_ready=true;
    return success;
}

void GfxPrim::PutPixelUnsafe(Vec2i pos, uint32_t color)
{
    if (!s_ready || s_info.fb_vaddr == 0) return;
    
    void* target_buffer = (s_info.backbuffer != nullptr) ? s_info.backbuffer : reinterpret_cast<void*>(s_info.fb_vaddr);
    uint32_t* base = static_cast<uint32_t*>(target_buffer);
    base[static_cast<uint64_t>(pos.y) * s_info.pitch_pixels + static_cast<uint64_t>(pos.x)] = color;
}

void GfxPrim::PutPixel(Vec2i pos, uint32_t color)
{
    if (!s_ready || s_info.fb_vaddr == 0) return;
    if (pos.x < 0 || pos.y < 0) return;
    if (static_cast<uint32_t>(pos.x) >= s_info.width || static_cast<uint32_t>(pos.y) >= s_info.height) return;
    PutPixelUnsafe(pos, color);
}

void GfxPrim::DrawHLine(Vec2i pos, int len, uint32_t color)
{
    if (!s_ready || s_info.fb_vaddr == 0) return;
    if (pos.y < 0 || static_cast<uint32_t>(pos.y) >= s_info.height) return;
    if (len == 0) return;

    if (len < 0) {
        pos.x -= (len + 1);
        len = -len;
    }

    int x0 = pos.x;
    int x1 = pos.x + len - 1;
    if (x1 < 0 || x0 >= static_cast<int>(s_info.width)) return;

    if (x0 < 0) x0 = 0;
    if (x1 >= static_cast<int>(s_info.width)) x1 = static_cast<int>(s_info.width) - 1;

    void* target_buffer = (s_info.backbuffer != nullptr) ? s_info.backbuffer : reinterpret_cast<void*>(s_info.fb_vaddr);
    uint32_t* base = static_cast<uint32_t*>(target_buffer);
    uint64_t row = static_cast<uint64_t>(pos.y) * s_info.pitch_pixels;
    uint32_t* start = base + row + static_cast<uint64_t>(x0);
    uint32_t count = static_cast<uint32_t>(x1 - x0 + 1);
    uint64_t color64 = (static_cast<uint64_t>(color) << 32) | color;
    ksetmem_64(start, color64, static_cast<uint64_t>(count) * sizeof(uint32_t));
}

void GfxPrim::DrawVLine(Vec2i pos, int len, uint32_t color)
{
    if (!s_ready || s_info.fb_vaddr == 0) return;
    if (pos.x < 0 || static_cast<uint32_t>(pos.x) >= s_info.width) return;
    if (len == 0) return;

    if (len < 0) {
        pos.y -= (len + 1);
        len = -len;
    }

    int y0 = pos.y;
    int y1 = pos.y + len - 1;
    if (y1 < 0 || y0 >= static_cast<int>(s_info.height)) return;

    if (y0 < 0) y0 = 0;
    if (y1 >= static_cast<int>(s_info.height)) y1 = static_cast<int>(s_info.height) - 1;

    void* target_buffer = (s_info.backbuffer != nullptr) ? s_info.backbuffer : reinterpret_cast<void*>(s_info.fb_vaddr);
    uint32_t* base = static_cast<uint32_t*>(target_buffer);
    for (int y = y0; y <= y1; ++y) {
        base[static_cast<uint64_t>(y) * s_info.pitch_pixels + static_cast<uint64_t>(pos.x)] = color;
    }
}

void GfxPrim::FillRect(Vec2i pos, Vec2i size, uint32_t color)
{
    if (!s_ready || s_info.fb_vaddr == 0) return;
    if (size.x <= 0 || size.y <= 0) return;

    int x0 = pos.x;
    int y0 = pos.y;
    int x1 = pos.x + size.x - 1;
    int y1 = pos.y + size.y - 1;

    if (x1 < 0 || y1 < 0) return;
    if (x0 >= static_cast<int>(s_info.width) || y0 >= static_cast<int>(s_info.height)) return;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= static_cast<int>(s_info.width)) x1 = static_cast<int>(s_info.width) - 1;
    if (y1 >= static_cast<int>(s_info.height)) y1 = static_cast<int>(s_info.height) - 1;

    const uint32_t row_len = static_cast<uint32_t>(x1 - x0 + 1);
    void* target_buffer = (s_info.backbuffer != nullptr) ? s_info.backbuffer : reinterpret_cast<void*>(s_info.fb_vaddr);
    uint32_t* base = static_cast<uint32_t*>(target_buffer);
    uint64_t color64 = (static_cast<uint64_t>(color) << 32) | color;
    for (int y = y0; y <= y1; ++y) {
        uint32_t* row = base + static_cast<uint64_t>(y) * s_info.pitch_pixels + static_cast<uint64_t>(x0);
        ksetmem_64(row, color64, static_cast<uint64_t>(row_len) * sizeof(uint32_t));
    }
}

void GfxPrim::MoveUp(Vec2i pos, Vec2i size, int dy, uint32_t fill_color)
{
    if (!s_ready || s_info.fb_vaddr == 0) return;
    if (dy <= 0) return;
    if (size.x <= 0 || size.y <= 0) return;

    int x0 = pos.x;
    int y0 = pos.y;
    int x1 = pos.x + size.x - 1;
    int y1 = pos.y + size.y - 1;

    if (x1 < 0 || y1 < 0) return;
    if (x0 >= static_cast<int>(s_info.width) || y0 >= static_cast<int>(s_info.height)) return;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= static_cast<int>(s_info.width)) x1 = static_cast<int>(s_info.width) - 1;
    if (y1 >= static_cast<int>(s_info.height)) y1 = static_cast<int>(s_info.height) - 1;

    int height = y1 - y0 + 1;
    if (dy >= height) {
        FillRect({x0, y0}, {x1 - x0 + 1, height}, fill_color);
        return;
    }

    const uint32_t row_len = static_cast<uint32_t>(x1 - x0 + 1);
    const uint64_t row_bytes = static_cast<uint64_t>(row_len) * sizeof(uint32_t);
    const uint64_t pitch_bytes = static_cast<uint64_t>(s_info.pitch_pixels) * sizeof(uint32_t);

    // 统一使用目标缓冲区（backbuffer 或 fb_vaddr）
    void* target_buffer = (s_info.backbuffer != nullptr) ? s_info.backbuffer : reinterpret_cast<void*>(s_info.fb_vaddr);
    uint8_t* base = static_cast<uint8_t*>(target_buffer);

    // 从下往上复制，避免数据覆盖问题
    for (int y = 0; y < height - dy; ++y) {
        uint8_t* dst = base + static_cast<uint64_t>(y0 + y) * pitch_bytes + static_cast<uint64_t>(x0) * sizeof(uint32_t);
        uint8_t* src = base + static_cast<uint64_t>(y0 + y + dy) * pitch_bytes + static_cast<uint64_t>(x0) * sizeof(uint32_t);
        ksystemramcpy(src, dst, row_bytes);
    }

    // 填充底部区域
    uint32_t* fill_base = reinterpret_cast<uint32_t*>(base);
    uint64_t color64 = (static_cast<uint64_t>(fill_color) << 32) | fill_color;
    for (int y = height - dy; y < height; ++y) {
        uint32_t* row = fill_base + static_cast<uint64_t>(y0 + y) * s_info.pitch_pixels + static_cast<uint64_t>(x0);
        ksetmem_64(row, color64, row_bytes);
    }
}

void GfxPrim::Blit(Vec2i pos, const GfxImage* img)
{
    if (!s_ready || s_info.fb_vaddr == 0 || img == nullptr || img->pixels == nullptr) return;
    if (img->width == 0 || img->height == 0 || img->stride_bytes == 0) return;

    int x0 = pos.x;
    int y0 = pos.y;
    int x1 = pos.x + static_cast<int>(img->width) - 1;
    int y1 = pos.y + static_cast<int>(img->height) - 1;

    if (x1 < 0 || y1 < 0) return;
    if (x0 >= static_cast<int>(s_info.width) || y0 >= static_cast<int>(s_info.height)) return;

    int src_x0 = 0;
    int src_y0 = 0;
    if (x0 < 0) { src_x0 = -x0; x0 = 0; }
    if (y0 < 0) { src_y0 = -y0; y0 = 0; }

    if (x1 >= static_cast<int>(s_info.width)) {
        x1 = static_cast<int>(s_info.width) - 1;
    }
    if (y1 >= static_cast<int>(s_info.height)) {
        y1 = static_cast<int>(s_info.height) - 1;
    }

    uint32_t copy_w = static_cast<uint32_t>(x1 - x0 + 1);
    uint32_t copy_h = static_cast<uint32_t>(y1 - y0 + 1);
    if (copy_w == 0 || copy_h == 0) return;

    const uint8_t* src_base = static_cast<const uint8_t*>(img->pixels);
    const uint32_t bytes_per_pixel = 4;
    const uint64_t src_row_bytes = static_cast<uint64_t>(img->stride_bytes);
    const uint64_t dst_pitch_bytes = static_cast<uint64_t>(s_info.pitch_pixels) * bytes_per_pixel;

    void* target_buffer = (s_info.backbuffer != nullptr) ? s_info.backbuffer : reinterpret_cast<void*>(s_info.fb_vaddr);
    uint8_t* dst_base = static_cast<uint8_t*>(target_buffer);
    uint8_t* dst = dst_base + static_cast<uint64_t>(y0) * dst_pitch_bytes + static_cast<uint64_t>(x0) * bytes_per_pixel;
    const uint8_t* src = src_base + static_cast<uint64_t>(src_y0) * src_row_bytes + static_cast<uint64_t>(src_x0) * bytes_per_pixel;

    const uint64_t row_copy_bytes = static_cast<uint64_t>(copy_w) * bytes_per_pixel;
    for (uint32_t row = 0; row < copy_h; ++row) {
        ksystemramcpy(const_cast<uint8_t*>(src), dst, row_copy_bytes);
        src += src_row_bytes;
        dst += dst_pitch_bytes;
    }
}

KURD_t GfxPrim::enable_ram_buffer() {
    KURD_t kurd = KURD_t();
    s_info.backbuffer = __wrapped_pgs_valloc(&kurd, align_up(s_info.fb_bytes, 4096) / 4096, page_state_t::kernel_pinned, 21);
    return kurd;
}

void GfxPrim::Flush()
{
    // 如果没有 backbuffer，不需要 flush（直接写入硬件）
    if (!s_ready || s_info.fb_vaddr == 0 || s_info.backbuffer == nullptr) return;
    ksystemramcpy(s_info.backbuffer, reinterpret_cast<void*>(s_info.fb_vaddr), s_info.fb_bytes);
    asm volatile("sfence" ::: "memory");
}

void GfxPrim::FlushRect(Vec2i pos, Vec2i size)
{
    // 如果没有 backbuffer，不需要 flush（直接写入硬件）
    if (!s_ready || s_info.fb_vaddr == 0 || s_info.backbuffer == nullptr) return;
    if (size.x <= 0 || size.y <= 0) return;

    int x0 = pos.x;
    int y0 = pos.y;
    int x1 = pos.x + size.x - 1;
    int y1 = pos.y + size.y - 1;

    if (x1 < 0 || y1 < 0) return;
    if (x0 >= static_cast<int>(s_info.width) || y0 >= static_cast<int>(s_info.height)) return;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= static_cast<int>(s_info.width)) x1 = static_cast<int>(s_info.width) - 1;
    if (y1 >= static_cast<int>(s_info.height)) y1 = static_cast<int>(s_info.height) - 1;

    const uint32_t copy_w = static_cast<uint32_t>(x1 - x0 + 1);
    const uint32_t copy_h = static_cast<uint32_t>(y1 - y0 + 1);
    if (copy_w == 0 || copy_h == 0) return;

    const uint32_t bytes_per_pixel = 4;
    const uint64_t row_bytes = static_cast<uint64_t>(copy_w) * bytes_per_pixel;
    const uint64_t pitch_bytes = static_cast<uint64_t>(s_info.pitch_pixels) * bytes_per_pixel;

    uint8_t* src = static_cast<uint8_t*>(s_info.backbuffer)
        + static_cast<uint64_t>(y0) * pitch_bytes
        + static_cast<uint64_t>(x0) * bytes_per_pixel;
    uint8_t* dst = reinterpret_cast<uint8_t*>(s_info.fb_vaddr)
        + static_cast<uint64_t>(y0) * pitch_bytes
        + static_cast<uint64_t>(x0) * bytes_per_pixel;

    for (uint32_t row = 0; row < copy_h; ++row) {
        ksystemramcpy(src, dst, row_bytes);
        src += pitch_bytes;
        dst += pitch_bytes;
    }
    asm volatile("sfence" ::: "memory");
}

bool GfxPrim::Ready()
    
{
    return s_ready;
}

const GfxPrim::Info GfxPrim::GetInfo()
{
    return s_info;
}

void* GfxPrim::BackBuffer()
{
    return s_info.backbuffer;
}
