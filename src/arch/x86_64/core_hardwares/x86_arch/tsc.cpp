#include "arch/x86_64/core_hardwares/tsc.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/textConsole.h"
#include "exec_env_detect.h"
#include "ktime.h"
#include "util/kout.h"
#include "init/util/OS_utils.h"
#include "abi/boot.h"

uint32_t tsc_fs_per_cycle;
bool is_tsc_ddline_avaliabe;
bool is_tsc_reliable;

extern vm_interval pages_arr;
extern vm_interval Kspace_phyaddr_access_window;

static void tsc_panic_hlt(void)
{
    bsp_kout << "[PANIC] TSC registration failed: system cannot continue\n";
    bsp_kout.shift_hex();
    bsp_kout << "  g_env:             " << (uint32_t)g_env << "\n";
    bsp_kout << "  is_tsc_reliable:   " << is_tsc_reliable << "\n";
    bsp_kout << "  is_tsc_deadline:   " << is_tsc_ddline_avaliabe << "\n";
    {   cpuid_tmp cpuid7(0x07, 0x00);
        bsp_kout << "  WAITPKG:           " << !!((cpuid7.ecx >> 5) & 1) << "\n";
    }
    bsp_kout.shift_dec();
    for (;;)
        asm volatile("hlt");
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val);
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/*
 * pvclock_vcpu_time_info 结构体偏移：
 *   +0  version   (u32)
 *   +4  pad0      (u32)
 *   +8  tsc_timestamp (u64)
 *   +16 system_time   (u64)
 *   +24 tsc_to_system_mul (u32)
 *   +28 tsc_shift    (s8)
 *   +29 flags       (u8)
 *   +30 pad[2]      (u8[2])
 */
#define PVCLOCK_OFFSET_TSC_TO_SYSTEM_MUL  24
#define PVCLOCK_OFFSET_TSC_SHIFT          28

static void kvm_calc_tsc_fs_per_cycle(void)
{
    cpuid_tmp q(0x40000001, 0);

    // KVM_FEATURE_CLOCKSOURCE2 = bit 3
    if (!(q.eax & (1 << 3)))
        tsc_panic_hlt();

    // 从 pages_arr 借一页物理内存做 pvclock 页
    phyaddr_t pvclock_pa = pages_arr.pbase();
    vaddr_t   pvclock_va = Kspace_phyaddr_access_window.vbase() + pvclock_pa;

    // 清零
    ksetmem_8((void *)pvclock_va, 0, 4096);

    // 注册 pvclock 页，MSR_KVM_SYSTEM_TIME_NEW = 0x4b564d01
    wrmsr(0x4b564d01, pvclock_pa | 1);

    /*
     * FIXME: 有些 KVM 版本可能在第一次 WRMSR 后需要一次 VM-exit 才能更新
     * pvclock 页。如果读出来的 mul=0，可能需要先触发一次 cpuid 或类似
     * 操作强制同步后再读。
     * 暂时直接读。
     */
    uint32_t tsc_to_system_mul;
    int8_t   tsc_shift;

    // 确保读到一致的数据：检查 version 奇偶
    volatile uint32_t *ver   = (volatile uint32_t *)pvclock_va;
    volatile uint32_t *mul_p = (volatile uint32_t *)(pvclock_va + PVCLOCK_OFFSET_TSC_TO_SYSTEM_MUL);
    volatile uint8_t  *sft_p = (volatile uint8_t  *)(pvclock_va + PVCLOCK_OFFSET_TSC_SHIFT);

    uint32_t version;
    do {
        version   = *ver;
        tsc_to_system_mul = *mul_p;
        tsc_shift = *(volatile int8_t *)sft_p;
        asm volatile("mfence");
    } while ((*ver & 1) || *ver != version);

    // 清零归还
    ksetmem_8((void *)pvclock_va, 0, 4096);

    if (!tsc_to_system_mul)
        tsc_panic_hlt();

    /*
     * pvclock_tsc_khz:
     *   pv_tsc_khz = (1000000ULL << 32) / tsc_to_system_mul
     *   if (tsc_shift < 0) pv_tsc_khz <<= -tsc_shift
     *   else               pv_tsc_khz >>=  tsc_shift
     */
    uint64_t pv_tsc_khz = (1000000ULL << 32) / tsc_to_system_mul;
    if (tsc_shift < 0)
        pv_tsc_khz <<= -tsc_shift;
    else
        pv_tsc_khz >>=  tsc_shift;

    uint64_t tsc_frequency = pv_tsc_khz * 1000;
    tsc_fs_per_cycle = ((__uint128_t)1000000 * FS_per_mius) / tsc_frequency;
}

void tsc_regist()
{
    time_complex *complex = new time_complex;
    gs_u64_write(TIME_COMPLEX_GS_INDEX, (uint64_t)complex);
    complex->lapic_fs_per_cycle = 0;

    // ── TCG: TSC 不靠谱，直接退出 ──
    if (g_env == ENV_TCG) {
        is_tsc_reliable       = false;
        is_tsc_ddline_avaliabe = false;
        tsc_fs_per_cycle       = 0;
        return;
    }

    // ── KVM / Bare metal 能力探测 ──
    cpuid_tmp querier(0x80000007, 0);
    is_tsc_reliable       = !!(querier.edx & (1 << 8));

    querier.update(1, 0);
    is_tsc_ddline_avaliabe = !!(querier.edx & (1 << 24));

    // 必须同时支持，否则停在这里
    if (!is_tsc_reliable || !is_tsc_ddline_avaliabe)
        tsc_panic_hlt();

    // ── 频率测算 ──
    if (g_env == ENV_KVM) {
        kvm_calc_tsc_fs_per_cycle();
    } else {
        // ENV_BARE_METAL
        querier.update(0x15, 0);
        if (querier.eax) {
            uint64_t tsc_frequency = (uint64_t)querier.ecx * querier.ebx / querier.eax;
            tsc_fs_per_cycle = ((__uint128_t)1000000 * FS_per_mius) / tsc_frequency;
        } else {
            // HPET 校准 fallback
            uint64_t tsc_stamp1 = rdtsc();
            ktime::microsecond_polling(10000);
            uint64_t tsc_stamp2 = rdtsc();
            tsc_fs_per_cycle = ((uint64_t)10000 * FS_per_mius) / (tsc_stamp2 - tsc_stamp1);
        }
    }

    // ── 打印 ──
    bsp_kout << "[INFO] TSC registration completed:\n";
    bsp_kout.shift_hex();
    bsp_kout << "  g_env:              " << (uint32_t)g_env << "\n";
    bsp_kout << "  is_tsc_reliable:    " << is_tsc_reliable << "\n";
    bsp_kout << "  is_tsc_deadline:    " << is_tsc_ddline_avaliabe << "\n";
    bsp_kout << "  tsc_fs_per_cycle:   " << tsc_fs_per_cycle << "\n";
    bsp_kout << "  lapic_fs_per_cycle: " << (uint64_t)complex->lapic_fs_per_cycle << "\n";
    bsp_kout.shift_dec();

    // ── 计算 IA32_UMWAIT_CONTROL（50μs TSC 上限） ────────────────
    if (g_env != ENV_TCG && tsc_fs_per_cycle) {
        cpuid_tmp cpuid7(0x07, 0x00);
        if (!((cpuid7.ecx >> 5) & 1))  // WAITPKG required
            tsc_panic_hlt();
        uint64_t cycles_50us = 50ULL * FS_per_mius / tsc_fs_per_cycle;
        if (cycles_50us > (1ULL << 32)) cycles_50us = (1ULL << 32) - 1;
        // [31:2] = cycles_50us >> 2, [0] = 1 (allow C0.2)
        g_umwait_control_value = ((uint32_t)(cycles_50us >> 2) << 2) | 1;
        apply_umwait_control();
        bsp_kout << "[tsc] UMWAIT: " << (uint32_t)g_umwait_control_value
                 << " (50us=" << (uint64_t)cycles_50us << " cycles)" << kendl;
    }
}

uint32_t g_umwait_control_value = 0;

void apply_umwait_control(void)
{
    if (g_umwait_control_value)
        wrmsr(MSR_IA32_UMWAIT_CONTROL, g_umwait_control_value);
}
