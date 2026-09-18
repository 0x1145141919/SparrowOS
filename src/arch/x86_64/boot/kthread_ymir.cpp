// ════════════════════════════════════════════════════════════════
// kthread_ymir.cpp — 内核线程初始化（始祖线程 YMIR 及其派生线程）
//
// 从 kinit.cpp 拆出（2026-09-18）：kinit 只留「内存成熟 → 跳入线程运行时」的
// 引导胶水；凡由 kthread_ymir 派生/管理的线程初始化集中于此。
// 接口与符号声明见 src/include/boot/kthread_ymir.h。
//
// 2026-09-18：本文件恢复为「仅业务初始化」——原 MMU/WRAITH 压测分支已摘除
// （代码在 git 历史里兜底；设计/实测记录见 Docs/Memory/MMU压测落地设计.md）。
// ════════════════════════════════════════════════════════════════
#include "boot/kthread_ymir.h"

#include "abi/os_error_definitions.h"
#include "util/kout.h"                                      // bsp_kout / kendl
#include "util/kshell.h"                                    // kshell_framework_t
#include "arch/x86_64/core_hardwares/i8042.h"               // i8042_char_subscriber_init
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"   // nvme_parallel_init_all
#include "Scheduler/kthread_abi.h"                          // creat_kthread / kthread_creating_package / kthread_sleep
#include "boot/boot_cfg.h"                                   // g_boot_cfg（ymir 世界选择）

// ── 运行模式开关 ──────────────────────────────────────────────
// 是否派生 BQ 超时扫描线程（“兜底计时器”）。
// （世界选择已上收为启动期 boot_cfg.ymir，见 boot/boot_cfg.h。）
bool if_bq_sweeper = true;

// ────────────────────────────────────────────────────────────────
// 始祖线程
// ────────────────────────────────────────────────────────────────

// 派生 BQ 超时扫描线程。
static void spawn_bq_sweeper() {
    kthread_creating_package pkg = {};
    pkg.func_raw   = (uint64_t)bq_timeout_sweeper;
    pkg.args[0]    = (uint64_t)nullptr;
    pkg.launch_pid = 0;
    KURD_t kurd2{};
    uint64_t tid = creat_kthread(&pkg, &kurd2);
    if (error_kurd(kurd2)) {
        bsp_kout << "[BQ] sweeper thread spawn failed" << kendl;
    } else {
        bsp_kout << "[BQ] sweeper thread tid=" << tid << kendl;
    }
}

void*kthread_ymir(void*null){//所有内核线程的始祖之"尤米尔线程"（出自进击的巨人）
    (void)null;
    KURD_t kurd = KURD_t();

    if (g_boot_cfg.ymir == ymir_mode_t::NORMAL) {
        // ── 业务初始化 ──
        if (if_bq_sweeper) spawn_bq_sweeper();
        i8042_char_subscriber_init();
        //pcie_text_praser();
        // 并行初始化所有 NVMe 控制器（每控制器一线程，共享 u64 汇报画板，≤5s 轮询提前退出）
        nvme_parallel_init_all();
        //text_input_subscriber_init();

        // 初始化 kshell 框架
        kurd = kshell_framework_t::Init();
        if (error_kurd(kurd)) {
            bsp_kout << "[KSHELL] Failed to initialize framework!" << kendl;
        } else {
            bsp_kout << "[KSHELL] Framework initialized, ready for commands" << kendl;
        }
    } else {
        // 测试分支（TEST_MMU / TEST_WRAITH）：代码由 git 历史兜底（见 kthread_ymir.h），
        // 当前仅跳过业务初始化，作为后续测试场景的落点。
        bsp_kout << "[YMIR] mode=" << (uint32_t)g_boot_cfg.ymir
                 << " (test branch reserved; business init skipped)" << kendl;
    }

    while (true)
    {
        kthread_sleep(1000000);
    }

    return nullptr;
}
