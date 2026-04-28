#pragma once
#include "util/kshell_commands.h"
#include "util/Ktemplats.h"
#include "abi/os_error_definitions.h"
namespace INFR_LOCATIONS{
    constexpr uint8_t KSHELL = 0x01;
    namespace KSHELL_EVENTS{
        namespace COMMON_FAIL_REASONS{
            constexpr uint16_t SHELL_NOT_INITIALIZED = 0x0001;
            constexpr uint16_t INVALID_PARAMETER = 0x0002;
            constexpr uint16_t NOT_FOUND = 0x0003;
        };
        constexpr uint8_t COMMAND_INIT = 0x0;
        constexpr uint8_t COMMAND_REGISTER = 0x01;
        namespace COMMAND_REGISTER_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t INSERT_RBTREE_FAIL = 0x0004;
            }
        }
        constexpr uint8_t COMMAND_UNREGISTER = 0x02;
        constexpr uint8_t COMMAND_EXECUTE = 0x03;
        constexpr uint8_t COMMAND_FIND = 0x04;
        constexpr uint8_t COMMAND_PARSE = 0x05;
        constexpr uint8_t COMMAND_HELP = 0x06;
        constexpr uint8_t SHELL_LOOP = 0x07;

    }
};
/**
 * @brief kshell 内核 Shell 核心框架
 * 
 * 设计原则：
 * 1. 零动态分配：所有内存均为静态或调用方提供
 * 2. 线程安全：命令注册/注销需要外部同步保护
 * 3. 模块化：各子系统自行注册命令
 */
int command_compare(const command_entry_t& a, const command_entry_t& b);

class kshell_framework_t {
public:
    /**
     * @brief 获取单例实例
     * @return kshell_framework_t& 单例引用
     */
    static kshell_framework_t& get_instance();

    /**
     * @brief 初始化 Shell 框架
     * @return KURD_t 错误码
     */
    KURD_t initialize();

    /**
     * @brief 注册命令
     * 
     * @param cmd_entry 命令入口指针（调用方保证生命周期）
     * @return KURD_t 错误码
     * 
     * @note 命令名称必须唯一，重复注册将失败
     * @note cmd_entry 必须在整个系统运行期间保持有效
     */
    KURD_t command_register(command_entry_t* cmd_entry);

    /**
     * @brief 注销命令
     * 
     * @param cmd_name 命令名称
     * @return KURD_t 错误码
     */
    KURD_t command_unregister(const char* cmd_name);

    /**
     * @brief 查找命令
     * 
     * @param cmd_name 命令名称
     * @return command_entry_t* 命令入口指针，未找到返回 nullptr
     */
    command_entry_t* command_find(const char* cmd_name);

    /**
     * @brief 执行命令
     * 
     * @param line 解析后的命令行数据
     * @return KURD_t 错误码
     * 
     * @note 如果命令为 DANGEROUS 级别，会自动进行确认流程
     */
    KURD_t command_execute(const line_t* line);

    /**
     * @brief 解析命令行输入
     * 
     * @param input 原始输入字符串
     * @param line 输出的解析结果
     * @return KURD_t 错误码
     */
    KURD_t parse_line(const char* input, line_t* line);

    /**
     * @brief 运行 Shell 主循环
     * 
     * @note 此函数会阻塞，通过 i8042 键盘驱动获取输入
     * @note 按 Ctrl+C 或输入 'exit' 可退出循环
     */
    void run_shell_loop();

    /**
     * @brief 显示帮助信息
     * 
     * @return KURD_t 错误码
     */
    KURD_t show_help();

private:
    KURD_t default_kurd=KURD_t(result_code::SUCCESS,0,module_code::INFRA,INFR_LOCATIONS::KSHELL,0,level_code::INFO,err_domain::CORE_MODULE);
    KURD_t default_success=default_kurd;
    KURD_t default_fail=set_result_fail_and_error_level(default_kurd);
    KURD_t default_fatal=set_fatal_result_level(default_kurd);
    kshell_framework_t();
    ~kshell_framework_t();

    // 禁止拷贝和移动
    kshell_framework_t(const kshell_framework_t&) = delete;
    kshell_framework_t& operator=(const kshell_framework_t&) = delete;

    
    /**
     * @brief 危险命令确认流程
     * 
     * @param cmd_entry 命令入口
     * @return true 用户确认执行
     * @return false 用户取消执行
     */
    bool confirm_dangerous_command(const command_entry_t* cmd_entry);

    /**
     * @brief 从键盘读取一行输入
     * 
     * @param buffer 输出缓冲区
     * @param max_len 最大长度
     * @return size_t 实际读取长度
     */
    size_t read_line_from_keyboard(char* buffer, size_t max_len);

    /**
     * @brief 打印 Shell 提示符
     */
    void print_prompt();

private:
    Ktemplats::RBTree<command_entry_t, command_compare> m_command_tree;  // 命令红黑树
    bool m_initialized;                // 是否已初始化
    static kshell_framework_t* s_instance;  // 单例指针
};
