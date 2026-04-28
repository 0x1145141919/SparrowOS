# kshell 使用指南

## 🚀 快速开始

### 1. 启动 kshell 线程

在内核初始化完成后，创建 kshell 线程：

```cpp
#include "util/kshell.h"
#include "Scheduler/per_processor_scheduler.h"

// 在合适的初始化阶段调用
void start_kshell() {
    KURD_t result;
    uint64_t tid = Scheduler::create_kthread(kshell_thread, nullptr, &result);
    
    if (tid == 0) {
        bsp_kout << "[ERROR] Failed to create kshell thread: " << result << kendl;
    } else {
        bsp_kout << "[INFO] kshell thread created with TID: " << tid << kendl;
    }
}
```

### 2. 与 kshell 交互

启动后，通过键盘输入命令：

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

### 步骤 1: 实现命令处理器

在适当的源文件中实现命令处理函数：

```cpp
#include "util/kshell.h"
#include "util/kout.h"

// 命令处理器签名：int (*)(const line_t& line)
int cmd_mycommand(const kshell::line_t& line) {
    // 1. 检查参数数量
    if (line.word_count < 2) {
        bsp_kout << "[ERROR] Usage: mycommand <arg1>" << kendl;
        return -2;
    }
    
    // 2. 提取参数
    const char* arg1 = line.get_word(1);
    
    // 3. 参数验证
    if (!arg1 || kshell::strlen_kernel(arg1) == 0) {
        bsp_kout << "[ERROR] Invalid argument" << kendl;
        return -2;
    }
    
    // 4. 执行业务逻辑
    bsp_kout << "[INFO] Processing: " << arg1 << kendl;
    
    // ... 你的逻辑 ...
    
    // 5. 输出结果
    bsp_kout << "[INFO] Command completed successfully" << kendl;
    
    return 0;  // 成功
}
```

### 步骤 2: 注册命令

在 `kshell.cpp` 的命令表中添加新条目：

```cpp
static constexpr command_entry_t command_table[] = {
    {"help",       "显示帮助信息",          cmd_help,       RISK_SAFE, 0},
    {"meminfo",    "内存总体使用情况",      cmd_meminfo,    RISK_SAFE, 0},
    {"buddy",      "伙伴系统统计",          cmd_buddy,      RISK_SAFE, 0},
    {"heap",       "内核堆使用情况",        cmd_heap,       RISK_SAFE, 0},
    
    // 添加新命令
    {"mycommand",  "我的自定义命令",        cmd_mycommand,  RISK_SAFE, 0},
};
```

### 步骤 3: 前向声明

在命令表定义之前添加函数声明：

```cpp
// 前向声明命令处理器
int cmd_help(const line_t& line);
int cmd_meminfo(const line_t& line);
int cmd_buddy(const line_t& line);
int cmd_heap(const line_t& line);
int cmd_mycommand(const line_t& line);  // 新增
```

### 步骤 4: 重新编译并测试

```bash
# 编译内核
./onekey_local_test.sh

# 运行并测试新命令
kshell> mycommand test_arg
[INFO] Processing: test_arg
[INFO] Command completed successfully
```

---

## 🔧 命令开发模式

### 模式 1: 只读诊断命令（最常见）

适用于查询系统状态、显示信息等无副作用操作：

```cpp
int cmd_status(const kshell::line_t& line) {
    (void)line;  // 无参数
    
    bsp_kout << "=== System Status ===" << kendl;
    bsp_kout << "Uptime: 12345 seconds" << kendl;
    bsp_kout << "Tasks: 42" << kendl;
    
    return 0;
}
```

**注册**：
```cpp
{"status", "系统状态信息", cmd_status, RISK_SAFE, 0}
```

---

### 模式 2: 带参数的命令

支持位置参数和选项：

```cpp
int cmd_dump(const kshell::line_t& line) {
    // 检查最少参数
    if (line.word_count < 2) {
        bsp_kout << "[ERROR] Usage: dump <address> [count]" << kendl;
        return -2;
    }
    
    // 解析地址参数
    const kshell::word_t& addr_word = line.words[1];
    uint64_t address = addr_word.to_uint64(line.base);
    
    // 可选的计数参数
    uint64_t count = 16;  // 默认值
    if (line.word_count >= 3) {
        count = line.words[2].to_uint64(line.base);
        if (count == 0 || count > 256) {
            bsp_kout << "[ERROR] Count must be 1-256" << kendl;
            return -2;
        }
    }
    
    // 执行内存转储
    bsp_kout << "Dumping memory at 0x" << HEX << address << DEC << kendl;
    for (uint64_t i = 0; i < count; i += 16) {
        bsp_kout << "  " << HEX << (address + i) << ": ";
        // ... 输出数据 ...
        bsp_kout << kendl;
    }
    
    return 0;
}
```

**使用**：
```
kshell> dump 0xFFFF8000
kshell> dump 0xFFFF8000 32
```

---

### 模式 3: 危险命令（需确认）

涉及写操作或系统状态修改的命令：

```cpp
int cmd_reboot(const kshell::line_t& line) {
    (void)line;
    
    bsp_kout << "[WARNING] System will reboot immediately!" << kendl;
    bsp_kout << "All unsaved data will be lost." << kendl;
    
    // 注意：requires_confirmation=1 会自动触发确认流程
    // 如果用户输入不是 "yes"，命令不会执行到这里
    
    // 执行重启
    asm volatile("mov $0x64, %al; out %al, $0x64");
    asm volatile("mov $0xFE, %al; out %al, $0x60");
    
    return 0;
}
```

**注册**：
```cpp
{"reboot", "重启系统", cmd_reboot, RISK_DANGEROUS, 1}
```

**交互**：
```
kshell> reboot
[WARNING] This is a dangerous operation!
Type 'yes' to confirm: yes
[系统立即重启]
```

---

## 🎯 参数解析技巧

### 数值参数

利用 `word_t::to_uint64()` 自动识别进制：

```cpp
// 支持多种进制输入
const kshell::word_t& word = line.words[1];
uint64_t value = word.to_uint64(line.base);

// 用户可以使用：
//   cmd 123      → DEC
//   cmd 0x7B     → HEX
//   cmd 0b1111011 → BIN
```

### 字符串参数

```cpp
const char* str = line.get_word(1);
if (str && kshell::strcmp_kernel(str, "enable") == 0) {
    // 启用功能
} else if (str && kshell::strcmp_kernel(str, "disable") == 0) {
    // 禁用功能
}
```

### 选项解析

支持 `--option` 风格的参数：

```cpp
bool verbose = false;
for (uint16_t i = 1; i < line.word_count; i++) {
    const char* arg = line.get_word(i);
    if (kshell::strcmp_kernel(arg, "--verbose") == 0) {
        verbose = true;
    } else if (kshell::strcmp_kernel(arg, "--help") == 0) {
        bsp_kout << "Usage: cmd [options]" << kendl;
        return 0;
    }
}

if (verbose) {
    bsp_kout << "[VERBOSE] Detailed output..." << kendl;
}
```

---

## 🛡️ 最佳实践

### 1. 始终验证参数

```cpp
// ❌ 错误：未检查参数
int cmd_bad(const kshell::line_t& line) {
    uint64_t addr = line.words[1].to_uint64(line.base);  // 可能越界！
    // ...
}

// ✅ 正确：验证参数数量
int cmd_good(const kshell::line_t& line) {
    if (line.word_count < 2) {
        bsp_kout << "[ERROR] Missing required argument" << kendl;
        return -2;
    }
    uint64_t addr = line.words[1].to_uint64(line.base);
    // ...
}
```

### 2. 使用统一的错误格式

```cpp
// 错误
printf("Error occurred\n");  // ❌ 不要在内核用 printf

// 正确
bsp_kout << "[ERROR] Operation failed" << kendl;  // ✅
```

### 3. 避免动态内存分配

```cpp
// ❌ 错误
char* buf = new char[1024];

// ✅ 正确：使用栈或静态缓冲区
char buf[1024];
static char static_buf[4096];
```

### 4. 提供清晰的帮助信息

```cpp
int cmd_example(const kshell::line_t& line) {
    // 检查 --help 选项
    if (line.word_count >= 2) {
        const char* arg = line.get_word(1);
        if (kshell::strcmp_kernel(arg, "--help") == 0) {
            bsp_kout << "Usage: example <arg1> [arg2]" << kendl;
            bsp_kout << "Options:" << kendl;
            bsp_kout << "  --help    Show this help" << kendl;
            bsp_kout << "  --verbose Enable verbose output" << kendl;
            return 0;
        }
    }
    
    // ... 正常逻辑 ...
}
```

### 5. 保持命令原子性

每个命令应该是自包含的，不依赖其他命令的状态：

```cpp
// ❌ 错误：依赖全局状态
static int last_result = 0;
int cmd_a(const kshell::line_t& line) {
    last_result = 42;
    return 0;
}
int cmd_b(const kshell::line_t& line) {
    bsp_kout << "Last result: " << last_result << kendl;  // 耦合！
}

// ✅ 正确：独立命令
int cmd_a(const kshell::line_t& line) {
    bsp_kout << "Result: 42" << kendl;
    return 0;
}
```

---

## 🐛 调试技巧

### 1. 打印调试信息

```cpp
int cmd_debug(const kshell::line_t& line) {
    bsp_kout << "[DEBUG] word_count: " << line.word_count << kendl;
    for (uint16_t i = 0; i < line.word_count; i++) {
        bsp_kout << "[DEBUG] word[" << i << "]: " << line.get_word(i) << kendl;
    }
    return 0;
}
```

### 2. 检查返回值

```cpp
int result = some_operation();
if (result != 0) {
    bsp_kout << "[ERROR] Operation failed with code: " << result << kendl;
    return result;  // 向上传播错误
}
```

### 3. 使用临时命令测试

在开发新命令时，可以先注册为临时命令测试：

```cpp
{"test", "临时测试命令", cmd_test_feature, RISK_SAFE, 0}
```

测试完成后改为正式名称或删除。

---

## 📚 完整示例：内存信息查询

下面是一个完整的 `meminfo` 命令实现示例：

```cpp
#include "util/kshell.h"
#include "util/kout.h"
#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"

int cmd_meminfo(const kshell::line_t& line) {
    (void)line;
    
    // 获取页分配器统计
    auto stats = FreePagesAllocator::get_fpa_stats_all();
    
    // 获取空闲段信息
    auto free_segs = all_pages_arr::free_segs_get();
    
    // 计算总内存（简化示例，实际需要从 UEFI 获取）
    uint64_t total_pages = stats.alloc_count + free_segs->count;
    uint64_t total_mb = (total_pages * 4096) >> 20;
    uint64_t alloc_mb = (stats.alloc_count * 4096) >> 20;
    uint64_t free_mb = (free_segs->count * 4096) >> 20;
    
    // 输出格式化信息
    bsp_kout << kendl;
    bsp_kout << "=== Memory Overview ===" << kendl;
    bsp_kout << "Total Physical RAM:    " << total_mb << " MB" << kendl;
    bsp_kout << "Allocated Pages:       " << alloc_mb << " MB";
    
    // 计算百分比（整数运算）
    if (total_pages > 0) {
        uint64_t pct = (stats.alloc_count * 100) / total_pages;
        bsp_kout << " (" << pct << "%)" << kendl;
    } else {
        bsp_kout << " (0%)" << kendl;
    }
    
    bsp_kout << "Free Segments:         " << free_mb << " MB" << kendl;
    bsp_kout << "Free Segment Count:    " << free_segs->count << kendl;
    bsp_kout << kendl;
    
    bsp_kout << "Page Allocator Stats:" << kendl;
    bsp_kout << "  Alloc Count:         " << stats.alloc_count << kendl;
    bsp_kout << "  Free Count:          " << stats.free_count << kendl;
    bsp_kout << "  Alloc Failures:      " << stats.alloc_fail << kendl;
    bsp_kout << "  Lock Try Failures:   " << stats.lock_try_fail << kendl;
    bsp_kout << "  Max BCB Scans:       " << stats.bcb_scan_max << kendl;
    
    return 0;
}
```

**输出效果**：
```
kshell> meminfo

=== Memory Overview ===
Total Physical RAM:    16384 MB
Allocated Pages:       8192 MB (50%)
Free Segments:         8192 MB
Free Segment Count:    12

Page Allocator Stats:
  Alloc Count:         15234
  Free Count:          8921
  Alloc Failures:      3
  Lock Try Failures:   0
  Max BCB Scans:       7
```

---

## 🔗 相关资源

- **设计文档**: `Docs/kshell_framework_design.md`
- **命令设计规范**: `Docs/kshell_*_commands_design.md`
- **输入接口**: `src/include/arch/x86_64/core_hardwares/i8042.h`
- **输出接口**: `src/include/util/kout.h`
- **线程创建**: `src/include/Scheduler/per_processor_scheduler.h`

---

## ❓ 常见问题

### Q: 为什么不能使用 printf？

A: kshell 运行在内核态，没有标准库支持。必须使用 `bsp_kout` 进行输出。

### Q: 如何处理长命令？

A: 当前最大行长为 4096 字节，足够容纳大多数命令。如需更长，可调整 `MAX_LINE_LENGTH`。

### Q: 命令执行失败怎么办？

A: 返回非零错误码，框架会自动显示错误信息。常见错误码：
- `-1`: 空行
- `-2`: 参数错误
- `-3`: 未知命令
- `-7`: 用户取消操作

### Q: 如何添加子命令？

A: 可以在主命令中解析子命令：
```cpp
int cmd_memory(const kshell::line_t& line) {
    if (line.word_count < 2) {
        bsp_kout << "Usage: memory <subcommand>" << kendl;
        return -2;
    }
    
    const char* subcmd = line.get_word(1);
    if (strcmp_kernel(subcmd, "info") == 0) {
        return cmd_meminfo(line);
    } else if (strcmp_kernel(subcmd, "dump") == 0) {
        return cmd_memdump(line);
    }
    // ...
}
```
