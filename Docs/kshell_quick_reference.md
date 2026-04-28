# kshell 快速参考卡片

## 📋 命令处理器模板

### 基础模板

```cpp
int cmd_<name>(const kshell::line_t& line) {
    // 1. 检查参数数量
    if (line.word_count < <min_args>) {
        bsp_kout << "[ERROR] Usage: <name> <args>" << kendl;
        return -2;
    }
    
    // 2. 提取参数
    const char* arg1 = line.get_word(1);
    
    // 3. 验证参数
    if (!arg1 || kshell::strlen_kernel(arg1) == 0) {
        bsp_kout << "[ERROR] Invalid argument" << kendl;
        return -2;
    }
    
    // 4. 执行逻辑
    // ... your code ...
    
    // 5. 输出结果
    bsp_kout << "[INFO] Success" << kendl;
    
    return 0;
}
```

### 数值参数模板

```cpp
// 解析数值（自动识别进制：0x=HEX, 0b=BIN, 其他=DEC）
uint64_t value = line.words[1].to_uint64(line.base);

// 带默认值的可选参数
uint64_t count = 16;  // 默认值
if (line.word_count >= 3) {
    count = line.words[2].to_uint64(line.base);
}
```

### 选项解析模板

```cpp
bool verbose = false;
for (uint16_t i = 1; i < line.word_count; i++) {
    const char* arg = line.get_word(i);
    if (kshell::strcmp_kernel(arg, "--verbose") == 0) {
        verbose = true;
    } else if (kshell::strcmp_kernel(arg, "--help") == 0) {
        // 显示帮助
        return 0;
    }
}
```

---

## 🔧 注册新命令（三步走）

### 步骤 1: 实现命令处理器

```cpp
int cmd_mycommand(const kshell::line_t& line) {
    // ... implementation ...
    return 0;
}
```

### 步骤 2: 前向声明（在 kshell.cpp 顶部）

```cpp
int cmd_mycommand(const line_t& line);
```

### 步骤 3: 添加到命令表

```cpp
static constexpr command_entry_t command_table[] = {
    // ... existing commands ...
    {"mycommand", "描述信息", cmd_mycommand, RISK_SAFE, 0},
};
```

---

## 🛡️ 风险等级选择

| 等级 | 常量 | 适用场景 | 确认机制 |
|------|------|----------|----------|
| 安全 | `RISK_SAFE` | 只读操作、查询信息 | 无 |
| 警告 | `RISK_WARNING` | 可能影响状态 | 无 |
| 危险 | `RISK_DANGEROUS` | 写操作、系统修改 | 自动触发 |

**示例**:
```cpp
{"meminfo", "内存信息", cmd_meminfo, RISK_SAFE, 0},      // 只读
{"dump", "内存转储", cmd_dump, RISK_WARNING, 0},         // 可能访问敏感数据
{"reboot", "重启系统", cmd_reboot, RISK_DANGEROUS, 1},   // 需要确认
```

---

## 📊 常用 API

### 字符串操作

```cpp
// 字符串比较
if (kshell::strcmp_kernel(str1, str2) == 0) { /* equal */ }

// 字符串长度
uint16_t len = kshell::strlen_kernel(str);

// 获取词
const char* word = line.get_word(index);
```

### 数值转换

```cpp
// 从词法单元转换（自动识别进制）
uint64_t val = line.words[i].to_uint64(line.base);

// 手动转换
uint64_t hex_val = kshell::hex_to_uint64("FF");      // 255
uint64_t bin_val = kshell::bin_to_uint64("1010");    // 10
uint64_t dec_val = kshell::dec_to_uint64("123");     // 123
```

### 行缓冲操作

```cpp
// 检查空行
if (line.is_empty()) { /* skip */ }

// 获取词数
uint16_t count = line.word_count;

// 清空缓冲
line.clear();
```

---

## ❌ 常见错误码

| 错误码 | 含义 | 使用场景 |
|--------|------|----------|
| `0` | 成功 | 正常返回 |
| `-1` | 空行 | 未提供输入 |
| `-2` | 参数错误 | 参数数量/格式不对 |
| `-3` | 未知命令 | 命令不存在 |
| `-4` | 权限不足 | 无权执行 |
| `-5` | 资源限制 | 超出限制 |
| `-6` | 操作失败 | 业务逻辑错误 |
| `-7` | 用户取消 | 确认时输入非 "yes" |

---

## 💡 输出格式化

### 统一错误格式

```cpp
bsp_kout << "[ERROR] Message" << kendl;
bsp_kout << "[WARNING] Message" << kendl;
bsp_kout << "[INFO] Message" << kendl;
bsp_kout << "[FATAL] Message" << kendl;
```

### 进制切换

```cpp
// 十六进制
bsp_kout << "Addr: 0x" << HEX << address << DEC << kendl;

// 二进制
bsp_kout << "Bits: " << BIN_shift << value << DEC << kendl;

// 十进制（默认）
bsp_kout << "Count: " << count << kendl;
```

### 对齐输出

```cpp
// 固定宽度对齐
bsp_kout << "  " << name;
for (uint16_t i = strlen(name); i < 15; i++) {
    bsp_kout << " ";
}
bsp_kout << " - " << description << kendl;
```

---

## 🎯 最佳实践清单

### ✅ 必须做

- [ ] 始终验证参数数量 (`line.word_count`)
- [ ] 检查指针是否为 nullptr
- [ ] 使用 `bsp_kout` 输出（不用 printf）
- [ ] 返回适当的错误码
- [ ] 提供清晰的错误信息

### ❌ 禁止做

- [ ] 不要使用 `malloc/new/delete`
- [ ] 不要使用 `printf/scanf`
- [ ] 不要假设参数一定存在
- [ ] 不要忽略返回值
- [ ] 不要依赖全局状态

### 💡 建议做

- [ ] 提供 `--help` 选项
- [ ] 使用有意义的变量名
- [ ] 添加注释说明复杂逻辑
- [ ] 保持命令原子性
- [ ] 测试边界情况

---

## 🔍 调试技巧

### 打印调试信息

```cpp
bsp_kout << "[DEBUG] word_count: " << line.word_count << kendl;
for (uint16_t i = 0; i < line.word_count; i++) {
    bsp_kout << "[DEBUG] word[" << i << "]: " << line.get_word(i) << kendl;
}
```

### 检查返回值

```cpp
int result = some_operation();
if (result != 0) {
    bsp_kout << "[ERROR] Failed with code: " << result << kendl;
    return result;  // 向上传播
}
```

---

## 📚 文件位置

| 文件 | 路径 | 说明 |
|------|------|------|
| 头文件 | `src/include/util/kshell.h` | 数据结构定义 |
| 实现 | `src/utils/kshell.cpp` | 核心逻辑 |
| 示例 | `src/utils/kshell_examples.cpp` | 7个完整示例 |
| 设计文档 | `Docs/kshell_framework_design.md` | 架构设计 |
| 使用指南 | `Docs/kshell_usage_guide.md` | 开发教程 |
| README | `Docs/kshell_README.md` | 项目概述 |

---

## 🚀 快速启动

```cpp
#include "util/kshell.h"
#include "Scheduler/per_processor_scheduler.h"

void start_kshell() {
    KURD_t result;
    uint64_t tid = Scheduler::create_kthread(kshell_thread, nullptr, &result);
    
    if (tid == 0) {
        bsp_kout << "[ERROR] Failed: " << result << kendl;
    } else {
        bsp_kout << "[INFO] kshell TID: " << tid << kendl;
    }
}
```

---

## 🔗 相关链接

- **i8042 接口**: `src/include/arch/x86_64/core_hardwares/i8042.h`
- **kout 接口**: `src/include/util/kout.h`
- **调度器**: `src/include/Scheduler/per_processor_scheduler.h`

---

**提示**: 将此文件打印或保存为书签，方便开发时快速查阅！
