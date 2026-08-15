#include "boot/exec_env_prepare.h"
#include "boot/info_pkg_link.h"
#include "boot/asset_table.h"
#include "memory/kpoolmemmgr.h"
#include "memory/page_frame_state_mgr.h"
#include "exec_env_detect.h"
#include "panic.h"
#include "arch/x86_64/mem_init.h"
#include "kcirclebufflogMgr.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "util/textConsole.h"
#include "util/kout.h"
#include "util/OS_utils.h"

// ════════════════════════════════════════════════════════════════
// exec_env_prepare 实现
//
// 特殊约束（本文件独享，见 boot/exec_env_prepare.h）：
//   无 kout / 无输出子系统，失败一律 boot_halt 裸停机。
// ════════════════════════════════════════════════════════════════

static void boot_halt(loc_code_t loc);


void exec_env_prepare(init_to_kernel_header_v2* pkg)
{
    g_env = probe_env();
    GlobalKernelStatus = kernel_state::EARLY_BOOT;
    kpoolmemmgr_t::Init();

    // 信息包链接：偏移式 → 指针式 analyzed 视图（原地链接，pkg 被改写为指针）
    init_to_kernel_header_analyzed* an = link_init_to_kernel_header(pkg);
    if (!an)
        boot_halt(SRC_LOC());

    // 一等字段：phymem_segments 复制到堆（焚包后仍有效）
    logical_processor_count = an->logical_processor_count;
    phymem_segments_count = an->phymem_segment_count;
    if (phymem_segments_count) {
        phymem_segments = new phymem_segment[phymem_segments_count];
        ksystemramcpy(an->phymem_segments, phymem_segments,
                      phymem_segments_count * sizeof(phymem_segment));
    }

    // 一等字段：free_segs_descriptors_table 由 page_frame_state_mgr::adopt 拷贝
    //           （索引式，无需堆中转；adopt 内部已解析进 intervals[]）

    // 资产表：create + pour（全量深拷贝 properties，焚包后仍有效）
    g_asset_table = asset_table_t::create();
    if (!g_asset_table || g_asset_table->pour(an) != 0)
        boot_halt(SRC_LOC());

    // read/deal 三个必需资产：log_buffer / gop_framebuffer / gop_info
    // 模式：read 直读 → 调用方自拷 → deal 标记（deal 后 entry 悬垂）
    {
        const asset_table_entry* e = g_asset_table->read("log_buffer mem");
        if (!e) boot_halt(SRC_LOC());
        vm_interval log_iv = *(vm_interval*)e->data;
        g_asset_table->deal("log_buffer mem");
        DmesgRingBuffer::Init(&log_iv);
    }
    {
        const asset_table_entry* e = g_asset_table->read("gop_framebuffer mem");
        if (!e) boot_halt(SRC_LOC());
        vm_interval gop_fb = *(vm_interval*)e->data;
        g_asset_table->deal("gop_framebuffer mem");
        GlobalBasicGraphicInfoType gop_info = {};
        e = g_asset_table->read("gop_info gop");
        if (!e) boot_halt(SRC_LOC());
        gop_info = *(GlobalBasicGraphicInfoType*)e->data;
        g_asset_table->deal("gop_info gop");
        if (error_kurd(GfxPrim::Init(&gop_info, gop_fb)))
            boot_halt(SRC_LOC());
    }

    // 输出子系统链路：GfxPrim 就绪 → textconsole → serial → kout
    {
        Vec2i font_vec = {.x = 16, .y = 32};
        if (error_kurd(textconsole_GoP::Init(&ter16x32_data[0][0][0],
                                             font_vec, 0x00ffffffff, 0)))
            boot_halt(SRC_LOC());
        textconsole_GoP::Clear();
        serial_init_stage1();
        bsp_kout.Init();
        bsp_kout.shift_dec();
    }

    // 页框状态管理器收养：接管 init 穿越的 pages_arr 账本（mem_map）+ free_segs
    // 描述符。收养后本模块即持有权威物理页账本（state_set / state_query /
    // kind_check / idx_base_* / early_alloc 可用）；FPA 后续重建基于它
    // （intervals_snapshot 全量区间，自行分桶折叠），不再有 BCB 位图交接。
    {
        const asset_table_entry* e = g_asset_table->read("pages_arr mem");
        if (!e) boot_halt(SRC_LOC());
        vm_interval pages_arr_iv = *(vm_interval*)e->data;
        g_asset_table->deal("pages_arr mem");
        if (page_frame_state_mgr::adopt(&pages_arr_iv,
                                        an->free_segs_descriptors_table,
                                        an->free_segs_count) != 0)
            boot_halt(SRC_LOC());
        bsp_kout << "[exec_env_prepare] page_frame_state_mgr adopted: "
                 << an->free_segs_count << " free_segs descriptors" << kendl;
    }
}
// 早期失败停机：exec_env_prepare 阶段无 kout，只能裸停机
static void boot_halt(loc_code_t loc) {
    (void)loc;
    asm volatile("cli; hlt");
    for (;;) asm volatile("hlt");
}
