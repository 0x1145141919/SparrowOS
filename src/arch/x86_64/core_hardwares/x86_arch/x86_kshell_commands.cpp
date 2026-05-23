/**
 * @file x86_kshell_commands.cpp
 * @brief kshell x86 架构诊断命令
 *
 * 遵行 Docs/kshell_x86_architecture_commands_design.md。
 * 所有命令 SAFE 等级，直接执行无确认。
 * 零 libc/libc++ 依赖。
 *
 * @note 数值 token 的解析由 parse_line 在分词阶段自动完成，
 *       结果存储在 token.num_value 中。命令处理器通过
 *       token_to_uint64() 直接读取，无需手写解析器。
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include <sys/io.h>

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

static bool tok_eq(const token_t& t, const char* s) {
    size_t n = strlen_in_kernel(s);
    return (t.len == n) && (strcmp_in_kernel(t.str, s, n) == 0);
}

// ── cpuid ─────────────────────────────────────────────────────

KURD_t cmd_cpuid(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: cpuid <leaf> [subleaf]" << kendl;
        return ok;
    }

    uint64_t leaf;
    if (!token_to_uint64(line->tokens[1], &leaf)) {
        bsp_kout << "[ERROR] invalid leaf" << kendl; return ok;
    }

    uint64_t sub = 0;
    if (line->token_count >= 3) {
        if (!token_to_uint64(line->tokens[2], &sub)) {
            bsp_kout << "[ERROR] invalid subleaf" << kendl; return ok;
        }
    }

    cpuid_tmp cp((uint32_t)leaf, (uint32_t)sub);

    bsp_kout << "CPUID leaf=0x" << HEX << leaf << DEC << " subleaf=" << sub << kendl;
    bsp_kout << "  eax=0x" << HEX << cp.eax << DEC
             << "  ebx=0x" << HEX << cp.ebx << DEC
             << "  ecx=0x" << HEX << cp.ecx << DEC
             << "  edx=0x" << HEX << cp.edx << DEC << kendl;

    // Parse known leaves
    if (leaf == 0) {
        char vendor[13];
        vendor[0] = (cp.ebx >> 0) & 0xFF; vendor[1] = (cp.ebx >> 8) & 0xFF;
        vendor[2] = (cp.ebx >> 16) & 0xFF; vendor[3] = (cp.ebx >> 24) & 0xFF;
        vendor[4] = (cp.edx >> 0) & 0xFF; vendor[5] = (cp.edx >> 8) & 0xFF;
        vendor[6] = (cp.edx >> 16) & 0xFF; vendor[7] = (cp.edx >> 24) & 0xFF;
        vendor[8] = (cp.ecx >> 0) & 0xFF; vendor[9] = (cp.ecx >> 8) & 0xFF;
        vendor[10] = (cp.ecx >> 16) & 0xFF; vendor[11] = (cp.ecx >> 24) & 0xFF;
        vendor[12] = 0;
        bsp_kout << "  Vendor: " << vendor << kendl;
    }
    if (leaf == 1) {
        bsp_kout << "  Stepping=" << (cp.eax & 0xF)
                 << " Model=" << ((cp.eax >> 4) & 0xF)
                 << " Family=" << ((cp.eax >> 8) & 0xF)
                 << " ExtModel=" << ((cp.eax >> 16) & 0xF)
                 << " ExtFamily=" << ((cp.eax >> 20) & 0xFF) << kendl;
    }
    if (leaf >= 0x80000002 && leaf <= 0x80000004) {
        static char brand[49];
        uint32_t* bp = (uint32_t*)brand + ((leaf - 0x80000002) * 4);
        bp[0] = cp.eax; bp[1] = cp.ebx; bp[2] = cp.ecx; bp[3] = cp.edx;
        if (leaf == 0x80000004) {
            brand[48] = 0;
            bsp_kout << "  Brand: " << brand << kendl;
        }
    }

    return ok;
}

// ── cpuinfo ────────────────────────────────────────────────────

KURD_t cmd_cpuinfo(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    cpuid_tmp cp0(0, 0);
    cpuid_tmp cp1(1, 0);
    cpuid_tmp cp7(7, 0);
    cpuid_tmp cpext(0x80000000, 0);

    bsp_kout << "=== CPU Info ===" << kendl;

    // Vendor
    char vendor[13];
    vendor[0]  = (cp0.ebx >> 0) & 0xFF; vendor[1]  = (cp0.ebx >> 8) & 0xFF;
    vendor[2]  = (cp0.ebx >> 16) & 0xFF; vendor[3] = (cp0.ebx >> 24) & 0xFF;
    vendor[4]  = (cp0.edx >> 0) & 0xFF; vendor[5]  = (cp0.edx >> 8) & 0xFF;
    vendor[6]  = (cp0.edx >> 16) & 0xFF; vendor[7] = (cp0.edx >> 24) & 0xFF;
    vendor[8]  = (cp0.ecx >> 0) & 0xFF; vendor[9]  = (cp0.ecx >> 8) & 0xFF;
    vendor[10] = (cp0.ecx >> 16) & 0xFF; vendor[11]= (cp0.ecx >> 24) & 0xFF;
    vendor[12] = 0;
    bsp_kout << "  Vendor: " << vendor << kendl;

    // Brand string
    if (cpext.eax >= 0x80000004) {
        char brand[49];
        for (uint32_t l = 0x80000002; l <= 0x80000004; l++) {
            cpuid_tmp cpb(l, 0);
            uint32_t* bp = (uint32_t*)brand + ((l - 0x80000002) * 4);
            bp[0] = cpb.eax; bp[1] = cpb.ebx; bp[2] = cpb.ecx; bp[3] = cpb.edx;
        }
        brand[48] = 0;
        bsp_kout << "  Brand: " << brand << kendl;
    }

    // CPUID leaf 1 details
    uint8_t stepping = cp1.eax & 0xF;
    uint8_t model    = (cp1.eax >> 4) & 0xF;
    uint8_t family   = (cp1.eax >> 8) & 0xF;
    uint8_t ext_model= (cp1.eax >> 16) & 0xF;
    uint8_t ext_family = (cp1.eax >> 20) & 0xFF;
    if (family == 15) family += ext_family;
    if (family == 15 || family == 6) model = (ext_model << 4) | model;

    bsp_kout << "  Family: " << (uint64_t)family
             << "  Model: " << (uint64_t)model
             << "  Stepping: " << (uint64_t)stepping << kendl;

    // Topology
    uint8_t core_count = (uint8_t)((cp1.ebx >> 16) & 0xFF);
    bsp_kout << "  Logical CPUs: " << (uint64_t)core_count << kendl;

    // Feature flags (basics)
    bsp_kout << "  Features:";
    if (cp1.edx & (1 << 4))  bsp_kout << " TSC";
    if (cp1.edx & (1 << 15)) bsp_kout << " CMOV";
    if (cp1.edx & (1 << 23)) bsp_kout << " MMX";
    if (cp1.edx & (1 << 25)) bsp_kout << " SSE";
    if (cp1.edx & (1 << 26)) bsp_kout << " SSE2";
    if (cp1.ecx & (1 << 0))  bsp_kout << " SSE3";
    if (cp1.ecx & (1 << 9))  bsp_kout << " SSSE3";
    if (cp1.ecx & (1 << 19)) bsp_kout << " SSE4.1";
    if (cp1.ecx & (1 << 20)) bsp_kout << " SSE4.2";
    if (cp1.ecx & (1 << 28)) bsp_kout << " AVX";
    if (cp1.ecx & (1 << 12)) bsp_kout << " FMA";
    if (cp1.ecx & (1 << 31)) bsp_kout << " RDRAND";
    bsp_kout << kendl;

    bsp_kout << "  Ext Features:";
    if (cp1.ecx & (1 << 5))  bsp_kout << " VMX";
    if (cp1.ecx & (1 << 21)) bsp_kout << " x2APIC";
    if (cp7.ebx & (1 << 1))  bsp_kout << " AVX2";
    if (cp7.ebx & (1 << 31)) bsp_kout << " AVX512F";
    bsp_kout << kendl;

    bsp_kout << "  APIC ID: " << (uint64_t)query_apicid() << kendl;
    return ok;
}

// ── rdmsr ─────────────────────────────────────────────────────

KURD_t cmd_rdmsr(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: rdmsr <address>" << kendl;
        return ok;
    }

    uint64_t addr;
    if (!token_to_uint64(line->tokens[1], &addr) || addr > 0xFFFFFFFF) {
        bsp_kout << "[ERROR] Invalid MSR address" << kendl;
        return ok;
    }

    uint64_t val = rdmsr((uint32_t)addr);
    bsp_kout << "MSR[0x" << HEX << addr << DEC << "] = 0x" << HEX << val << DEC << kendl;

    // Common MSR bitfield parsing
    if (addr == 0x1B) { // IA32_APIC_BASE
        bsp_kout << "  BSP=" << ((val >> 8) & 1)
                 << " x2APIC=" << ((val >> 10) & 1)
                 << " xAPIC=" << ((val >> 11) & 1)
                 << " Base=0x" << HEX << uint64_t(val & ~0xFFFULL) << DEC << kendl;
    }
    if (addr == 0x3A) { // IA32_FEATURE_CONTROL
        bsp_kout << "  Lock=" << (val & 1)
                 << " VMXON=" << ((val>>1)&1)
                 << " VMXSMX=" << ((val>>2)&1) << kendl;
    }
    if (addr == 0x3B) { // IA32_TSC_ADJUST
        bsp_kout << "  TSC Adjust: " << (int64_t)(int32_t)(val & 0xFFFFFFFF) << kendl;
    }
    return ok;
}


// ── wrmsr ─────────────────────────────────────────────────────

KURD_t cmd_wrmsr(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: wrmsr <address> <value>" << kendl;
        return ok;
    }

    uint64_t addr, value;
    if (!token_to_uint64(line->tokens[1], &addr) || addr > 0xFFFFFFFF ||
        !token_to_uint64(line->tokens[2], &value)) {
        bsp_kout << "[ERROR] Invalid address or value" << kendl;
        return ok;
    }

    uint64_t old = rdmsr((uint32_t)addr);
    bsp_kout << "[wrmsr] old=0x" << HEX << old << DEC;
    wrmsr_func((uint32_t)addr, value);
    bsp_kout << " new=0x" << HEX << value << DEC << kendl;
    return ok;
}

// ── rdtsc ─────────────────────────────────────────────────────

KURD_t cmd_rdtsc(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    uint64_t tsc = rdtsc();
    bsp_kout << "TSC = " << tsc << "  (0x" << HEX << tsc << DEC << ")" << kendl;

    extern bool is_tsc_reliable;
    extern uint64_t tsc_fs_per_cycle;
    if (is_tsc_reliable && tsc_fs_per_cycle > 0) {
        uint64_t ns = (tsc / 1000000) * (tsc_fs_per_cycle / 1000);
        bsp_kout << "  ~" << ns << " ns since boot  (freq=" << tsc_fs_per_cycle << " fs/cycle)" << kendl;
    } else {
        bsp_kout << "  (TSC unreliable or freq unknown)" << kendl;
    }
    return ok;
}

// ── inb/inw/inl ───────────────────────────────────────────────

KURD_t cmd_inb(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: inb <port> [count]" << kendl;
        return ok;
    }
    uint64_t port, count = 1;
    if (!token_to_uint64(line->tokens[1], &port) || port > 0xFFFF) { bsp_kout << "[ERROR] invalid port" << kendl; return ok; }
    if (line->token_count >= 3 && !token_to_uint64(line->tokens[2], &count)) { bsp_kout << "[ERROR] invalid count" << kendl; return ok; }
    if (count > 256) count = 256;

    for (uint64_t i = 0; i < count; i++) {
        uint8_t v = inb((uint16_t)(port + i));
        bsp_kout << HEX << (port + i) << DEC << " → ";
        if (v >= 0x20 && v < 0x7F) bsp_kout << "'" << (char)v << "' ";
        bsp_kout << "0x" << HEX << v << DEC;
        if (i < count - 1) bsp_kout << "  ";
    }
    bsp_kout << kendl;
    return ok;
}

KURD_t cmd_inw(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: inw <port>" << kendl;
        return ok;
    }
    uint64_t port;
    if (!token_to_uint64(line->tokens[1], &port) || port > 0xFFFF) { bsp_kout << "[ERROR] invalid port" << kendl; return ok; }
    uint16_t v = inw((uint16_t)port);
    bsp_kout << "inw(0x" << HEX << port << DEC << ") = 0x" << HEX << v << DEC << kendl;
    return ok;
}

KURD_t cmd_inl(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: inl <port>" << kendl;
        return ok;
    }
    uint64_t port;
    if (!token_to_uint64(line->tokens[1], &port) || port > 0xFFFF) { bsp_kout << "[ERROR] invalid port" << kendl; return ok; }
    uint32_t v = inl((uint16_t)port);
    bsp_kout << "inl(0x" << HEX << port << DEC << ") = 0x" << HEX << v << DEC << kendl;
    return ok;
}

// ── outb/outw/outl ────────────────────────────────────────────

KURD_t cmd_outb(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: outb <port> <value>" << kendl;
        return ok;
    }
    uint64_t port, val;
    if (!token_to_uint64(line->tokens[1], &port) || port > 0xFFFF ||
        !token_to_uint64(line->tokens[2], &val) || val > 255) {
        bsp_kout << "[ERROR] invalid port or value" << kendl; return ok;
    }
    outb((uint8_t)val, (uint16_t)port);
    bsp_kout << "outb(0x" << HEX << port << DEC << ", 0x" << HEX << val << DEC << ")" << kendl;
    return ok;
}

KURD_t cmd_outw(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: outw <port> <value>" << kendl;
        return ok;
    }
    uint64_t port, val;
    if (!token_to_uint64(line->tokens[1], &port) || port > 0xFFFF ||
        !token_to_uint64(line->tokens[2], &val) || val > 0xFFFF) {
        bsp_kout << "[ERROR] invalid port or value" << kendl; return ok;
    }
    outw((uint16_t)val, (uint16_t)port);
    bsp_kout << "outw(0x" << HEX << port << DEC << ", 0x" << HEX << val << DEC << ")" << kendl;
    return ok;
}

KURD_t cmd_outl(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: outl <port> <value>" << kendl;
        return ok;
    }
    uint64_t port, val;
    if (!token_to_uint64(line->tokens[1], &port) || port > 0xFFFF ||
        !token_to_uint64(line->tokens[2], &val) || val > 0xFFFFFFFF) {
        bsp_kout << "[ERROR] invalid port or value" << kendl; return ok;
    }
    outl((uint32_t)val, (uint16_t)port);
    bsp_kout << "outl(0x" << HEX << port << DEC << ", 0x" << HEX << val << DEC << ")" << kendl;
    return ok;
}

// ── apic ─────────────────────────────────────────────────────

KURD_t cmd_apic(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    uint32_t apic_id = query_x2apicid();
    uint32_t x2apic_id = 0;
    bool x2apic = is_x2apic_supported();

    bsp_kout << "=== APIC Status ===" << kendl;
    bsp_kout << "  APIC mode: " << (x2apic ? "x2APIC" : "xAPIC") << kendl;
    bsp_kout << "  APIC ID:   " << (uint64_t)apic_id << " (0x" << HEX << apic_id << DEC << ")" << kendl;

    // Read APIC version and TPR from MSR in x2APIC mode
    if (x2apic) {
        uint64_t ver = rdmsr(0x803);  // APIC_VERSION (offset 0x30)
        bsp_kout << "  Version:   " << (ver & 0xFF) << kendl;
        uint64_t svr = rdmsr(0x80F); // SVR (offset 0xF0)
        bsp_kout << "  SVR:       0x" << HEX << svr << DEC;
        if (svr & (1<<8)) bsp_kout << " [ENABLED]";
        bsp_kout << kendl;

        uint64_t tpr = rdmsr(0x808); // TPR (offset 0x80)
        bsp_kout << "  TPR:       0x" << HEX << tpr << DEC << kendl;

        // LVT entries
        uint64_t lvt_timer   = rdmsr(0x832); // LVT_TIMER
        uint64_t lvt_error   = rdmsr(0x833); // LVT_ERROR
        uint64_t lvt_lint0   = rdmsr(0x834); // LVT_LINT0
        uint64_t lvt_lint1   = rdmsr(0x835); // LVT_LINT1
        bsp_kout << "  LVT Timer:  0x" << HEX << lvt_timer << DEC << kendl;
        bsp_kout << "  LVT Error:  0x" << HEX << lvt_error << DEC << kendl;
        bsp_kout << "  LVT LINT0:  0x" << HEX << lvt_lint0 << DEC << kendl;
        bsp_kout << "  LVT LINT1:  0x" << HEX << lvt_lint1 << DEC << kendl;
    } else {
        bsp_kout << "  (use rdmsr 0x1B for xAPIC base)" << kendl;
    }
    return ok;
}

// ── cr ────────────────────────────────────────────────────────

static uint64_t read_cr0() { uint64_t v; asm("mov %%cr0, %0" : "=r"(v)); return v; }
static uint64_t read_cr2() { uint64_t v; asm("mov %%cr2, %0" : "=r"(v)); return v; }
static uint64_t read_cr3() { uint64_t v; asm("mov %%cr3, %0" : "=r"(v)); return v; }
static uint64_t read_cr4() { uint64_t v; asm("mov %%cr4, %0" : "=r"(v)); return v; }

KURD_t cmd_cr(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: cr <0|2|3|4>" << kendl;
        return ok;
    }

    uint64_t reg;
    if (!token_to_uint64(line->tokens[1], &reg)) {
        bsp_kout << "[ERROR] invalid register" << kendl; return ok;
    }

    uint64_t val;
    switch (reg) {
        case 0: val = read_cr0();
            bsp_kout << "CR0 = 0x" << HEX << val << DEC << kendl;
            bsp_kout << "  PE=" << ((val>>0)&1) << " MP=" << ((val>>1)&1)
                     << " EM=" << ((val>>2)&1) << " TS=" << ((val>>3)&1)
                     << " ET=" << ((val>>4)&1) << " NE=" << ((val>>5)&1) << kendl;
            bsp_kout << "  WP=" << ((val>>16)&1) << " AM=" << ((val>>18)&1)
                     << " NW=" << ((val>>29)&1) << " CD=" << ((val>>30)&1)
                     << " PG=" << ((val>>31)&1) << kendl;
            break;
        case 2: val = read_cr2();
            bsp_kout << "CR2 = 0x" << HEX << val << DEC
                     << "  (Page Fault Linear Address)" << kendl;
            break;
        case 3: val = read_cr3();
            bsp_kout << "CR3 = 0x" << HEX << val << DEC
                     << "  PML4 Physical Base = 0x" << HEX << uint64_t(val & ~0xFFFULL) << DEC << kendl;
            break;
        case 4: val = read_cr4();
            bsp_kout << "CR4 = 0x" << HEX << val << DEC << kendl;
            bsp_kout << "  VME=" << ((val>>0)&1) << " PVI=" << ((val>>1)&1)
                     << " TSD=" << ((val>>2)&1) << " DE=" << ((val>>3)&1)
                     << " PSE=" << ((val>>4)&1) << " PAE=" << ((val>>5)&1) << kendl;
            bsp_kout << "  MCE=" << ((val>>6)&1) << " PGE=" << ((val>>7)&1)
                     << " PCE=" << ((val>>8)&1) << " OSFXSR=" << ((val>>9)&1)
                     << " OSXMMEXCPT=" << ((val>>10)&1) << kendl;
            bsp_kout << "  SMEP=" << ((val>>20)&1) << " SMAP=" << ((val>>21)&1)
                     << " PKE=" << ((val>>22)&1) << kendl;
            break;
        default:
            bsp_kout << "[ERROR] CR" << reg << " not supported (use 0/2/3/4)" << kendl;
            return ok;
    }
    return ok;
}

// ── hpet ──────────────────────────────────────────────────────

KURD_t cmd_hpet(const line_t* line) {
    (void)line;
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (readonly_timer == nullptr) {
        bsp_kout << "[ERROR] HPET not available" << kendl;
        return ok;
    }

    uint64_t ts = readonly_timer->get_time_stamp_in_us();
    bsp_kout << "=== HPET ===" << kendl;
    bsp_kout << "  Timestamp: " << ts << " us (" << (ts/1000) << " ms)" << kendl;
    return ok;
}
