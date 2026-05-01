#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"

using namespace kio;

// ═══════════════════════════════════════════════════════════════════
//  前向声明：各模块命令处理函数
// ═══════════════════════════════════════════════════════════════════

// i8042 键盘诊断命令（src/arch/x86_64/core_hardwares/i8042_kshell.cpp）
extern KURD_t cmd_kbdstatus(const line_t* line);
extern KURD_t cmd_kbdevents(const line_t* line);
extern KURD_t cmd_kbdchars(const line_t* line);
extern KURD_t cmd_kbdmodifiers(const line_t* line);
extern KURD_t cmd_kdb(const line_t* line);
extern KURD_t cmd_kbdsubscribers(const line_t* line);
extern KURD_t cmd_kbdmonitor(const line_t* line);
extern KURD_t cmd_kbdtest(const line_t* line);
extern KURD_t cmd_i8042regs(const line_t* line);

// UEFI 运行时服务命令（src/firmware/uefi_kshell_commands.cpp）
extern KURD_t cmd_uefitime(const line_t* line);
extern KURD_t cmd_uefisettime(const line_t* line);
extern KURD_t cmd_uefireboot(const line_t* line);
extern KURD_t cmd_ueficreset(const line_t* line);
extern KURD_t cmd_uefishutdown(const line_t* line);
extern KURD_t cmd_uefiptrs(const line_t* line);
extern KURD_t cmd_get_macro_time(const line_t* line);
extern KURD_t cmd_set_marcro_time(const line_t* line);

// 内存操作命令（src/memory/mem_kshell_commands.cpp）
extern KURD_t cmd_palloc(const line_t* line);
extern KURD_t cmd_pfree(const line_t* line);
extern KURD_t cmd_valloc(const line_t* line);
extern KURD_t cmd_vfree(const line_t* line);
extern KURD_t cmd_pread(const line_t* line);
extern KURD_t cmd_pwrite(const line_t* line);
extern KURD_t cmd_pmap(const line_t* line);
extern KURD_t cmd_punmap(const line_t* line);
extern KURD_t cmd_dmap(const line_t* line);
extern KURD_t cmd_stackalloc(const line_t* line);
extern KURD_t cmd_kvread(const line_t* line);
extern KURD_t cmd_kvwrite(const line_t* line);
extern KURD_t cmd_phymem(const line_t* line);
extern KURD_t cmd_vminfo(const line_t* line);

// PCIe 观察命令（src/arch/x86_64/core_hardwares/pcie_kshell_commands.cpp）
extern KURD_t cmd_pcie_segs(const line_t* line);
extern KURD_t cmd_pcie_BDFs(const line_t* line);
extern KURD_t cmd_pcie_BDF(const line_t* line);

// x86 架构诊断命令（src/arch/x86_64/core_hardwares/x86_kshell_commands.cpp）
extern KURD_t cmd_cpuid(const line_t* line);
extern KURD_t cmd_cpuinfo(const line_t* line);
extern KURD_t cmd_rdmsr(const line_t* line);
extern KURD_t cmd_wrmsr(const line_t* line);
extern KURD_t cmd_rdtsc(const line_t* line);
extern KURD_t cmd_hpet(const line_t* line);
extern KURD_t cmd_inb(const line_t* line);
extern KURD_t cmd_inw(const line_t* line);
extern KURD_t cmd_inl(const line_t* line);
extern KURD_t cmd_outb(const line_t* line);
extern KURD_t cmd_outw(const line_t* line);
extern KURD_t cmd_outl(const line_t* line);
extern KURD_t cmd_apic(const line_t* line);
extern KURD_t cmd_cr(const line_t* line);

// ═══════════════════════════════════════════════════════════════════
//  i8042 键盘诊断命令表
// ═══════════════════════════════════════════════════════════════════

static command_entry_t g_i8042_command_table[] = {
    {"kbdstatus",      "Keyboard system overall status (brief/normal/full)",
        cmd_kbdstatus,      command_risk_level_t::SAFE, false},
    {"kbdevents",      "Event statistics (summary/rate/errors)",
        cmd_kbdevents,      command_risk_level_t::SAFE, false},
    {"kbdchars",       "Char buffer statistics (summary/distribution/drops)",
        cmd_kbdchars,       command_risk_level_t::SAFE, false},
    {"kbdmodifiers",   "Modifier key state (current/history/map)",
        cmd_kbdmodifiers,   command_risk_level_t::SAFE, false},
    {"kdb",            "Quick lock key status query",
        cmd_kdb,            command_risk_level_t::SAFE, false},
    {"kbdsubscribers", "Subscriber queue status (all/scancode/analyzed/char)",
        cmd_kbdsubscribers, command_risk_level_t::SAFE, false},
    {"kbdmonitor",     "Real-time event monitoring (events/chars/both)",
        cmd_kbdmonitor,     command_risk_level_t::SAFE, false},
    {"kbdtest",        "Interactive keyboard input test",
        cmd_kbdtest,         command_risk_level_t::SAFE, false},
    {"i8042regs",      "i8042 controller register state",
        cmd_i8042regs,       command_risk_level_t::SAFE, false},
};

static constexpr size_t I8042_CMD_COUNT =
    sizeof(g_i8042_command_table) / sizeof(g_i8042_command_table[0]);

// ═══════════════════════════════════════════════════════════════════
//  UEFI 运行时服务命令表
// ═══════════════════════════════════════════════════════════════════

static command_entry_t g_uefi_command_table[] = {
    {"uefitime",       "Query UEFI RTC time (full/simple/timestamp)",
        cmd_uefitime,       command_risk_level_t::SAFE, false},
    {"uefisettime",    "Set UEFI RTC time (YYYY-MM-DD HH:MM:SS [-f])",
        cmd_uefisettime,    command_risk_level_t::WARNING, true},
    {"uefireboot",     "System warm reboot",
        cmd_uefireboot,     command_risk_level_t::DANGEROUS, false},
    {"ueficreset",     "System cold reset",
        cmd_ueficreset,     command_risk_level_t::DANGEROUS, false},
    {"uefishutdown",   "System shutdown",
        cmd_uefishutdown,   command_risk_level_t::DANGEROUS, false},
    {"uefiptrs",       "UEFI RT function pointer table",
        cmd_uefiptrs,       command_risk_level_t::SAFE, false},
    {"get_macro_time", "Read OS RTC-anchored calendar time",
        cmd_get_macro_time, command_risk_level_t::SAFE, false},
    {"set_marcro_time","Set OS time calibration offset (no RTC write)",
        cmd_set_marcro_time,command_risk_level_t::WARNING, false},
};

static constexpr size_t UEFI_CMD_COUNT =
    sizeof(g_uefi_command_table) / sizeof(g_uefi_command_table[0]);

// ═══════════════════════════════════════════════════════════════════
//  内存操作命令表
// ═══════════════════════════════════════════════════════════════════

static command_entry_t g_mem_command_table[] = {
    {"palloc",     "Allocate physical pages [size align_log2]",
        cmd_palloc,      command_risk_level_t::SAFE, false},
    {"pfree",      "Free physical pages <phyaddr> <size>",
        cmd_pfree,       command_risk_level_t::SAFE, false},
    {"valloc",     "Allocate virtual pages [count align_log2]",
        cmd_valloc,      command_risk_level_t::SAFE, false},
    {"vfree",      "Free virtual pages <vaddr> <count>",
        cmd_vfree,       command_risk_level_t::SAFE, false},
    {"pread",      "Read physical memory <phyaddr> <size> [hex/dec/ascii]",
        cmd_pread,       command_risk_level_t::SAFE, false},
    {"pwrite",     "Write physical memory <phyaddr> <value> [size]",
        cmd_pwrite,      command_risk_level_t::SAFE, false},
    {"pmap",       "Map physical→virtual <phyaddr> <vaddr|0> <size> [access]",
        cmd_pmap,        command_risk_level_t::SAFE, false},
    {"punmap",     "Unmap virtual mapping <vaddr> <size>",
        cmd_punmap,      command_risk_level_t::SAFE, false},
    {"dmap",       "Direct physical map <phyaddr> <size>",
        cmd_dmap,        command_risk_level_t::SAFE, false},
    {"stackalloc", "Allocate kernel stack <pages_count>",
        cmd_stackalloc,  command_risk_level_t::SAFE, false},
    {"kvread",      "Read kernel virtual memory <vaddr> <size> [fmt]",
        cmd_kvread,      command_risk_level_t::SAFE, false},
    {"kvwrite",     "Write kernel virtual memory <vaddr> <value> [size]",
        cmd_kvwrite,     command_risk_level_t::SAFE, false},
    {"phymem",      "Print physical memory segment table",
        cmd_phymem,      command_risk_level_t::SAFE, false},
    {"vminfo",      "Print VM interval mapping table",
        cmd_vminfo,      command_risk_level_t::SAFE, false},
};

static constexpr size_t MEM_CMD_COUNT =
    sizeof(g_mem_command_table) / sizeof(g_mem_command_table[0]);
// ═══════════════════════════════════════════════════════════════════
//  PCIe 观察命令表
// ═══════════════════════════════════════════════════════════════════

static command_entry_t g_pcie_command_table[] = {
    {"pcie_segs",  "List ECAM segments",
        cmd_pcie_segs,  command_risk_level_t::SAFE, false},
    {"pcie_BDFs",  "List PCIe devices in segment [--bus=N] [--class=0xCC] [--vendor=0xVVVV] [--bridge-only] [--tree]",
        cmd_pcie_BDFs,  command_risk_level_t::SAFE, false},
    {"pcie_BDF",   "Show PCIe device details <BDF> [--bars] [--caps] [--header] [--raw=OFF:LEN]",
        cmd_pcie_BDF,   command_risk_level_t::SAFE, false},
};

static constexpr size_t PCIE_CMD_COUNT =
    sizeof(g_pcie_command_table) / sizeof(g_pcie_command_table[0]);

// ═══════════════════════════════════════════════════════════════════
//  x86 架构诊断命令表
// ═══════════════════════════════════════════════════════════════════

static command_entry_t g_x86_command_table[] = {
    {"cpuid",    "CPUID query <leaf> [subleaf]",
        cmd_cpuid,    command_risk_level_t::SAFE, false},
    {"cpuinfo",  "CPU comprehensive info",
        cmd_cpuinfo,  command_risk_level_t::SAFE, false},
    {"rdmsr",    "Read MSR <address>",
        cmd_rdmsr,    command_risk_level_t::SAFE, false},
    {"wrmsr",    "Write MSR <address> <value>",
        cmd_wrmsr,    command_risk_level_t::SAFE, false},
    {"rdtsc",    "Read timestamp counter",
        cmd_rdtsc,    command_risk_level_t::SAFE, false},
    {"hpet",     "HPET timer query",
        cmd_hpet,     command_risk_level_t::SAFE, false},
    {"inb",      "Read IO port byte <port> [count]",
        cmd_inb,      command_risk_level_t::SAFE, false},
    {"inw",      "Read IO port word <port>",
        cmd_inw,      command_risk_level_t::SAFE, false},
    {"inl",      "Read IO port dword <port>",
        cmd_inl,      command_risk_level_t::SAFE, false},
    {"outb",     "Write IO port byte <port> <value>",
        cmd_outb,     command_risk_level_t::SAFE, false},
    {"outw",     "Write IO port word <port> <value>",
        cmd_outw,     command_risk_level_t::SAFE, false},
    {"outl",     "Write IO port dword <port> <value>",
        cmd_outl,     command_risk_level_t::SAFE, false},
    {"apic",     "APIC status query",
        cmd_apic,     command_risk_level_t::SAFE, false},
    {"cr",       "Read control register <0|2|3|4>",
        cmd_cr,       command_risk_level_t::SAFE, false},
};

static constexpr size_t X86_CMD_COUNT =
    sizeof(g_x86_command_table) / sizeof(g_x86_command_table[0]);
// ═══════════════════════════════════════════════════════════════════
//  注册函数
// ═══════════════════════════════════════════════════════════════════

static void regist_table(command_entry_t* table, size_t count) {
    for (size_t i = 0; i < count; i++) {
        KURD_t r = kshell_framework_t::command_register(&table[i]);
        if (r.result != 0) {
            bsp_kout << "[KSHELL] Failed to register: " << table[i].name << kendl;
        }
    }
}

void register_i8042_kshell_commands() {
    regist_table(g_i8042_command_table, I8042_CMD_COUNT);
    bsp_kout << "[KSHELL] Registered " << I8042_CMD_COUNT
             << " i8042 keyboard debug commands" << kendl;
}

void register_uefi_kshell_commands() {
    regist_table(g_uefi_command_table, UEFI_CMD_COUNT);
    bsp_kout << "[KSHELL] Registered " << UEFI_CMD_COUNT
             << " UEFI runtime service commands" << kendl;
}

void register_mem_kshell_commands() {
    regist_table(g_mem_command_table, MEM_CMD_COUNT);
    bsp_kout << "[KSHELL] Registered " << MEM_CMD_COUNT
             << " memory operation commands" << kendl;
}
void register_pcie_kshell_commands() {
    regist_table(g_pcie_command_table, PCIE_CMD_COUNT);
    bsp_kout << "[KSHELL] Registered " << PCIE_CMD_COUNT
             << " PCIe observe commands" << kendl;
}

void register_x86_kshell_commands() {
    regist_table(g_x86_command_table, X86_CMD_COUNT);
    bsp_kout << "[KSHELL] Registered " << X86_CMD_COUNT
             << " x86 architecture diagnostic commands" << kendl;
}
KURD_t kshell_framework_t::initial_commands_regist()
{
    register_i8042_kshell_commands();
    register_uefi_kshell_commands();
    register_mem_kshell_commands();
    register_pcie_kshell_commands();
    register_x86_kshell_commands();
    return default_success();
}




