#include "exec_env_detect.h"
#include "util/arch/x86-64/cpuid_intel.h"
runtime_env g_env;

/*
 * Sole criterion: CPUID leaf 0x40000000 (hypervisor signature).
 *   "KVMKVMKVM"    → KVM
 *   "TCGTCGTCGTCG" → TCG
 *   anything else   → bare metal
 *
 * WAITPKG etc. are pure capability probes, NOT environment indicators.
 */
runtime_env probe_env(void)
{
    cpuid_tmp cpuid(0x40000000, 0);

    /*
     * All four registers are architecture-defined for leaf 0x40000000.
     * Match the full quad to avoid false positives from leaf aliasing.
     */
    /* KVM: "KVMKVMKVM\0\0\0"  EAX=0x40000001 */
    if (cpuid.ebx == 0x4B4D564B &&
        cpuid.ecx == 0x564B4D56 &&
        cpuid.edx == 0x0000004D)
        return ENV_KVM;

    /* TCG: "TCGTCGTCGTCG"  EAX=0x40000001 */
    if (cpuid.eax == 0x40000001 &&
        cpuid.ebx == 0x54474354 &&
        cpuid.ecx == 0x43544743 &&
        cpuid.edx == 0x47435447)
        return ENV_TCG;

    /* No known hypervisor signature → bare metal */
    return ENV_BARE_METAL;
}