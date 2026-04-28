# kshell - 内核 Shell 通用框架

## 📖 概述

kshell 是一个运行在内核态的交互式 Shell，用于系统调试、状态查询和紧急干预。它通过 i8042 键盘驱动接收输入，通过 `bsp_kout` 输出到 GOP 帧缓冲或串口。

**核心特性**：
- ✅ 零动态内存分配（所有缓冲区静态/栈上分配）
- ✅ 模块化命令系统（易于扩展）
- ✅ 安全机制（危险操作确认、参数验证）
- ✅ 统一错误处理
- ✅ 完整的词法分析器

---

## 🏗️ 架构

```
┌─────────────────────────────────────┐
│       kshell_thread (内核线程)       │
├─────────────────────────────────────┤
│   命令分发器 (Command Dispatcher)    │
├─────────────────────────────────────┤
│   行解析器 (Line Parser)             │
│   ├── 词法分析 (Tokenizer)           │
│   └── 语法分析 (Parser)              │
├─────────────────────────────────────┤
│   输入层 (Input Layer)               │
│   └── i8042 键盘监听                 │
└─────────────────────────────────────┘
         ↓ 输出通过 bsp_kout
```

---

## 📁 文件结构

```
kernel/
├── Docs/
│   ├── kshell_framework_design.md      # 框架设计文档
│   ├── kshell_usage_guide.md           # 使用指南
│   └── kshell_*_commands_design.md     # 各子系统命令设计
├── src/
│   ├── include/util/
│   │   └── kshell.h                    # 公共头文件
│   └── utils/
│       └── kshell.cpp                  # 核心实现
```

---

## 🚀 快速开始

### 1. 启动 kshell

```cpp
#include "util/kshell.h"
#include "Scheduler/per_processor_scheduler.h"

void start_kshell() {
    KURD_t result;
    uint64_t tid = Scheduler::create_kthread(kshell_thread, nullptr, &result);
    
    if (tid == 0) {
        bsp_kout << "[ERROR] Failed to create kshell thread" << kendl;
    } else {
        bsp_kout << "[INFO] kshell started (TID: " << tid << ")" << kendl;
    }
}
```

### 2. 基本使用

```
========================================
  Kernel Shell (kshell) v0.1
  Type 'help' for available commands
========================================

kshell> help

=== Kernel Shell (kshell) ===
Available Commands:

  help            - 显示帮助信息
  meminfo         - 内存总体使用情况
  buddy           - 伙伴系统统计
  heap            - 内核堆使用情况

Type '<command>' to execute.

kshell> meminfo
[TODO] meminfo command not implemented yet

kshell> 
```

---

## 📝 添加新命令

### 三步走

#### 步骤 1: 实现命令处理器

```cpp
int cmd_mycommand(const kshell::line_t& line) {
    // 1. 检查参数
    if (line.word_count < 2) {
        bsp_kout << "[ERROR] Usage: mycommand <arg>" << kendl;
        return -2;
    }
    
    // 2. 提取参数
    const char* arg = line.get_word(1);
    
    // 3. 执行逻辑
    bsp_kout << "[INFO] Processing: " << arg << kendl;
    
    return 0;
}
```

#### 步骤 2: 前向声明

在 `kshell.cpp` 中添加：

```cpp
int cmd_mycommand(const line_t& line);
```

#### 步骤 3: 注册命令

在命令表中添加：

```cpp
static constexpr command_entry_t command_table[] = {
    {"help",       "显示帮助信息",          cmd_help,       RISK_SAFE, 0},
    // ... 其他命令 ...
    {"mycommand",  "我的自定义命令",        cmd_mycommand,  RISK_SAFE, 0},
};
```

---

## 🔧 核心 API

### 数据结构

#### `word_t` - 词法单元

```cpp
struct word_t {
    word_type type;       // BIN_NUM, DEC_NUM, HEX_NUM, STR
    uint16_t offset;      // 在行缓冲中的偏移
    uint16_t length;      // 长度
    
    uint64_t to_uint64(const char* line_base) const;
    const char* to_string(const char* line_base) const;
};
```

#### `line_t` - 行缓冲

```cpp
struct line_t {
    char base[MAX_LINE_LENGTH];     // 缓冲区
    uint16_t length;                // 实际长度
    uint16_t word_count;            // 词数
    word_t words[MAX_WORDS];        // 词数组
    
    void parse();                   // 词法分析
    const char* get_word(uint16_t idx) const;
    void clear();
    bool is_empty() const;
};
```

#### `command_entry_t` - 命令表项

```cpp
struct command_entry_t {
    const char* name;                    // 命令名
    const char* description;             // 描述
    command_handler_t handler;           // 处理函数
    risk_level risk;                     // 风险等级
    uint8_t requires_confirmation;       // 是否需确认
};
```

### 辅助函数

```cpp
namespace kshell {
    // 字符串操作（内核态安全版本）
    int strcmp_kernel(const char* s1, const char* s2);
    uint16_t strlen_kernel(const char* str);
    
    // 数值转换
    uint64_t hex_to_uint64(const char* str);
    uint64_t bin_to_uint64(const char* str);
    uint64_t dec_to_uint64(const char* str);
    
    // 命令查找与执行
    const command_entry_t* find_command(const char* name);
    int execute_command(const line_t& line);
}
```

---

## 🛡️ 安全机制

### 1. 风险等级

```cpp
enum risk_level : uint8_t {
    RISK_SAFE = 0,         // 只读操作
    RISK_WARNING = 1,      // 需谨慎
    RISK_DANGEROUS = 2     // 高风险
};
```

### 2. 确认机制

对 `requires_confirmation=1` 的命令，自动触发确认流程：

```
kshell> reboot
[WARNING] This is a dangerous operation!
Type 'yes' to confirm: yes
[执行重启]
```

### 3. 参数验证

始终验证参数数量和有效性：

```cpp
if (line.word_count < 2) {
    bsp_kout << "[ERROR] Missing argument" << kendl;
    return -2;
}
```

---

## 📊 错误处理

### 错误码约定

```cpp
enum kshell_error_codes : int {
    ERR_SUCCESS = 0,
    ERR_EMPTY_LINE = -1,
    ERR_UNKNOWN_COMMAND = -2,
    ERR_INVALID_ARGS = -3,
    ERR_PERMISSION_DENIED = -4,
    ERR_RESOURCE_LIMIT = -5,
    ERR_OPERATION_FAILED = -6,
    ERR_CANCELLED = -7,
    ERR_INTERNAL = -8,
};
```

### 统一错误格式

```cpp
// 成功
bsp_kout << "[INFO] Operation completed" << kendl;

// 警告
bsp_kout << "[WARNING] Resource usage high" << kendl;

// 错误
bsp_kout << "[ERROR] Invalid parameter" << kendl;

// 致命
bsp_kout << "[FATAL] System instability detected" << kendl;
```

---

## 🎯 最佳实践

### ✅ 推荐做法

1. **始终验证参数**
2. **使用 `bsp_kout` 输出**
3. **避免动态内存分配**
4. **提供清晰的帮助信息**
5. **保持命令原子性**

### ❌ 禁止做法

1. **不要使用 `printf`**
2. **不要使用 `malloc/new`**
3. **不要忽略返回值**
4. **不要假设参数存在**
5. **不要依赖全局状态**

---

## 📚 文档

- **[框架设计文档](Docs/kshell_framework_design.md)** - 详细的架构设计和规范
- **[使用指南](Docs/kshell_usage_guide.md)** - 命令开发和调试教程
- **[命令设计文档](Docs/kshell_*_commands_design.md)** - 各子系统命令规范

---

## 🔗 相关接口

- **输入**: `src/include/arch/x86_64/core_hardwares/i8042.h`
- **输出**: `src/include/util/kout.h`
- **线程**: `src/include/Scheduler/per_processor_scheduler.h`

---

## 🚧 未来计划

- [ ] 历史命令支持（上下键浏览）
- [ ] Tab 键自动补全
- [ ] 脚本文件支持
- [ ] 多会话支持（串口/网络）
- [ ] 插件系统
- [ ] ANSI 颜色输出

---

## 📄 许可证

本项目遵循项目整体许可证。

---

## 👥 贡献

欢迎提交 Issue 和 Pull Request！

在添加新命令前，请先：
1. 阅读设计文档
2. 在 `Docs/` 中创建设计说明
3. 实现并测试命令
4. 更新本文档
