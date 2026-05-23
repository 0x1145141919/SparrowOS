#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"

// ── asm 原语所需的数据类型（与 runtime_processor_regist.asm 一致） ──
struct load_resources_struct {
    GDTR* gdtr;
    IDTR* idtr;
    uint64_t K_CS_selector;
    uint64_t K_DS_selector;
    uint64_t TSS_selector;
};

// 低层 asm 原语：LGDT + 远跳转重载 CS + 数据段 + LTR
extern "C" void runtime_processor_regist(load_resources_struct* resources);

// ============================================================================
// gs_complex_load_gdt_tss — 无条件加载 GS 复合体的 GDT/TSS
// ============================================================================
//
// 前置条件：gs_complex_prepare_all 已完成所有描述符填充。
// 本函数仅构造 GDTR 并委托 asm 原语执行：
//   1. LGDT（GDT 已包含 GDT 条目 + TSS 描述符）
//   2. 远跳转重载 CS
//   3. 重载 DS/ES/FS/GS/SS
//   4. LTR
//
extern "C" void gs_complex_load_gdt_tss(gs_complex_t* complex) {
    // ── 构造 GDTR ────────────────────────────────────────────────────────
    // GDT 区域覆盖 gdt[6] + tss_descriptor，连续位于 gs_complex_t 内
    GDTR gdtr;
    gdtr.base = reinterpret_cast<uint64_t>(&complex->gdt);
    gdtr.limit = static_cast<uint16_t>(
        offsetof(gs_complex_t, tss_descriptor) + sizeof(TSSDescriptorEntry)
        - offsetof(gs_complex_t, gdt) - 1);

    // ── 构造加载资源包 ────────────────────────────────────────────────────
    load_resources_struct resources;
    resources.gdtr            = &gdtr;
    resources.idtr            = nullptr;
    resources.K_CS_selector   = K_cs_idx << 3;   // 0x08
    resources.K_DS_selector   = K_ds_ss_idx << 3; // 0x10
    resources.TSS_selector    = gdt_headcount << 3; // 0x30

    // ── 委托 asm 原语 ─────────────────────────────────────────────────────
    runtime_processor_regist(&resources);
}
