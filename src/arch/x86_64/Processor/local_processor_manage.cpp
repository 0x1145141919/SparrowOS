#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kout.h"
#include "arch/x86_64/Interrupt_system/AP_Init_error_observing_protocol.h"


/* ── x2apic_core_init ──────────────────────────────────────────────
 *
 * 每个逻辑处理器在进入长模式后调用一次，启用 x2APIC 模式并返回本处理器的
 * x2APIC ID。
 *
 * - BSP 已由 UEFI 固件启用 x2APIC，此函数可安全反复调用（检测已启用则跳过）
 * - AP 在 ap_init 中被调用，启用 x2APIC 后可通过 IA32_X2APIC_ICR 收发 IPI
 * - CR8 TPR 统一设为 1，允许接收优先级 ≥ 1 的中断
 *
 * 返回值：本处理器的 x2APIC ID（uint32_t），用作 g_gs_by_apicid 查表键。
 *
 * 移植自 master 版 x64_local_processor::x64_local_processor() 的 x2APIC
 * 初始化时序。
 *
 * 注意：x2APIC ID 在 CPUID leaf 0xB EDX 中始终可读，不依赖 x2APIC 模式状态。
 * 此处先在 MSR 层面确保硬件模式正确，再返回 ID。
 */
extern "C" x2apicid_t x2apic_core_init()
{
    // ── 硬件能力检查 ────────────────────────────────────────────────
    if (!is_x2apic_supported()) {
        bsp_kout << now << "[x2apic_core_init] x2APIC not supported on this processor"
                 << kendl;
        return 0;
    }

    uint64_t apic_base = rdmsr(msr::apic::IA32_APIC_BASE);

        // x2APIC 尚未启用（AP 路径）→ 启用
        apic_base |= (1ULL << 11);   // APIC 全局使能
        apic_base |= (1ULL << 10);   // x2APIC 模式
        wrmsr_func(msr::apic::IA32_APIC_BASE, apic_base);

        // Spurious Vector Register — 只在首次启用时写入
        // 向量 0xFF, 位 8 = APIC software enable
        wrmsr_func(msr::apic::IA32_X2APIC_SVR, (uint64_t)0x1FF);
    

    // ── TPR: 允许优先级 ≥1 的中断 ───────────────────────────────────
    uint64_t tpr = 1;
    asm volatile("mov %0, %%cr8" : : "r"(tpr) : "memory");

    /* 返回 x2APIC ID
     *
     * 在 x2APIC 模式下也可以读 IA32_X2APIC_ID (MSR 0x802)，
     * 但 query_x2apicid() 用 CPUID 0xB，不依赖 MSR，且已在多处使用。 */
    return query_x2apicid();
}
check_point longmode_enter_checkpoint;
extern "C" uint64_t ap_bootstrap_init()
{
    // ── 1. x2APIC 启用 + 获取 x2APIC ID ──────────────────────────────
    x2apicid_t apicid = x2apic_core_init();

    // ── 2. 查表获取本核 GS 复合体 ─────────────────────────────────────
    gs_complex_t* self = g_gs_by_apicid[apicid];
    if (!self) {
        // 映射表条目为 nullptr → 表太小或 APIC ID 超出范围
        return 0;
    }

    // ── 3. GS_BASE → 真实 gs_complex_t（Phase 4.5 已填好所有内容） ──
    wrmsr_func(msr::syscall::IA32_GS_BASE, (uint64_t)self);
    wrmsr_func(msr::syscall::IA32_KERNEL_GS_BASE, (uint64_t)self);

    // ── 4. 加载 GDT + TSS（GDT 条目、TSS 描述符、TSS 栈指针均已就绪） ─
    gs_complex_load_gdt_tss(self);

    // ── 5. Checkpoint: longmode enter ─────────────────────────────────
    longmode_enter_checkpoint.success_word = ~self->slots[PROCESSOR_ID_GS_INDEX];
    asm volatile("sfence");

    // ── 6. 返回 hdstacks 中本核的 rsp0 栈底 ──────────────────────────
    return self->tss.rsp0;
} 