#include "util/kshell.h"
#include "util/kout.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "util/OS_utils.h"
#include "util/kshell.h"

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

// ============================================================
// 行编辑器状态与辅助函数
// ============================================================
namespace {

struct line_editor_t {
    char*   line;
    size_t  cap;
    size_t  len;
    size_t  cursor;
};

static void redraw_line(line_editor_t* ed) {
    bsp_kout << "\rkshell> ";
    for (size_t i = 0; i < ed->len; i++)
        bsp_kout << ed->line[i];

    constexpr size_t LINE_CLEAR = 256;
    size_t total = 8 + ed->len;
    for (size_t i = total; i < LINE_CLEAR; i++)
        bsp_kout << ' ';

    bsp_kout << "\rkshell> ";
    for (size_t i = 0; i < ed->len; i++)
        bsp_kout << ed->line[i];
    for (size_t i = ed->cursor; i < ed->len; i++)
        bsp_kout << '\b';
}

static void sync_cursor(line_editor_t* ed) {
    size_t cur = ed->len;
    size_t target = ed->cursor;
    if (target < cur) {
        for (size_t i = target; i < cur; i++)
            bsp_kout << '\b';
    }
}

// ---- 历史管理 ----
static constexpr size_t HISTORY_MAX   = 64;
static constexpr size_t HISTORY_LEN   = 256;
static char history_pool[HISTORY_MAX][HISTORY_LEN];
static size_t history_count   = 0;
static size_t history_write   = 0;
static size_t history_browse  = 0;
static bool   browsing        = false;

static void add_to_history(const char* line, size_t len) {
    if (len == 0 || len >= HISTORY_LEN) return;
    size_t last = (history_write == 0) ? HISTORY_MAX - 1 : history_write - 1;
    if (history_count > 0 && strcmp_in_kernel(history_pool[last], line) == 0)
        return;
    ksystemramcpy((void*)line, history_pool[history_write], len);
    history_pool[history_write][len] = '\0';
    history_write = (history_write + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
    browsing = false;
}

static void history_navigate(line_editor_t* ed, int direction) {
    if (history_count == 0) return;

    if (!browsing) {
        browsing = true;
        history_browse = history_write;
    }

    if (direction < 0) {
        if (history_browse == 0)
            history_browse = HISTORY_MAX - 1;
        else
            history_browse--;
    } else {
        history_browse++;
        if (history_browse >= HISTORY_MAX)
            history_browse = 0;
    }

    const char* src = history_pool[history_browse];
    size_t slen = strlen_in_kernel(src);
    if (slen >= ed->cap) slen = ed->cap - 1;
    ksystemramcpy((void*)src, ed->line, slen);
    ed->len = slen;
    ed->cursor = slen;
    ed->line[slen] = '\0';
    redraw_line(ed);
}

// ---- Tab 补全 ----
static void try_autocomplete(line_editor_t* ed) {
    if (ed->len == 0) return;

    char prefix[256];
    size_t prefix_len = ed->len;
    if (prefix_len > 255) prefix_len = 255;
    ksystemramcpy((void*)ed->line, prefix, prefix_len);
    prefix[prefix_len] = '\0';

    command_entry_t* matches[64];
    int count = kshell_framework_t::command_find_prefix(prefix, matches, 64);
    if (count <= 0) {
        bsp_kout << '\a';
        return;
    }

    if (count == 1) {
        const char* cmd_name = matches[0]->name;
        size_t name_len = strlen_in_kernel(cmd_name);
        if (name_len >= ed->cap) name_len = ed->cap - 1;
        ksystemramcpy((void*)cmd_name, ed->line, name_len);
        ed->len = name_len;
        ed->cursor = name_len;
        ed->line[name_len] = '\0';
        redraw_line(ed);
        return;
    }

    char common[256];
    const char* first = matches[0]->name;
    size_t common_len = strlen_in_kernel(first);
    if (common_len > 255) common_len = 255;
    ksystemramcpy((void*)first, common, common_len);
    for (int i = 1; i < count; i++) {
        const char* m = matches[i]->name;
        size_t j = 0;
        while (j < common_len && common[j] == m[j]) j++;
        common_len = j;
        if (common_len == 0) break;
    }

    if (common_len > prefix_len) {
        ksystemramcpy((void*)common, ed->line, common_len);
        ed->len = common_len;
        ed->cursor = common_len;
        ed->line[common_len] = '\0';
        redraw_line(ed);
        return;
    }

    bsp_kout << kendl;
    for (int i = 0; i < count; i++) {
        if (i > 0) bsp_kout << "  ";
        bsp_kout << matches[i]->name;
    }
    bsp_kout << kendl;
    redraw_line(ed);
}

} // anonymous namespace

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
 * @brief 查找所有以 prefix 开头的命令
 */
int kshell_framework_t::command_find_prefix(
    const char* prefix,
    command_entry_t** out_matches,
    int max_matches)
{
    if (!prefix || !out_matches || max_matches <= 0 || !m_command_tree)
        return -1;

    int count = 0;
    size_t prefix_len = strlen_in_kernel(prefix);
    if (prefix_len == 0) return 0;

    auto it = m_command_tree->begin();
    auto end = m_command_tree->end();

    for (; it != end; ++it) {
        int cmp = strcmp_in_kernel(it->name, prefix);
        if (cmp >= 0) break;
    }

    for (; it != end; ++it) {
        if (strncmp_in_kernel(it->name, prefix, prefix_len) != 0)
            break;
        if (count < max_matches)
            out_matches[count++] = &(*it);
    }
    return count;
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
 * @brief 检测 text_input 管线是否可用
 */
bool kshell_framework_t::is_text_input_available() {
    return text_input_ring_readonly_view != nullptr
        || text_input_get_publish_seq() > 0;
}

/**
 * @brief 降级路径：保留原来的 i8042_blockable_keyboard_listening 方式
 */
size_t kshell_framework_t::read_line_fallback(char* buffer, size_t max_len) {
    if (!buffer || max_len == 0) return 0;

    size_t pos = 0;
    while (pos < max_len - 1) {
        buff_t buf;
        i8042_blockable_keyboard_listening(&buf);

        for(uint16_t i = 0; i < buf.len && pos < max_len - 1; i++) {
            char c = buf.data[i];
            if (c == '\r' || c == '\n') {
                bsp_kout << kendl;
                buffer[pos] = '\0';
                return pos;
            }
            if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    bsp_kout << "\b \b";
                }
                continue;
            }
            if (c == 3) {
                bsp_kout << "^C" << kendl;
                return 0;
            }
            bsp_kout << c;
            buffer[pos] = c;
            pos++;
        }
    }
    buffer[pos] = '\0';
    return pos;
}

/**
 * @brief 从键盘读取一行输入（基于 text_input_event）
 */
size_t kshell_framework_t::read_line_from_keyboard(char* buffer, size_t max_len) {
    if (!buffer || max_len == 0) return 0;

    if (!is_text_input_available())
        return read_line_fallback(buffer, max_len);

    line_editor_t ed;
    ed.line   = buffer;
    ed.cap    = max_len;
    ed.len    = 0;
    ed.cursor = 0;
    buffer[0] = '\0';

    static uint64_t read_seq = 0;
    if (read_seq == 0)
        read_seq = text_input_get_publish_seq();

    while (true) {
        text_input_event batch[16];
        uint32_t n = text_input_batch_read(read_seq, batch, 16);
        if (n == 0) {
            text_input_wait_event(read_seq);
            continue;
        }
        read_seq += n;

        for (uint32_t i = 0; i < n; i++) {
            const text_input_event& ev = batch[i];
            bool finished = false;

            switch (ev.event_type) {
            case 0: {
                char ch = static_cast<char>(ev.data & 0xFF);
                switch (ch) {
                case '\n':
                    ed.line[ed.len] = '\0';
                    finished = true;
                    break;
                case '\b':
                    if (ed.cursor > 0) {
                        size_t move = ed.len - ed.cursor;
                        ksystemramcpy((void*)(ed.line + ed.cursor),
                                      ed.line + ed.cursor - 1,
                                      move);
                        ed.cursor--;
                        ed.len--;
                        redraw_line(&ed);
                    }
                    break;
                default:
                    if (ed.len < ed.cap - 1) {
                        size_t move = ed.len - ed.cursor;
                        if (move > 0) {
                            // 光标处及后续字符右移，腾出插入位置
                            ksystemramcpy((void*)(ed.line + ed.cursor),
                                          (void*)(ed.line + ed.cursor + 1),
                                          move);
                        }
                        ed.line[ed.cursor] = ch;
                        ed.cursor++;
                        ed.len++;
                        redraw_line(&ed);
                    }
                    break;
                }
                break;
            }
            case 1:
                switch (ev.data) {
                case TEXT_CTRL_ENTER:
                    ed.line[ed.len] = '\0';
                    bsp_kout << kendl;
                    finished = true;
                    break;
                case TEXT_CTRL_LEFT:
                    if (ed.cursor > 0) { ed.cursor--; sync_cursor(&ed); }
                    break;
                case TEXT_CTRL_RIGHT:
                    if (ed.cursor < ed.len) { ed.cursor++; sync_cursor(&ed); }
                    break;
                case TEXT_CTRL_HOME:
                    ed.cursor = 0;
                    sync_cursor(&ed);
                    break;
                case TEXT_CTRL_END:
                    ed.cursor = ed.len;
                    sync_cursor(&ed);
                    break;
                case TEXT_CTRL_DELETE:
                    if (ed.cursor < ed.len) {
                        size_t move = ed.len - ed.cursor - 1;
                        // 光标后字符左移，覆盖光标处
                        ksystemramcpy((void*)(ed.line + ed.cursor + 1),
                                      (void*)(ed.line + ed.cursor),
                                      move);
                        ed.len--;
                        redraw_line(&ed);
                    }
                    break;
                case TEXT_CTRL_BACKSPACE:
                    if (ed.cursor > 0) {
                        size_t move = ed.len - ed.cursor;
                        // 光标处及后续字符左移，覆盖光标前一格
                        ksystemramcpy((void*)(ed.line + ed.cursor),
                                      (void*)(ed.line + ed.cursor - 1),
                                      move);
                        ed.cursor--;
                        ed.len--;
                        redraw_line(&ed);
                    }
                    break;
                case TEXT_CTRL_UP:
                    history_navigate(&ed, -1);
                    break;
                case TEXT_CTRL_DOWN:
                    history_navigate(&ed, +1);
                    break;
                case TEXT_CTRL_ESCAPE:
                    ed.len = 0;
                    ed.cursor = 0;
                    redraw_line(&ed);
                    break;
                case TEXT_CTRL_TAB:
                    try_autocomplete(&ed);
                    break;
                default:
                    break;
                }
                break;
            }

            if (finished) {
                add_to_history(ed.line, ed.len);
                return ed.len;
            }
        }
    }
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
        size_t len = read_line_from_keyboard(input_buf, 4095);
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

        // 帮助命令
        if (strcmp_in_kernel(parsed_line->tokens[0].str, "help") == 0 &&
            parsed_line->tokens[0].len == 4) {
            show_help();
            bsp_kout << kendl;
            continue;
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
