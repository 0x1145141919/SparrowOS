#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"

using namespace kio;

// 命令处理函数前向声明（实际实现在各模块文件中）
// i8042 键盘诊断命令（i8042_kshell.cpp）
extern KURD_t cmd_kbdstatus(const line_t* line);
extern KURD_t cmd_kbdevents(const line_t* line);
extern KURD_t cmd_kbdchars(const line_t* line);
extern KURD_t cmd_kbdmodifiers(const line_t* line);
extern KURD_t cmd_kdb(const line_t* line);
extern KURD_t cmd_kbdsubscribers(const line_t* line);
extern KURD_t cmd_kbdmonitor(const line_t* line);
extern KURD_t cmd_kbdtest(const line_t* line);
extern KURD_t cmd_i8042regs(const line_t* line);

// ── 命令表：i8042 键盘诊断 ──────────────────────────────────
static command_entry_t g_i8042_command_table[] = {
    {"kbdstatus",      "Keyboard system overall status (brief/normal/full)", cmd_kbdstatus,      command_risk_level_t::SAFE, false},
    {"kbdevents",      "Event statistics (summary/rate/errors)",             cmd_kbdevents,      command_risk_level_t::SAFE, false},
    {"kbdchars",       "Char buffer statistics (summary/distribution/drops)",cmd_kbdchars,       command_risk_level_t::SAFE, false},
    {"kbdmodifiers",   "Modifier key state (current/history/map)",           cmd_kbdmodifiers,   command_risk_level_t::SAFE, false},
    {"kdb",            "Quick lock key status query",                        cmd_kdb,            command_risk_level_t::SAFE, false},
    {"kbdsubscribers", "Subscriber queue status (all/scancode/analyzed/char)",cmd_kbdsubscribers,command_risk_level_t::SAFE, false},
    {"kbdmonitor",     "Real-time event monitoring (events/chars/both)",      cmd_kbdmonitor,    command_risk_level_t::SAFE, false},
    {"kbdtest",        "Interactive keyboard input test",                    cmd_kbdtest,        command_risk_level_t::SAFE, false},
    {"i8042regs",      "i8042 controller register state",                    cmd_i8042regs,      command_risk_level_t::SAFE, false},
};

static constexpr size_t I8042_CMD_COUNT =
    sizeof(g_i8042_command_table) / sizeof(g_i8042_command_table[0]);

/**
 * @brief 注册所有 i8042 键盘诊断命令
 * 
 * 各模块的初始化阶段调用此函数（通常在对应硬件初始化完成后）。
 * 如果 i8042 管线未初始化，注册仍会成功但命令执行时可能无法获取数据。
 */
// ═══════════════════════════════════════════════════════════════════
//  UEFI 运行时服务命令表
// ═══════════════════════════════════════════════════════════════════

// UEFI 命令处理函数前向声明
// 实现在 src/firmware/uefi_kshell_commands.cpp
extern KURD_t cmd_uefitime(const line_t* line);
extern KURD_t cmd_uefisettime(const line_t* line);
extern KURD_t cmd_uefireboot(const line_t* line);
extern KURD_t cmd_ueficreset(const line_t* line);
extern KURD_t cmd_uefishutdown(const line_t* line);
extern KURD_t cmd_uefiptrs(const line_t* line);

// 电源命令内部自行处理确认（特定确认词 REBOOT/SHUTDOWN），
// 因此 need_confirm=false 避免框架默认的 "yes" 确认。
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
};

static constexpr size_t UEFI_CMD_COUNT =
    sizeof(g_uefi_command_table) / sizeof(g_uefi_command_table[0]);

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

KURD_t kshell_framework_t::initial_commands_regist()
{
    register_i8042_kshell_commands();
    register_uefi_kshell_commands();
    return default_success();
}