// ════════════════════════════════════════════════════════════════
// shutdown.cpp — 广播关机（从 kinit.cpp 迁出，2026-09-18）
// 接口声明见 src/include/boot/shutdown.h。
// ════════════════════════════════════════════════════════════════
#include "boot/shutdown.h"

#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"       // nvme_parallel_offline_all
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"  // fly_ipi_send / ipi_package_t
#include "util/arch/x86-64/cpuid_intel.h"                       // fast_get_processor_id
#include "ktime.h"                                              // ktime::get_microsecond_stamp
#include "abi/boot.h"                                           // logical_processor_count

// ─── 核心关机 IPI handler（跑飞型，三指令收工） ────────────
// 由 fly_ipi_send 投递到目标核，cli + wbinvd + hlt 后永不复返
static uint64_t ipi_shutdown_func(void*)
{
    asm volatile("cli; wbinvd; hlt" ::: "memory");
    return 1;
}

// ─── 广播关机 ─────────────────────────────────────────────
// 遍历除 self 外所有 AP，逐一 fly_ipi_send 关机
// 50ms 硬上限，超时则跳过剩余 AP，发起者自救
extern "C" void broadcast_shutdown()
{
    // 关机：并行析构/关机所有 NVMe 控制器（每控制器一线程，汇报画板 ≤5s 轮询）
    nvme_parallel_offline_all();

    uint32_t self = fast_get_processor_id();
    uint32_t nproc = logical_processor_count;
    uint64_t deadline = ktime::get_microsecond_stamp() + 50000;

    for (uint32_t pid = 0; pid < nproc; pid++) {
        if (pid == self) continue;
        if (ktime::get_microsecond_stamp() >= deadline)
            break;

        ipi_package_t ipi;
        ipi.arg        = nullptr;
        ipi.func       = (uint64_t)ipi_shutdown_func;
        ipi.id         = pid;
        ipi.is_apicid  = false;
        ipi.is_returnable = false;

        fly_ipi_send(&ipi);  // best-effort
    }

    // 发起者自救
}
