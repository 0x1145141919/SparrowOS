#include "util/kshell.h"
#include "util/kout.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "Scheduler/per_processor_scheduler.h"
#include <cstring>
#include <sys/io.h>

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

// 修饰键位定义（与 i8042.cpp 保持一致）
constexpr uint8_t MOD_CAPS = 1 << 3;
constexpr uint8_t MOD_NUM = 1 << 4;
constexpr uint8_t MOD_SCROLL = 1 << 5;
constexpr uint8_t MOD_SHIFT = 1 << 0;
constexpr uint8_t MOD_CTRL = 1 << 1;
constexpr uint8_t MOD_ALT = 1 << 2;

// 辅助函数：获取默认的 KURD_t 返回值
static KURD_t get_default_success() {
    return KURD_t(result_code::SUCCESS, 0, module_code::INFRA, INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE);
}

static KURD_t get_default_fail() {
    KURD_t kurd = get_default_success();
    // 构造一个 FAIL 状态的 KURD_t
    return KURD_t(result_code::FAIL, 0, module_code::INFRA, INFR_LOCATIONS::KSHELL, 0, level_code::ERROR, err_domain::CORE_MODULE);
}

/**
 * @brief KDB - 锁定键快速查询命令
 * 
 * 无参数，大小写不敏感
 * 输出 Caps Lock、Num Lock、Scroll Lock 状态
 */
KURD_t cmd_kdb(const line_t* line) {
    KURD_t fail = get_default_fail();
    KURD_t success = get_default_success();
    
    fail.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;
    success.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;
    
    // 获取当前事件以读取修饰键状态
    uint64_t current_seq = i8042_get_publish_seq();
    ps_2_keyboard_event current_event;
    
    if (current_seq == 0 || !i8042_read_event_by_seq(current_seq - 1, &current_event)) {
        // 如果没有事件，使用默认状态
        current_event.modifiers = 0;
    }
    
    bsp_kout << kendl;
    bsp_kout << "Keyboard Lock Status:" << kendl;
    bsp_kout << "---------------------" << kendl;
    
    // Caps Lock 状态
    bsp_kout << "  Caps Lock:   ";
    if (current_event.modifiers & MOD_CAPS) {
        bsp_kout << "ON";
    } else {
        bsp_kout << "OFF";
    }
    bsp_kout << kendl;
    
    // Num Lock 状态
    bsp_kout << "  Num Lock:    ";
    if (current_event.modifiers & MOD_NUM) {
        bsp_kout << "ON";
    } else {
        bsp_kout << "OFF";
    }
    bsp_kout << kendl;
    
    // Scroll Lock 状态
    bsp_kout << "  Scroll Lock: ";
    if (current_event.modifiers & MOD_SCROLL) {
        bsp_kout << "ON";
    } else {
        bsp_kout << "OFF";
    }
    bsp_kout << kendl;
    
    bsp_kout << kendl;
    bsp_kout << "Note: LED indicators are NOT maintained by the system" << kendl;
    bsp_kout << kendl;
    
    return success;
}

