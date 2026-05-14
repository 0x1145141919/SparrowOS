// ════════════════════════════════════════════════════════════════
// kdeps_stubs.cpp — 用户态 kout 所需的内核依赖垫片
// ════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <unistd.h>

// Interrupt stubs
namespace InterruptSystem {
    bool is_interrupts_enabled() { return true; }
    bool early_interrupt_entered() { return false; }
}

// kcirclebufflogMgr stubs
void circle_buff_log_write(const char*, uint64_t) {}

// PortDriver stubs
namespace PortDriver {
    void early_write_char(char c) { write(2, &c, 1); }
}

// ktime stubs
namespace ktime_ns {
    uint64_t ktime_ns_now() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
}

// panic stub
struct panic_info_inshort { bool a, b, c, d, e; };
struct panic_behaviors_flags { uint64_t v; };
namespace Panic {
    void panic(panic_behaviors_flags, const char* msg, ...) {
        fprintf(stderr, "PANIC: %s\n", msg);
        _exit(1);
    }
}

// KspacePageTable symbol referenced by AddresSpace.h inline
// Just define the extern symbols
class KspacePageTable;
KspacePageTable* kspace_pagetable = nullptr;

// AddresSpace globals
uint64_t base_kernel_address = 0x100000;
