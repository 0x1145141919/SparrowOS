#pragma once
#include <stdint.h>
#include <stddef.h>

// 前向声明，避免循环依赖
struct line_t;

/**
 * @brief 命令危险等级
 */
enum class command_risk_level_t : uint8_t {
    SAFE = 0,        // 只读操作，无风险
    WARNING = 1,     // 需要注意的操作
    DANGEROUS = 2    // 危险操作，需要确认
};

/**
 * @brief 命令处理函数签名
 * @param line 解析后的命令行数据
 * @return KURD_t 错误码
 */
typedef struct KURD_t (*command_handler_t)(const line_t* line);

/**
 * @brief 命令入口结构
 * 
 * 注意：此结构由调用方提供并保证长期有效（通常为全局或静态变量）
 * 命令管理器不会复制此结构，仅保存指针
 */
struct command_entry_t {
    const char* name;              // 命令名称
    const char* description;       // 命令描述
    command_handler_t handler;     // 处理函数指针
    command_risk_level_t risk;     // 危险等级
    bool need_confirm;             // 是否需要用户确认（针对危险命令）
};

/**
 * @brief 词法单元结构
 */
struct token_t {
    const char* str;      // 字符串起始位置（指向原始缓冲区）
    size_t len;           // 字符串长度
};

/**
 * @brief 解析后的命令行数据结构
 * 
 * 所有数据均为静态分配，零动态内存分配
 */
struct line_t {
    char raw_buffer[4096];        // 原始输入缓冲区
    token_t tokens[64];           // 词法单元数组（最大64个）
    size_t token_count;           // 实际词法单元数量
    size_t raw_length;            // 原始输入长度
};

/**
 * @brief 数字解析结果
 */
struct parsed_number_t {
    uint64_t value;       // 解析后的数值
    bool is_valid;        // 是否解析成功
    bool is_negative;     // 是否为负数（仅对十进制有效）
};
