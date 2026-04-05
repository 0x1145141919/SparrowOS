#pragma once
#include "stdint.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "util/kout.h"
#include "util/lock.h"
class task;
struct TextViewport {
    Vec2i pos;      // 文字区域左上角（像素）
    Vec2i size;     // 文字区域大小（像素）
    Vec2i cell;     // 字符宽高（像素）
    int cols;       // size.x / cell.x
    int rows;       // size.y / cell.y
};
namespace infrastructure_location_code{ //bitmap和Ktemplate系列不能也不适合进入这个位置系统，数据结构必须实例化才有意义
    constexpr uint8_t location_code_textconsole_GoP=4;
    namespace textconsole_GoP_events{
        constexpr uint8_t textconsole_GoP_event_init=0;
        namespace init_results{
            namespace fail_reasons{
                constexpr uint8_t font_bitmap_is_null=0;
                constexpr uint8_t gfx_not_ready=1;
                constexpr uint8_t bad_cell_size=2;
                constexpr uint8_t zero_grid=3;
                constexpr uint8_t already_init=4;
                constexpr uint8_t glyph_cache_alloc_fail=5;
                constexpr uint8_t backend_register_fail=6;
            }
        }
    }
}
enum class tc_msg_type : uint8_t { string,single_character,num, flush };
enum tc_msg_flags : uint16_t {
    tc_msg_flag_none = 0,
    tc_msg_flag_urgent = 1u << 0,
};
struct tc_msg_frame_head{
    tc_msg_type type;
    uint8_t reserved;
    uint16_t flags; 
    uint32_t producer_cpu;
    uint64_t seq;
};
constexpr uint32_t TC_RING_CAP = 1024;
constexpr uint32_t TC_SERVICE_POP_BATCH = 64;
struct tc_slot {
    tc_msg_frame_head head;
    union {
        struct { const char* string; uint64_t len; } s;
        struct { char ch; } c;
        struct { uint64_t num_raw; num_format_t format; numer_system_select radix; } n;
    } payload;
};
struct tc_ring {
    spintrylock_cpp_t lock;
    tc_slot slots[TC_RING_CAP];
    uint32_t head;
    uint32_t tail;
    uint64_t seq_gen;
    uint64_t drop_count;
    uint64_t push_count;
    uint64_t pop_count;
};
struct tc_service_local_batch {
    uint32_t count;
    tc_slot items[TC_SERVICE_POP_BATCH];
};
struct TextCursor {
    int m;          // 列
    int n;          // 行
};
class textconsole_GoP {
public:
    // 唯一入口
    static KURD_t Init(//屏幕分辨率大小查询GfxPrim，并且起始向量默认为（0,0），初始化后处于直接位图渲染模式
        const unsigned char* font_bitmap,
        Vec2i cell_size,
        uint32_t font_color,
        uint32_t background_color
    );
    static KURD_t enable_font_render();//在切换好地址空间以及内存相关子系统初始化好之后才可以进行，申请 glyph_cache 并预渲染字符，切换到缓存渲染模式
    static bool Ready();
    static void PutChar(char ch);
    static void PutString(const char* s,uint64_t len);
    static void Clear();
    static bool RuntimeInitServiceThread();
    static bool RuntimeSubmitString(const char* s, uint64_t len, bool urgent = false);
    static bool RuntimeSubmitChar(char ch, bool urgent = false);
    static bool RuntimeSubmitNum(uint64_t raw, num_format_t format, numer_system_select radix, bool urgent = false);
    static bool RuntimeSubmitFlush(bool urgent = true);

private:
    static void* RuntimeServiceThreadMain(void* data);
    static void RuntimeWakeServiceThread();
    static GfxImage template_character;
    static TextViewport view;
    static TextCursor cursor;
    static const unsigned char* font_bitmap;
    static uint32_t font_color;
    static uint32_t background_color;
    static void* glyph_cache;
    static uint64_t runtime_service_tid;
    static spintrylock_cpp_t runtime_service_create_lock;
    static tc_ring runtime_ring;
    static bool ready;
    static uint16_t glyph_index[256];
    static bool is_font_rendered; // true: 使用 glyph_cache 缓存渲染模式，false: 直接位图渲染模式
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();
};
