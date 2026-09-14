// ════════════════════════════════════════════════════════════════
// hpet_early.cpp（init.elf 摸黑时基）—— 参考 kernel 侧 HPET_driver / time.cpp
//
// 为什么能在 phase1 干：_init_entry 已载入 root_table（全物理恒等映射），
// 任何物理地址（ACPI 表、HPET MMIO）都可当 VA 直接读；再用 init_gop 的
// modify_access 把 HPET 页改成 UC（恒等映射下 VA==PA）。
//
// 失败一律【静默】（无 HPET / period==0）：now_ts_us() 返回 0。
// ════════════════════════════════════════════════════════════════
#include "init/util/hpet_early.h"

#include <efi.h>
#include "firmware/gSTResloveAPIs.h"          // RSDP_struct / XSDT_Table / ACPI_Table_Header / HPET_SIGNATURE_UINT32
#include "arch/x86_64/core_hardwares/HPET.h"  // HPET::ACPItb::HPET_Table / reg_layout
#include "arch/x86_64/init/page_table.h"      // modify_access
#include "memory/memory_base.h"               // phymem_segment / pgaccess / KSPACE_RW_UC_ACCESS

namespace
{

constexpr uint64_t FS_PER_US = 1000000000ull;   // 1 µs = 1e9 fs

HPET::reg_layout::head_regs_t* g_hpet_regs = nullptr;
uint64_t                       g_hpet_period_fs = 0;

// 摸黑爬 ACPI：gST → ACPI20 GUID → RSDP → XSDT → "HPET" 表 → Base_Address
void* acpi_find_hpet_base(BootInfoHeader* header)
{
    if (!header || !header->gST_ptr) return nullptr;

    auto* st = (EFI_SYSTEM_TABLE*)header->gST_ptr;
    auto* ct = st->ConfigurationTable;
    UINTN n  = st->NumberOfTableEntries;

    const EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    const uint8_t* g1 = (const uint8_t*)&acpi20;

    uint64_t xsdt_pa = 0;
    for (UINTN i = 0; i < n; ++i)
    {
        const uint8_t* g2 = (const uint8_t*)&ct[i].VendorGuid;
        bool match = true;
        for (size_t b = 0; b < sizeof(EFI_GUID); ++b)
            if (g1[b] != g2[b]) { match = false; break; }
        if (match)
        {
            auto* rsdp = (RSDP_struct*)ct[i].VendorTable;
            if (rsdp && rsdp->Revision >= 2) { xsdt_pa = rsdp->XsdtAddress; break; }
        }
    }
    if (!xsdt_pa) return nullptr;

    auto* xsdt = (const XSDT_Table*)(uintptr_t)xsdt_pa;
    uint64_t ec = (xsdt->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
    for (uint64_t i = 0; i < ec; ++i)
    {
        auto* hdr = (const ACPI_Table_Header*)(uintptr_t)xsdt->Entry[i];
        if (!hdr) continue;
        if (*(const uint32_t*)hdr->Signature == HPET_SIGNATURE_UINT32)
            return (void*)(uintptr_t)((const HPET::ACPItb::HPET_Table*)hdr)->Base_Address;
    }
    return nullptr;
}

}  // namespace

void init_hpet_early(BootInfoHeader* header)
{
    void* base = acpi_find_hpet_base(header);
    if (!base) return;                                   // 静默失败：没有 HPET

    // MMIO 改 UC（恒等映射下 VA==PA；失败也继续，QEMU/TCG 下 WB 可跑）
    phymem_segment seg{ .start = (uint64_t)base, .size = 0x1000 };
    (void)modify_access(seg, KSPACE_RW_UC_ACCESS);

    auto* regs = (HPET::reg_layout::head_regs_t*)base;

    // 1) GCAP_ID：取 counter_clk_period（fs/tick）
    HPET::reg_layout::GCAP_ID gcap;
    gcap.raw = regs->capabilities_id;
    uint64_t period = gcap.counter_clk_period;
    if (period == 0) return;                             // 静默失败：不可用

    // 2) 停计数 → 清零主计数器 → 启计数（Spec §2.3.7：仅停机时可写 MAIN_CNT）
    HPET::reg_layout::GEN_CONFIG cfg;
    cfg.raw = regs->general_config;
    cfg.enable_cnf = 0;
    regs->general_config = cfg.raw;
    asm volatile("mfence" ::: "memory");

    regs->main_counter_value = 0;
    asm volatile("mfence" ::: "memory");

    cfg.enable_cnf = 1;
    regs->general_config = cfg.raw;
    asm volatile("mfence" ::: "memory");

    // 3) 关 timer0..2 中断（只计数）。head_regs_t 是 packed，不能取字段引用 —— 用宏原地读改写。
    uint32_t timer_count = (uint32_t)gcap.num_tim_cap + 1;
#define HPET_DISABLE_TIMER(REG)                        \
    do {                                               \
        HPET::reg_layout::TIMER_CONFIG_CAP _tc;        \
        _tc.raw = (REG);                               \
        _tc.tn_int_enb_cnf = 0;                        \
        _tc.tn_type_cnf    = 0;                        \
        (REG) = _tc.raw;                               \
    } while (0)
    if (timer_count >= 1) HPET_DISABLE_TIMER(regs->timer0_config_cap);
    if (timer_count >= 2) HPET_DISABLE_TIMER(regs->timer1_config_cap);
    if (timer_count >= 3) HPET_DISABLE_TIMER(regs->timer2_config_cap);
#undef HPET_DISABLE_TIMER

    g_hpet_period_fs = period;
    g_hpet_regs      = regs;
}

// ── 时基接口（与 util/printk.h / kcirclebufflogMgr.h 同一约定：微秒）──
// 不用 __uint128_t 除法（会引 __udivti3 / libgcc）：拆成 64 位运算。
//   us = (count / 1e9) * period + (count % 1e9) * period / 1e9
// 第二项 < 1e9 * period ≤ 1e9 * 2^32 < 2^63 安全；实际 period ~7e7（~14.3MHz）时首项亦远不溢出。
extern "C" uint64_t now_ts_us()
{
    if (!g_hpet_regs) return 0;
    uint64_t count = g_hpet_regs->main_counter_value;
    uint64_t q = count / FS_PER_US;
    uint64_t r = count % FS_PER_US;
    return q * g_hpet_period_fs + (r * g_hpet_period_fs) / FS_PER_US;
}
