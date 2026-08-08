#include "boot/exec_env_prepare.h"
#include "boot/info_pkg_link.h"
#include "boot/asset_table.h"
#include "memory/kpoolmemmgr.h"
#include "memory/FreePagesAllocator.h"
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

// BCB 交接（一等字段复制目标；声明在 arch/x86_64/mem_init.h，收养路径消费）
bcb_desc_v2_t* g_bcbs = nullptr;
uint64_t g_bcbs_count = 0;
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

    // 一等字段：bcb_table 复制到堆（收养路径消费，焚包后仍有效）
    g_bcbs_count = an->bcbs_count;
    if (g_bcbs_count) {
        g_bcbs = new bcb_desc_v2_t[g_bcbs_count];
        ksystemramcpy(an->bcb_table, g_bcbs,
                      g_bcbs_count * sizeof(bcb_desc_v2_t));
    }

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

    // BCB 继承：把 init.elf 穿越的 BCB 生态（g_bcbs）收养给 FPA，全部置幼年态。
    // 收养后幼年态供 pages_arr 等全局大数组顺序分配；大数组分配完再由
    // basic_init 调 Adopt_all_adult 全量催熟。
    {
        FreePagesAllocator::inherit_bcbs_config cfg = {};
        cfg.descs                   = g_bcbs;
        cfg.count                   = g_bcbs_count;
        cfg.logical_processor_count = logical_processor_count;
        KURD_t ihk = FreePagesAllocator::Inherit_bcbs(&cfg);
        if (error_kurd(ihk)) {
            bsp_kout << "[exec_env_prepare] Inherit_bcbs failed" << kendl;
            boot_halt(SRC_LOC());
        }
        bsp_kout << "[exec_env_prepare] Inherit_bcbs: " << g_bcbs_count
                 << " BCBs adopted (all juvenile)" << kendl;
    }
}
// 早期失败停机：exec_env_prepare 阶段无 kout，只能裸停机
static void boot_halt(loc_code_t loc) {
    (void)loc;
    asm volatile("cli; hlt");
    for (;;) asm volatile("hlt");
}
