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
#include "util/kptrace.h"
#include "arch/x86_64/core_hardwares/HPET.h"

// ════════════════════════════════════════════════════════════════
// exec_env_prepare 实现
//
// 特殊约束（本文件独享，见 boot/exec_env_prepare.h）：
//   无 kout / 无输出子系统，失败一律 boot_halt 裸停机。
// ════════════════════════════════════════════════════════════════

static void boot_halt(loc_code_t loc);
static void init_panic_early_support(void);
static void init_output_subsystem(void);


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

    // 早期 panic 支撑：phyaddr_window / ksymmanager / HPET（先于输出子系统就绪，
    // 保证第一条可能崩溃即可调符号表 + 时间戳）
    init_panic_early_support();

    // 输出子系统初始化（资产 read/deal + 输出链路），统一收敛到独立函数
    init_output_subsystem();

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
// 早期 panic 支撑：phyaddr_window / ksymmanager / HPET
//   ① "phyaddr_window mem" → Kspace_phyaddr_access_window 全局落账（[0,dram_top)→高 VA
//      窗口）。它是二级重链的钥匙：包外纯物理资产（movable 等）经它把 phys 重链成内核
//      VA（见 Init_v3_string_tag_container.md §三.2），后续 PhyAddrAccessor 也依赖它。
//   ② "ksymbols movable" → movable_file_entry_t（纯物理描述符，init.elf 不做 KMMU 映射）。
//      应急：经 phyaddr_window 重链成 vm_interval 后 Init——panic 最早的符号表可得。
//   ③ "hpet_mmio mem" → readonly_timer（HPET）：kout 时间戳（now）与 ktime 依赖它。
// 本函数仍处摸黑阶段，失败一律 boot_halt 裸停机。
static void init_panic_early_support(void)
{
    // ① 二级重链基础：phyaddr_window 必须先于一切 phys→VA 换算落账
    {
        const asset_table_entry* e = g_asset_table->read("phyaddr_window mem");
        if (!e) boot_halt(SRC_LOC());
        Kspace_phyaddr_access_window = *(vm_interval*)e->data;
        g_asset_table->deal("phyaddr_window mem");
    }
    // ② ksymmanager：ksymbols 应急经 phyaddr_window 重链访问
    {
        const asset_table_entry* e = g_asset_table->read("ksymbols movable");
        if (!e) boot_halt(SRC_LOC());
        movable_file_entry_t sym = *(movable_file_entry_t*)e->data;
        g_asset_table->deal("ksymbols movable");

        phyaddr_t sym_pbase = sym.base_ppn << 12;
        uint64_t  sym_bytes = align_up(sym.size, 4096);
        vm_interval ksym_iv = {
            .vpn    = (Kspace_phyaddr_access_window.vbase() + sym_pbase) >> 12,
            .ppn    = sym.base_ppn,
            .npages = sym_bytes >> 12,
            .access = KSPACE_RW_ACCESS,
        };
        if (ksymmanager::Init(&ksym_iv, sym.size) != 0)
            boot_halt(SRC_LOC());
    }
    // ③ readonly_timer：HPET MMIO 已由 init.elf KMMU 映射（vm_interval 含真实 VA）
    {
        const asset_table_entry* e = g_asset_table->read("hpet_mmio mem");
        if (!e) boot_halt(SRC_LOC());
        vm_interval hpet_iv = *(vm_interval*)e->data;
        g_asset_table->deal("hpet_mmio mem");
        readonly_timer = new HPET_driver();
        if (error_kurd(readonly_timer->Init(&hpet_iv)))
            boot_halt(SRC_LOC());
    }
}

// 输出子系统初始化：
//   ① read/deal 三个必需资产：log_buffer / gop_framebuffer / gop_info
//     模式：read 直读 → 调用方自拷 → deal 标记（deal 后 entry 悬垂）
//   ② 输出链路：GfxPrim 就绪 → textconsole → serial → kout
// 本函数仍处摸黑阶段，失败一律 boot_halt 裸停机。
static void init_output_subsystem(void)
{
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
}

// 早期失败停机：exec_env_prepare 阶段无 kout，只能裸停机
static void boot_halt(loc_code_t loc) {
    (void)loc;
    asm volatile("cli; hlt");
    for (;;) asm volatile("hlt");
}
