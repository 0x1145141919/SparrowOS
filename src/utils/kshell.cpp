#include "util/kshell.h"
#include "util/kout.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "util/OS_utils.h"
#include "util/kshell.h"

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

/**
 * @brief 判断字符是否为数字
 */
static inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/**
 * @brief 判断字符是否为十六进制数字
 */
static inline bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * @brief 识别 token 类型并解析数值
 *
 * 同时设置 token.type 和 token.num_value，
 * 这样命令处理器可直接使用 token_to_uint64()/token_to_int64() 读取。
 */
static void classify_and_parse_token(token_t& token) {
    if (token.len == 0 || !token.str) {
        token.type = TOKEN_TYPE_NONE;
        token.num_value = 0;
        return;
    }

    const char* str = token.str;

    // 检查二进制数字: 0b 或 0B 开头
    if (token.len >= 3 && str[0] == '0' && (str[1] == 'b' || str[1] == 'B')) {
        bool valid = true;
        uint64_t value = 0;
        for (size_t i = 2; i < token.len; i++) {
            if (str[i] != '0' && str[i] != '1') { valid = false; break; }
            value = (value << 1) | (str[i] - '0');
        }
        if (valid) {
            token.type = TOKEN_TYPE_NUM_BIN;
            token.num_value = static_cast<int64_t>(value);
            return;
        }
        token.type = TOKEN_TYPE_STRING;
        token.num_value = 0;
        return;
    }

    // 检查十六进制数字: 0x 或 0X 开头
    if (token.len >= 3 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        bool valid = true;
        uint64_t value = 0;
        for (size_t i = 2; i < token.len; i++) {
            char c = str[i];
            if (!is_hex_digit(c)) { valid = false; break; }
            value = (value << 4);
            if (c >= '0' && c <= '9')       value |= (c - '0');
            else if (c >= 'a' && c <= 'f')  value |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')  value |= (c - 'A' + 10);
        }
        if (valid) {
            token.type = TOKEN_TYPE_NUM_HEX;
            token.num_value = static_cast<int64_t>(value);
            return;
        }
        token.type = TOKEN_TYPE_STRING;
        token.num_value = 0;
        return;
    }

    // 检查纯十进制数字 (可选的前导负号)
    size_t start_idx = 0;
    bool negative = false;
    if (str[0] == '-' && token.len > 1) {
        negative = true;
        start_idx = 1;
    }

    bool has_digit = false;
    bool valid = true;
    uint64_t value = 0;
    for (size_t i = start_idx; i < token.len; i++) {
        if (!is_digit(str[i])) { valid = false; break; }
        value = value * 10 + (str[i] - '0');
        has_digit = true;
    }

    if (valid && has_digit) {
        token.type = TOKEN_TYPE_NUM_DEC;
        token.num_value = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
        return;
    }

    // 默认为字符串类型
    token.type = TOKEN_TYPE_STRING;
    token.num_value = 0;
}

/**
 * @brief 判断 token 是否等于指定字符串
 * @param token 要比较的 token
 * @param str 要比较的字符串
 * @return true 如果相等
 */
bool token_equals(const token_t& token, const char* str) {
    if (!token.str || !str) return false;
    size_t n = strlen_in_kernel(str);
    if (token.len != n) return false;
    return strcmp_in_kernel(token.str, str, n) == 0;
}

/**
 * @brief 从 token 获取整数值（自动识别进制）
 * @param token 要解析的 token
 * @param out_value 输出参数，存储解析后的值
 * @return true 如果解析成功
 */
bool token_to_int64(const token_t& token, int64_t* out_value) {
    if (!out_value) return false;

    if (token.type == TOKEN_TYPE_NUM_BIN ||
        token.type == TOKEN_TYPE_NUM_HEX ||
        token.type == TOKEN_TYPE_NUM_DEC) {
        *out_value = token.num_value;
        return true;
    }

    return false;
}

/**
 * @brief 从 token 获取无符号整数值（自动识别进制）
 * @param token 要解析的 token
 * @param out_value 输出参数，存储解析后的值
 * @return true 如果解析成功
 */
bool token_to_uint64(const token_t& token, uint64_t* out_value) {
    if (!out_value) return false;

    if (token.type == TOKEN_TYPE_NUM_BIN ||
        token.type == TOKEN_TYPE_NUM_HEX ||
        token.type == TOKEN_TYPE_NUM_DEC) {
        *out_value = static_cast<uint64_t>(token.num_value);
        return true;
    }

    return false;
}

Ktemplats::RBTree<command_entry_t, command_compare>*kshell_framework_t::m_command_tree;
bool kshell_framework_t::m_initialized;
int command_compare(const command_entry_t &a, const command_entry_t &b)
{
    return strcmp_in_kernel(a.name, b.name);
}
KURD_t kshell_framework_t::default_kurd()
{
    return KURD_t(result_code::SUCCESS,0,module_code::INFRA,INFR_LOCATIONS::KSHELL,0,level_code::INFO,err_domain::CORE_MODULE);
}
KURD_t kshell_framework_t::default_success()
{
    return default_kurd();
}
KURD_t kshell_framework_t::default_fail()
{
    return set_result_fail_and_error_level(default_kurd());
}
KURD_t kshell_framework_t::default_fatal()
{
    return set_fatal_result_level(default_kurd());
}
void* kshell_thread_main(void*arg){
    bsp_kout << "[KSHELL] Kernel shell started" << kendl;
    kshell_framework_t::run_shell_loop();
}
/**
 * @brief 初始化 Shell 框架
 */
KURD_t kshell_framework_t::Init() {
    KURD_t fail=default_fail();
    KURD_t success=default_success();
    success.event_code=INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_INIT;
    fail.event_code=INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_INIT;

    if (m_initialized) {
        return success;  // 已初始化，返回成功
    }
    m_command_tree = new Ktemplats::RBTree<command_entry_t, command_compare>();
    KURD_t kurd=initial_commands_regist();
    if(error_kurd(kurd))return kurd;
    m_initialized = true;
    uint64_t tid=create_kthread(kshell_thread_main, nullptr,&kurd);
    return success;
}

/**
 * @brief 注册命令
 */
KURD_t kshell_framework_t::command_register(command_entry_t* cmd_entry) {
    KURD_t fail = default_fail();
    KURD_t success = default_success();
    using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_REGISTER_RESULTS;
    fail.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_REGISTER;
    success.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_REGISTER;

    if (!cmd_entry || !cmd_entry->name || !cmd_entry->handler) {
        fail.reason = INVALID_PARAMETER;
        return fail;
    }

    // 尝试插入红黑树
    bool insert_result = m_command_tree->insert(*cmd_entry);
    if (!insert_result) {
        bsp_kout << "[KSHELL] Failed to register command: " << cmd_entry->name
                 << " (may already exist)" << kendl;
        fail.reason = FAIL_REASONS::INSERT_RBTREE_FAIL;
        return fail;
    }

    bsp_kout << "[KSHELL] Command registered: " << cmd_entry->name << kendl;
    return success;
}

/**
 * @brief 注销命令
 */
KURD_t kshell_framework_t::command_unregister(const char* cmd_name) {
    KURD_t fail = default_fail();
    KURD_t success = default_success();
    fail.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_UNREGISTER;
    success.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_UNREGISTER;

    if (!cmd_name ) {
        fail.reason = INVALID_PARAMETER;
        return fail;
    }
    if(!m_initialized){
        fail.reason=SHELL_NOT_INITIALIZED;
        return fail;
    }

    // 查找命令
    command_entry_t dummy_entry;
    dummy_entry.name = cmd_name;
    dummy_entry.description = nullptr;
    dummy_entry.handler = nullptr;
    dummy_entry.risk = command_risk_level_t::SAFE;
    dummy_entry.need_confirm = false;


    // 注意：RBTree 的 erase 需要完整的对象来比较
    // 这里简化处理，实际可能需要遍历查找后删除
    bool erase_result = m_command_tree->erase(dummy_entry);
    if (!erase_result) {
        fail.reason = NOT_FOUND;
        return fail;
    }

    bsp_kout << "[KSHELL] Command unregistered: " << cmd_name << kendl;
    return success;
}

/**
 * @brief 查找命令
 */
command_entry_t* kshell_framework_t::command_find(const char* cmd_name) {
    if (!cmd_name || !m_initialized) {
        return nullptr;
    }

    // 构造临时对象用于查找
    command_entry_t dummy_entry;
    dummy_entry.name = cmd_name;
    dummy_entry.description = nullptr;
    dummy_entry.handler = nullptr;
    dummy_entry.risk = command_risk_level_t::SAFE;
    dummy_entry.need_confirm = false;

    // 刻意构造一个对应名字的command_entry_t调用find_node接口
    command_entry_t* node = m_command_tree->find(dummy_entry);
    return node;
}

/**
 * @brief 执行命令
 */
KURD_t kshell_framework_t::command_execute(const line_t* line) {
    KURD_t fail = default_fail();
    KURD_t success = default_success();
    fail.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;
    success.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (!line || line->token_count == 0) {
        fail.reason = INVALID_PARAMETER;
        return fail;
    }

    // 获取命令名（第一个 token）
    const char* cmd_name = line->tokens[0].str;

    // 查找命令
    command_entry_t* cmd = command_find(cmd_name);
    if (!cmd) {
        bsp_kout << "[KSHELL] Unknown command: " << cmd_name << kendl;
        fail.reason = INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS::NOT_FOUND;
        return fail;
    }

    // 危险等级检查
    if (cmd->risk == command_risk_level_t::DANGEROUS && cmd->need_confirm) {
        if (!confirm_dangerous_command(cmd)) {
            bsp_kout << "[KSHELL] Command cancelled by user" << kendl;
            return success;  // 用户取消，返回成功
        }
    } else if (cmd->risk == command_risk_level_t::WARNING) {
        bsp_kout << "[KSHELL] WARNING: This command may have side effects!" << kendl;
    }

    // 执行命令
    KURD_t handler_result = cmd->handler(line);
    return handler_result;
}

/**
 * @brief 解析命令行输入
 */
KURD_t kshell_framework_t::parse_line(const char* input, line_t* line) {
    KURD_t fail = default_fail();
    KURD_t success = default_success();
    fail.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_PARSE;
    success.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_PARSE;

    if (!input || !line) {
        fail.reason = INVALID_PARAMETER;
        return fail;
    }

    // 清空输出结构
    ksetmem_8((void*)line, 0, sizeof(line_t));

    // 复制原始输入
    size_t input_len = strlen_in_kernel(input);
    if (input_len >= sizeof(line->raw_buffer)) {
        input_len = sizeof(line->raw_buffer) - 1;
    }
    ksystemramcpy((void*)input, (void*)line->raw_buffer, input_len);
    line->raw_buffer[input_len] = '\0';
    line->raw_length = input_len;

    // 跳过前导空格
    const char* ptr = line->raw_buffer;
    while (*ptr == ' ' || *ptr == '\t') {
        ptr++;
    }

    // 空行检查
    if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') {
        line->token_count = 0;
        return success;  // 空行，返回成功但无 token
    }

    // 分词
    line->token_count = 0;
    while (*ptr != '\0' && line->token_count < 256) {
        // 跳过空格
        while (*ptr == ' ' || *ptr == '\t') {
            ptr++;
        }

        if (*ptr == '\0') break;

        // 记录 token 起始位置
        line->tokens[line->token_count].str = ptr;

        // 找到 token 结束位置
        while (*ptr != '\0' && *ptr != ' ' && *ptr != '\t') {
            ptr++;
        }

        // 计算 token 长度
        line->tokens[line->token_count].len = ptr - line->tokens[line->token_count].str;

        // 如果后面还有字符，将分隔符替换为 '\0' 以终止当前 token
        if (*ptr != '\0') {
            char* token_end = const_cast<char*>(ptr);
            *token_end = '\0';
            ptr++; // 移动到下一个字符继续处理
        }

        // 识别 token 类型并解析数值（一步完成）
        classify_and_parse_token(line->tokens[line->token_count]);

        line->token_count++;
    }

    return success;
}

/**
 * @brief 显示帮助信息
 */
KURD_t kshell_framework_t::show_help() {
    KURD_t fail = default_fail();
    KURD_t success = default_success();
    fail.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_HELP;
    success.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_HELP;

    if (!m_initialized) {
        fail.reason = SHELL_NOT_INITIALIZED;
        return fail;
    }

    bsp_kout << kendl;
    bsp_kout << "Available Commands:" << kendl;
    bsp_kout << "-------------------" << kendl;

    // 遍历红黑树输出所有命令（按名称排序）
    if (m_command_tree->empty()) {
        bsp_kout << "(No commands registered)" << kendl;
    } else {
        for (auto it = m_command_tree->begin(); it != m_command_tree->end(); ++it) {
            const command_entry_t& cmd = *it;

            // 输出命令名称和描述
            bsp_kout << "  " << cmd.name;

            // 对齐描述
            size_t name_len = strlen_in_kernel(cmd.name);
            for (size_t i = 0; i < (20 - name_len); ++i) {
                bsp_kout << " ";
            }

            bsp_kout << cmd.description;

            // 标记危险等级
            if (cmd.risk == command_risk_level_t::WARNING) {
                bsp_kout << " [WARNING]";
            } else if (cmd.risk == command_risk_level_t::DANGEROUS) {
                bsp_kout << " [DANGEROUS]";
            }

            bsp_kout << kendl;
        }
    }

    bsp_kout << kendl;
    bsp_kout << "Use 'help <command>' for more information about a specific command." << kendl;
    bsp_kout << kendl;

    return success;
}

/**
 * @brief 危险命令确认流程
 */
bool kshell_framework_t::confirm_dangerous_command(const command_entry_t* cmd_entry) {
    if (!cmd_entry) return false;

    bsp_kout << "[KSHELL] DANGEROUS COMMAND: " << cmd_entry->name << kendl;
    bsp_kout << "[KSHELL] Description: " << cmd_entry->description << kendl;
    bsp_kout << "[KSHELL] Are you sure? Type 'yes' to confirm: ";

    char confirm_buf[64];
    size_t len = read_line_from_keyboard(confirm_buf, sizeof(confirm_buf) - 1);
    confirm_buf[len] = '\0';

    return (strcmp_in_kernel(confirm_buf, "yes") == 0);
}

/**
 * @brief 从键盘读取一行输入
 */
size_t kshell_framework_t::read_line_from_keyboard(char* buffer, size_t max_len) {
    if (!buffer || max_len == 0) return 0;

    size_t pos = 0;

    while (pos < max_len - 1) {
        // 阻塞式读取单个字符

        char c = i8042_blockable_keyboard_listening();

        // 回车或换行表示输入结束
        if (c == '\r' || c == '\n') {
            bsp_kout << kendl;  // 输出换行
            break;
        }

        // 退格处理
        if (c == '\b' || c == 127) {  // 127 是 DEL
            if (pos > 0) {
                pos--;
                bsp_kout << "\b \b";  // 回退、空格、再回退
            }
            continue;
        }

        // Ctrl+C 中断
        if (c == 3) {  // Ctrl+C
            bsp_kout << "^C" << kendl;
            return 0;
        }

        // 正常字符，显示回显
        bsp_kout << c;
        buffer[pos] = c;
        pos++;
    }

    buffer[pos] = '\0';
    return pos;
}

/**
 * @brief 打印 Shell 提示符
 */
void kshell_framework_t::print_prompt() {
    bsp_kout << "kshell> ";
}

/**
 * @brief 运行 Shell 主循环
 */
void kshell_framework_t::run_shell_loop() {
    if (!m_initialized) {
        bsp_kout << "[KSHELL] Error: Framework not initialized!" << kendl;
        return;
    }

    bsp_kout << kendl;
    bsp_kout << "========================================" << kendl;
    bsp_kout << "  Kernel Shell (kshell) v1.0" << kendl;
    bsp_kout << "  Type 'help' for available commands" << kendl;
    bsp_kout << "========================================" << kendl << kendl;

    char*input_buf=new char[4096];
    line_t*parsed_line=new line_t;

    while (true) {
        print_prompt();

        // 读取输入
        size_t len = read_line_from_keyboard(input_buf, sizeof(input_buf) - 1);
        if (len == 0) {
            continue;  // Ctrl+C 或空输入
        }

        // 解析命令行
        KURD_t parse_result = parse_line(input_buf, parsed_line);
        if (parse_result.result != 0) {
            bsp_kout << "[KSHELL] Parse error" << kendl;
            continue;
        }

        // 空行跳过
        if (parsed_line->token_count == 0) {
            continue;
        }

        // 退出命令
        if (strcmp_in_kernel(parsed_line->tokens[0].str, "exit") == 0 &&
            parsed_line->tokens[0].len == 4) {
            bsp_kout << "[KSHELL] Exiting shell..." << kendl;
            break;
        }

        // 执行命令
        KURD_t exec_result = command_execute(parsed_line);
        if (exec_result.result != 0) {
            bsp_kout << "[KSHELL] Command execution failed (result="
                     << exec_result.result << ", reason=" << exec_result.reason << ")" << kendl;
        }

        bsp_kout << kendl;
    }
}
