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
enum token_type_t {
    TOKEN_TYPE_NONE = 0,
    TOKEN_TYPE_STRING,
    TOKEN_TYPE_NUM_BIN, //0B ，0b开头
    TOKEN_TYPE_NUM_DEC, //纯十进制数字
    TOKEN_TYPE_NUM_HEX, //0x开头
    TOKEN_TYPE_ARITHMETIC_EXPR, //算术表达式,复杂度太高暂且不支持，当作字符串
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
    token_type_t type;    // 词法单元类型
    int64_t num_value;    // 如果是数字类型，存储解析后的数值
};

/**
 * @brief 解析后的命令行数据结构
 * 
 * 所有数据均为静态分配，零动态内存分配
 */
struct line_t {
    char raw_buffer[4096];        // 原始输入缓冲区
    token_t tokens[256];           // 词法单元数组（最大256个）
    size_t token_count;           // 实际词法单元数量
    size_t raw_length;            // 原始输入长度
};

/**
 * @brief Token 辅助函数声明
 *
 * 数值 token 的解析由 parse_line 在分词阶段自动完成，
 * 结果存储在 token.num_value 中。命令处理器通过以下
 * 辅助函数直接读取，无需手写解析器。
 */

/**
 * @brief 判断 token 是否等于指定字符串
 * @param token 要比较的 token
 * @param str 要比较的字符串
 * @return true 如果相等
 */
bool token_equals(const token_t& token, const char* str);

/**
 * @brief 从 token 获取整数值（自动识别进制）
 * @param token 要解析的 token
 * @param out_value 输出参数，存储解析后的值
 * @return true 如果解析成功
 */
bool token_to_int64(const token_t& token, int64_t* out_value);

/**
 * @brief 从 token 获取无符号整数值（自动识别进制）
 * @param token 要解析的 token
 * @param out_value 输出参数，存储解析后的值
 * @return true 如果解析成功
 */
bool token_to_uint64(const token_t& token, uint64_t* out_value);
