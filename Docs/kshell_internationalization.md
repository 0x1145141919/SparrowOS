# kshell 国际化改进说明

## 📝 问题背景

[kshell](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kshell.cpp) 最初设计时使用了中文描述，但 [bsp_kout](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kout.cpp) 输出系统不支持中文字符编码，会导致：
- 显示乱码
- 字符截断
- 终端显示异常

## ✅ 改进方案

将所有用户可见的文本改为英文，符合内核开发的国际化惯例。

### 修改内容

#### 1. 命令表描述 ([kshell.cpp:211-216](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kshell.cpp#L211-L216))

**修改前**:
```cpp
static constexpr command_entry_t command_table[] = {
    {"help",    "显示帮助信息",          cmd_help,    RISK_SAFE, 0},
    {"meminfo", "内存总体使用情况",      cmd_meminfo, RISK_SAFE, 0},
    {"buddy",   "伙伴系统统计",          cmd_buddy,   RISK_SAFE, 0},
    {"heap",    "内核堆使用情况",        cmd_heap,    RISK_SAFE, 0},
};
```

**修改后**:
```cpp
static constexpr command_entry_t command_table[] = {
    {"help",    "Show help information",          cmd_help,    RISK_SAFE, 0},
    {"meminfo", "Memory overview statistics",     cmd_meminfo, RISK_SAFE, 0},
    {"buddy",   "Buddy system statistics",        cmd_buddy,   RISK_SAFE, 0},
    {"heap",    "Kernel heap usage",              cmd_heap,    RISK_SAFE, 0},
};
```

#### 2. 其他文本（已为英文）

以下部分在初始实现时已经使用英文，无需修改：
- ✅ 欢迎信息: `"Kernel Shell (kshell) v0.1"`
- ✅ 提示符: `"kshell> "`
- ✅ 帮助标题: `"=== Kernel Shell (kshell) ==="`
- ✅ 错误信息: `"[ERROR] Unknown command: ..."`
- ✅ 警告信息: `"[WARNING] This is a dangerous operation!"`

## 🎯 优势

1. **兼容性**: 完全兼容 [bsp_kout](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/utils/kout.cpp) 的 ASCII 输出能力
2. **国际化**: 符合操作系统内核开发的通用惯例
3. **可维护性**: 英文描述更简洁，减少字符宽度对齐问题
4. **专业性**: 与 Linux、BSD 等成熟系统的 shell 保持一致

## 📋 未来添加命令的规范

在注册新命令时，**必须使用英文描述**：

```cpp
// ✅ 正确示例
{"mycommand", "My custom command description", cmd_mycommand, RISK_SAFE, 0}

// ❌ 错误示例（不要使用中文）
{"mycommand", "我的自定义命令", cmd_mycommand, RISK_SAFE, 0}
```

## 🔗 相关资源

- 输出系统: [src/include/util/kout.h](file:///home/PS/PS_git/OS_pj_uefi/kernel/src/include/util/kout.h)
- 命令框架: [Docs/kshell_framework_design.md](file:///home/PS/PS_git/OS_pj_uefi/kernel/Docs/kshell_framework_design.md)
- 使用指南: [Docs/kshell_usage_guide.md](file:///home/PS/PS_git/OS_pj_uefi/kernel/Docs/kshell_usage_guide.md)

---

**更新日期**: 2026-04-27  
**改进原因**: bsp_kout 不支持中文字符编码
