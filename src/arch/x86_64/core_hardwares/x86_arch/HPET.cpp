#include "arch/x86_64/core_hardwares/HPET.h"
#include "memory/all_pages_arr.h"
#include "ktime.h"
#include "memory/AddresSpace.h"
#include "util/OS_utils.h"
#include "util/kout.h"

HPET_driver* readonly_timer = nullptr;



/* ── KURD 模板 ───────────────────────────────────────────────────── */
KURD_t HPET_driver::default_kurd() {
    return KURD_t(0, 0, module_code::DEVICES_CORE,
                  COREHARDWARES_LOCATIONS::LOCATION_CODE_HPET,
                  0, 0, err_domain::CORE_MODULE);
}
KURD_t HPET_driver::default_success() {
    KURD_t k = default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    return k;
}

/* ═══════════════════════════════════════════════════════════════════
 * Init — HPET 初始化
 *
 * 时序（IA-PC HPET Spec 1.0a §3.2.6 建议 + 补充）：
 *   1. 读取 GCAP_ID，校验 counter_clk_period 非零
 *   2. 清零主计数器（ENABLE_CNF=0 时写 MAIN_CNT）
 *   3. 使能主计数器（ENABLE_CNF=1）
 *   4. 可选：禁用所有定时器中断（TN_INT_ENB_CNF=0）
 *
 * 注意：必须在 ENABLE_CNF=0 时写入 MAIN_CNT，否则行为未定义。
 * ═══════════════════════════════════════════════════════════════════ */
KURD_t HPET_driver::Init(vm_interval* entry) {
    using namespace COREHARDWARES_LOCATIONS::HPET_DRIVER_EVENTS::INIT_RESULTS::FAIL_REASONS;
    KURD_t fail = default_kurd();
    fail.event_code = COREHARDWARES_LOCATIONS::HPET_DRIVER_EVENTS::INIT;
    fail.result     = result_code::FAIL;
    fail.level      = level_code::ERROR;

    if (!entry || !entry->vbase()) {
        bsp_kout << now << "[HPET] Init: invalid entry" << kendl;
        fail.reason = INVALID_ACPI_ADDR;
        return fail;
    }

    regs = reinterpret_cast<HPET::reg_layout::head_regs_t*>(entry->vbase());

    // ── 1. 读取 GCAP_ID ───────────────────────────────────────────────
    HPET::reg_layout::GCAP_ID gcap;
    gcap.raw = regs->capabilities_id;

    hpet_timer_period_fs = gcap.counter_clk_period;
    if (hpet_timer_period_fs == 0) {
        bsp_kout << now << "[HPET] counter_clk_period=0, unusable" << kendl;
        fail.reason = COUNTER_PERIOD_ZERO;
        return fail;
    }


    bsp_kout << now << "[HPET] rev=0x" << (uint32_t)gcap.rev_id
             << " timers=" << (uint32_t)(gcap.num_tim_cap + 1)
             << " counter=" << (gcap.count_size_cap ? "64-bit" : "32-bit")
             << " vendor=0x" << (uint32_t)gcap.vendor_id
             << " period=" << (uint64_t)hpet_timer_period_fs << " fs"
             << kendl;

    // ── 2. 确保 ENABLE_CNF=0（计数器停止状态），清零主计数器 ──────────
    HPET::reg_layout::GEN_CONFIG cfg;
    cfg.raw = regs->general_config;
    cfg.enable_cnf = 0;
    regs->general_config = cfg.raw;   // 停止计数器
    asm volatile("mfence" ::: "memory");

    regs->main_counter_value = 0;     // 清零（Spec §2.3.7: 仅当计数器停止时写入）
    asm volatile("mfence" ::: "memory");

    // ── 3. 使能主计数器 ──────────────────────────────────────────────
    cfg.enable_cnf = 1;
    regs->general_config = cfg.raw;
    asm volatile("mfence" ::: "memory");

    // ── 4. 禁用所有定时器的中断（仅计数，不产生中断） ────────────────
    uint32_t timer_count = gcap.num_tim_cap + 1;
    if (timer_count >= 1) {
        HPET::reg_layout::TIMER_CONFIG_CAP tc;
        tc.raw = regs->timer0_config_cap;
        tc.tn_int_enb_cnf = 0;
        tc.tn_type_cnf    = 0;    // one-shot
        regs->timer0_config_cap = tc.raw;
    }
    if (timer_count >= 2) {
        HPET::reg_layout::TIMER_CONFIG_CAP tc;
        tc.raw = regs->timer1_config_cap;
        tc.tn_int_enb_cnf = 0;
        tc.tn_type_cnf    = 0;
        regs->timer1_config_cap = tc.raw;
    }
    if (timer_count >= 3) {
        HPET::reg_layout::TIMER_CONFIG_CAP tc;
        tc.raw = regs->timer2_config_cap;
        tc.tn_int_enb_cnf = 0;
        tc.tn_type_cnf    = 0;
        regs->timer2_config_cap = tc.raw;
    }

    bsp_kout << now << "[HPET] Init done, main counter running" << kendl;
    return default_success();
}

/* ═══════════════════════════════════════════════════════════════════
 * get_time_stamp_in_us — 返回自使能以来的微秒数
 *
 *   微秒数 = 主计数器值 × counter_clk_period(fs) / 10^9
 *
 * 使用 __uint128_t 避免 64-bit 乘法溢出。
 * 主计数器为 64-bit，周期为 32-bit，乘积 < 2^64 × 2^32 = 2^96，
 * 128-bit 运算足够。
 * ═══════════════════════════════════════════════════════════════════ */
uint64_t HPET_driver::get_time_stamp_in_us() {
    if (!regs) return 0;

    uint64_t count = regs->main_counter_value;
    __uint128_t fs = __uint128_t(count) * hpet_timer_period_fs;
    return uint64_t(fs / FS_per_mius);
}

/* ═══════════════════════════════════════════════════════════════════
 * dump_regs — 打印关键寄存器值（调试用）
 * ═══════════════════════════════════════════════════════════════════ */
void HPET_driver::dump_regs() {
    if (!regs) { bsp_kout << "[HPET] regs=nullptr" << kendl; return; }

    HPET::reg_layout::GCAP_ID gcap;
    gcap.raw = regs->capabilities_id;
    HPET::reg_layout::GEN_CONFIG cfg;
    cfg.raw = regs->general_config;

    bsp_kout << "[HPET] GCAP_ID  = 0x" << (uint64_t)gcap.raw
             << " (period=" << (uint64_t)gcap.counter_clk_period << "fs)" << kendl;
    bsp_kout << "[HPET] CFG      = 0x" << (uint64_t)cfg.raw
             << " (enable=" << (uint32_t)cfg.enable_cnf << ")" << kendl;
    bsp_kout << "[HPET] MAIN_CNT = 0x" << (uint64_t)regs->main_counter_value
             << " (~" << get_time_stamp_in_us() << " us)" << kendl;
}
