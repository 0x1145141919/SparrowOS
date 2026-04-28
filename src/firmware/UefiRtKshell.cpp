#include "util/kshell.h"
#include "util/kout.h"
#include "firmware/UefiRunTimeServices.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include <cstring>

using namespace kio;

// 静态辅助函数：闰年判断
static inline bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// 静态辅助函数：获取月份天数
static inline uint8_t get_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month];
}

// 静态辅助函数：验证日期时间
static inline bool validate_datetime(uint16_t year, uint8_t month, uint8_t day, 
                                      uint8_t hour, uint8_t minute, uint8_t second) {
    if (year < 1970 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > get_days_in_month(year, month)) return false;
    if (hour > 23) return false;
    if (minute > 59) return false;
    if (second > 59) return false;
    return true;
}

// 静态辅助函数：解析数字（支持十进制）
static inline bool parse_number(const char* str, size_t len, uint64_t& out) {
    out = 0;
    for (size_t i = 0; i < len; ++i) {
        if (str[i] < '0' || str[i] > '9') return false;
        out = out * 10 + (str[i] - '0');
    }
    return true;
}

// 静态辅助函数：显示警告并请求确认
static inline bool confirm_dangerous_operation(const char* warning_msg, const char* confirm_word) {
    bsp_kout << kendl;
    bsp_kout << "WARNING: " << warning_msg << kendl;
    bsp_kout << "Type '" << confirm_word << "' to confirm: ";
    
    char input_buffer[256];
    i8042_blockable_keyboard_listening(input_buffer);
    
    // 简单比较（不区分大小写）
    size_t word_len = strlen(confirm_word);
    size_t input_len = strlen(input_buffer);
    
    if (input_len != word_len) return false;
    
    for (size_t i = 0; i < word_len; ++i) {
        char c1 = input_buffer[i];
        char c2 = confirm_word[i];
        // 转换为大写比较
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return false;
    }
    
    return true;
}

/**
 * @brief uefitime - 查询 UEFI 时间命令
 * 
 * 参数：full（默认）/ simple / timestamp
 */
KURD_t cmd_uefitime(const line_t* line) {
    KURD_t success;
    KURD_t fail;
    
    // 解析参数
    const char* format = "full";
    if (line->token_count > 1) {
        format = line->tokens[1].str;
    }
    
    // 获取 UEFI 时间
    EFI_TIME time = EFI_RT_SVS::rt_time_get();
    
    bsp_kout << kendl;
    
    if (strcmp(format, "timestamp") == 0) {
        // 输出时间戳格式
        bsp_kout << time.Year << "-" 
                 << (uint32_t)time.Month << "-" 
                 << (uint32_t)time.Day << " "
                 << (uint32_t)time.Hour << ":" 
                 << (uint32_t)time.Minute << ":" 
                 << (uint32_t)time.Second << kendl;
    } else if (strcmp(format, "simple") == 0) {
        // 简化格式
        bsp_kout << (uint32_t)time.Year << "/" 
                 << (uint32_t)time.Month << "/" 
                 << (uint32_t)time.Day << " "
                 << (uint32_t)time.Hour << ":" 
                 << (uint32_t)time.Minute << ":" 
                 << (uint32_t)time.Second << kendl;
    } else {
        // 完整格式（默认）
        bsp_kout << "UEFI System Time:" << kendl;
        bsp_kout << "-----------------" << kendl;
        bsp_kout << "  Date: " << (uint32_t)time.Year << "-" 
                 << (uint32_t)time.Month << "-" 
                 << (uint32_t)time.Day << kendl;
        bsp_kout << "  Time: " << (uint32_t)time.Hour << ":" 
                 << (uint32_t)time.Minute << ":" 
                 << (uint32_t)time.Second << "." 
                 << (uint32_t)time.Nanosecond << kendl;
        
        // 时区信息
        int16_t timezone = time.TimeZone;
        if (timezone == EFI_UNSPECIFIED_TIMEZONE) {
            bsp_kout << "  TimeZone: Unspecified" << kendl;
        } else {
            bsp_kout << "  TimeZone: UTC";
            if (timezone >= 0) {
                bsp_kout << "+";
            }
            bsp_kout << timezone << kendl;
        }
        
        // 夏令时
        bsp_kout << "  Daylight: ";
        if (time.Daylight & EFI_TIME_ADJUST_DAYLIGHT) {
            bsp_kout << "Daylight Saving Time Active";
        } else {
            bsp_kout << "Standard Time";
        }
        bsp_kout << kendl;
    }
    
    bsp_kout << kendl;
    
    return success;
}

/**
 * @brief uefisettime - 设置 UEFI 时间命令
 * 
 * 需要确认，日期格式 YYYY-MM-DD，时间格式 HH:MM:SS
 * 前置验证：闰年、月份天数、时分秒范围
 */
KURD_t cmd_uefisettime(const line_t* line) {
    KURD_t success;
    KURD_t fail;
    
    // 需要至少两个参数：日期和时间
    if (line->token_count < 3) {
        bsp_kout << "Usage: uefisettime <YYYY-MM-DD> <HH:MM:SS>" << kendl;
        return fail;
    }
    
    // 解析日期 YYYY-MM-DD
    const char* date_str = line->tokens[1].str;
    size_t date_len = line->tokens[1].len;
    
    if (date_len != 10 || date_str[4] != '-' || date_str[7] != '-') {
        bsp_kout << "Error: Invalid date format. Use YYYY-MM-DD" << kendl;
        return fail;
    }
    
    uint64_t year, month, day;
    if (!parse_number(date_str, 4, year) ||
        !parse_number(date_str + 5, 2, month) ||
        !parse_number(date_str + 8, 2, day)) {
        bsp_kout << "Error: Invalid date values" << kendl;
        return fail;
    }
    
    // 解析时间 HH:MM:SS
    const char* time_str = line->tokens[2].str;
    size_t time_len = line->tokens[2].len;
    
    if (time_len != 8 || time_str[2] != ':' || time_str[5] != ':') {
        bsp_kout << "Error: Invalid time format. Use HH:MM:SS" << kendl;
        return fail;
    }
    
    uint64_t hour, minute, second;
    if (!parse_number(time_str, 2, hour) ||
        !parse_number(time_str + 3, 2, minute) ||
        !parse_number(time_str + 6, 2, second)) {
        bsp_kout << "Error: Invalid time values" << kendl;
        return fail;
    }
    
    // 验证日期时间
    if (!validate_datetime((uint16_t)year, (uint8_t)month, (uint8_t)day,
                           (uint8_t)hour, (uint8_t)minute, (uint8_t)second)) {
        bsp_kout << "Error: Invalid date/time values (check leap year, month days, ranges)" << kendl;
        return fail;
    }
    
    // 显示将要设置的时间
    bsp_kout << kendl;
    bsp_kout << "Set UEFI time to:" << kendl;
    bsp_kout << "  " << year << "-" << month << "-" << day << " "
             << hour << ":" << minute << ":" << second << kendl;
    
    // 请求确认
    if (!confirm_dangerous_operation("This will change the system time!", "YES")) {
        bsp_kout << "Operation cancelled." << kendl;
        return success;
    }
    
    // 构造 EFI_TIME 结构
    EFI_TIME new_time;
    memset(&new_time, 0, sizeof(EFI_TIME));
    new_time.Year = (uint16_t)year;
    new_time.Month = (uint8_t)month;
    new_time.Day = (uint8_t)day;
    new_time.Hour = (uint8_t)hour;
    new_time.Minute = (uint8_t)minute;
    new_time.Second = (uint8_t)second;
    new_time.Nanosecond = 0;
    new_time.TimeZone = EFI_UNSPECIFIED_TIMEZONE;
    new_time.Daylight = 0;
    
    // 调用 UEFI 运行时服务设置时间
    EFI_STATUS status = EFI_RT_SVS::rt_time_set(new_time);
    
    if (status != EFI_SUCCESS) {
        bsp_kout << "Error: Failed to set UEFI time (status: 0x" 
                 << (uint64_t)status << ")" << kendl;
        return fail;
    }
    
    bsp_kout << "UEFI time set successfully." << kendl;
    bsp_kout << kendl;
    
    return success;
}

/**
 * @brief uefireboot - 热重启命令
 * 
 * 需要确认词 REBOOT
 */
KURD_t cmd_uefireboot(const line_t* line) {
    KURD_t success;
    KURD_t fail;
    
    // 请求确认
    if (!confirm_dangerous_operation(
            "System will perform warm reset! All unsaved data will be lost.",
            "REBOOT")) {
        bsp_kout << "Operation cancelled." << kendl;
        return success;
    }
    
    bsp_kout << "Performing warm reset..." << kendl;
    
    // 调用 UEFI 热重启
    EFI_RT_SVS::rt_reset(EfiResetWarm, EFI_SUCCESS, 0, nullptr);
    
    // 如果重启失败，返回错误
    return fail;
}

/**
 * @brief ueficreset - 冷重启命令
 * 
 * 需要确认词 REBOOT
 */
KURD_t cmd_ueficreset(const line_t* line) {
    KURD_t success;
    KURD_t fail;
    
    // 请求确认
    if (!confirm_dangerous_operation(
            "System will perform cold reset! All unsaved data will be lost.",
            "REBOOT")) {
        bsp_kout << "Operation cancelled." << kendl;
        return success;
    }
    
    bsp_kout << "Performing cold reset..." << kendl;
    
    // 调用 UEFI 冷重启
    EFI_RT_SVS::rt_reset(EfiResetCold, EFI_SUCCESS, 0, nullptr);
    
    // 如果重启失败，返回错误
    return fail;
}

/**
 * @brief uefishutdown - 关机命令
 * 
 * 需要确认词 SHUTDOWN
 */
KURD_t cmd_uefishutdown(const line_t* line) {
    KURD_t success;
    KURD_t fail;
    
    // 请求确认
    if (!confirm_dangerous_operation(
            "System will shut down! All unsaved data will be lost.",
            "SHUTDOWN")) {
        bsp_kout << "Operation cancelled." << kendl;
        return success;
    }
    
    bsp_kout << "Shutting down..." << kendl;
    
    // 调用 UEFI 关机
    EFI_RT_SVS::rt_reset(EfiResetShutdown, EFI_SUCCESS, 0, nullptr);
    
    // 如果关机失败，返回错误
    return fail;
}
