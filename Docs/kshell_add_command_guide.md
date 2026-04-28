# kshell 命令添加快速指南

## 🚀 三步添加新命令

### 步骤 1: 声明命令处理器

在 [`kshell_commands.h`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/include/util/kshell_commands.h) 中添加前向声明：

```cpp
namespace kshell {

// ... existing declarations ...
int cmd_yourcommand(const line_t& line);  // 新增

} // namespace kshell
```

---

### 步骤 2: 注册到命令表

在 [`kshell_commands.h`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/include/util/kshell_commands.h) 的命令表中添加条目：

```cpp
static constexpr command_entry_t command_table[] = {
    // ... existing commands ...
    {"yourcmd", "Your command description", cmd_yourcommand, RISK_SAFE, 0},
};
```

**参数说明**：
- `name`: 命令名称（用户输入的名称）
- `description`: 简短描述（英文，不超过40字符）
- `handler`: 处理函数指针
- `risk`: 风险等级（`RISK_SAFE` / `RISK_WARNING` / `RISK_DANGEROUS`）
- `requires_confirmation`: 是否需要确认（0=否，1=是）

---

### 步骤 3: 实现命令处理器

#### 选项 A: 简单命令 → 添加到 `kshell_builtin_commands.cpp`

```cpp
#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"

namespace kshell {

// ... existing commands ...

int cmd_yourcommand(const line_t& line) {
    // 示例：获取第一个参数
    if (line.word_count < 2) {
        bsp_kout << "[ERROR] Usage: yourcmd <argument>" << kendl;
        return -1;
    }
    
    const char* arg = line.get_word(1);
    bsp_kout << "You entered: " << arg << kendl;
    
    return 0;  // 成功返回 0
}

} // namespace kshell
```

#### 选项 B: 复杂子系统 → 创建新文件

例如创建 `kshell_memory_commands.cpp`：

```cpp
/**
 * kshell 内存诊断命令实现
 */
#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"
// #include "memory/FreePagesAllocator.h"  // 实际使用时包含相关头文件

namespace kshell {

int cmd_meminfo(const line_t& line) {
    (void)line;
    
    bsp_kout << "=== Memory Information ===" << kendl;
    // TODO: 调用 FreePagesAllocator::get_fpa_stats_all()
    bsp_kout << "[TODO] Implementation pending" << kendl;
    
    return 0;
}

int cmd_buddy(const line_t& line) {
    (void)line;
    
    bsp_kout << "=== Buddy System Statistics ===" << kendl;
    // TODO: 调用 FreePagesAllocator::print_all_bcb_statistics()
    bsp_kout << "[TODO] Implementation pending" << kendl;
    
    return 0;
}

} // namespace kshell
```

然后在 CMakeLists.txt 中添加新文件到编译列表。

---

## 📋 命令模板

### 模板 1: 只读查询命令

```cpp
int cmd_example(const line_t& line) {
    (void)line;  // 如果不需要参数
    
    bsp_kout << "=== Example Command ===" << kendl;
    bsp_kout << "Status: OK" << kendl;
    
    return 0;
}
```

### 模板 2: 带参数的命令

```cpp
int cmd_example(const line_t& line) {
    // 检查参数数量
    if (line.word_count < 2) {
        bsp_kout << "[ERROR] Usage: example <value>" << kendl;
        return -1;
    }
    
    // 获取参数
    const char* arg_str = line.get_word(1);
    uint64_t value = line.words[1].to_uint64(line.base);
    
    bsp_kout << "Value: " << value << " (0x" << khex << value << ")" << kendl;
    
    return 0;
}
```

### 模板 3: 带选项的命令

```cpp
int cmd_example(const line_t& line) {
    bool verbose = false;
    
    // 解析选项
    for (uint16_t i = 1; i < line.word_count; i++) {
        const char* word = line.get_word(i);
        if (strcmp_kernel(word, "--verbose") == 0 || strcmp_kernel(word, "-v") == 0) {
            verbose = true;
        } else if (strcmp_kernel(word, "--help") == 0 || strcmp_kernel(word, "-h") == 0) {
            bsp_kout << "Usage: example [--verbose]" << kendl;
            return 0;
        }
    }
    
    if (verbose) {
        bsp_kout << "Verbose output..." << kendl;
    } else {
        bsp_kout << "Normal output..." << kendl;
    }
    
    return 0;
}
```

### 模板 4: 危险操作命令（需要确认）

```cpp
// 在 kshell_commands.h 中注册时设置 requires_confirmation = 1
{"dangerous_cmd", "Dangerous operation", cmd_dangerous, RISK_DANGEROUS, 1}

// 实现时框架会自动要求确认
int cmd_dangerous(const line_t& line) {
    // 执行到这里说明用户已确认
    bsp_kout << "[WARNING] Executing dangerous operation..." << kendl;
    
    // 实际操作...
    
    bsp_kout << "[INFO] Operation completed." << kendl;
    return 0;
}
```

---

## 🎯 最佳实践

### ✅ 推荐做法

1. **使用英文描述** - 避免 [bsp_kout](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kout.cpp) 不支持中文的问题
2. **参数验证** - 始终检查 `line.word_count`
3. **错误提示清晰** - 使用 `[ERROR]`, `[WARNING]`, `[INFO]` 前缀
4. **返回值规范** - 成功返回 0，失败返回负数错误码
5. **保持简洁** - 每个命令专注于单一功能

### ❌ 避免的做法

1. **不要动态分配内存** - 禁止 `malloc/new`
2. **不要使用浮点运算** - 内核态不支持
3. **不要阻塞太久** - 避免长时间循环
4. **不要忽略参数检查** - 防止越界访问

---

## 🔍 调试技巧

### 1. 测试命令是否注册

启动 kshell 后输入 `help`，检查命令是否出现在列表中。

### 2. 查看命令执行情况

命令执行后会显示：
- 成功：无额外输出（或自定义信息）
- 失败：`[ERROR] Command failed with code: <error_code>`

### 3. 临时添加调试输出

```cpp
int cmd_debug(const line_t& line) {
    bsp_kout << "[DEBUG] Word count: " << line.word_count << kendl;
    for (uint16_t i = 0; i < line.word_count; i++) {
        bsp_kout << "[DEBUG] Word " << i << ": " << line.get_word(i) << kendl;
    }
    return 0;
}
```

---

## 📊 常见错误码

| 错误码 | 含义 | 使用场景 |
|--------|------|----------|
| 0 | 成功 | 命令正常执行 |
| -1 | 通用错误 | 参数不足、格式错误 |
| -2 | 无效参数 | 参数值超出范围 |
| -3 | 资源不足 | 内存不足、设备忙 |
| -7 | 操作取消 | 用户拒绝确认 |

---

## 📚 完整示例

参考 [`kshell_examples.cpp`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kshell_examples.cpp)，包含 7 个完整的命令示例：
- `cmd_version` - 简单只读命令
- `cmd_calc` - 多进制计算器
- `cmd_info` - 带选项的命令
- `cmd_dump` - 内存转储（危险操作）
- `cmd_status` - 系统状态概览
- `cmd_clear` - 交互式确认示例
- `cmd_config` - 多子命令示例

---

## 🔗 相关文件

- **命令表定义**: [`src/include/util/kshell_commands.h`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/include/util/kshell_commands.h) (~60行)
- **内置命令实现**: [`src/utils/kshell_builtin_commands.cpp`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kshell_builtin_commands.cpp) (~80行)
- **核心框架**: [`src/utils/kshell.cpp`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kshell.cpp) (~340行)
- **示例代码**: [`src/utils/kshell_examples.cpp`](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kshell_examples.cpp) (~369行)
- **重构说明**: [`Docs/kshell_commands_modularization.md`](file:///home/PS/PS_git/OS_pj_uefi/kernel/Docs/kshell_commands_modularization.md)

---

**最后更新**: 2026-04-27  
**适用版本**: kshell v0.1+
