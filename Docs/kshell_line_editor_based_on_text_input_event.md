# kshell 行编辑器重构：基于 text_input_event 管线

## 1. 目标

将 kshell 当前基于 `i8042_blockable_keyboard_listening`（纯字符流）的输入模型，替换为基于 `text_input_event` 管线的 L2 batch 接口，从而支持：

- 方向键  ← →  光标移动
- 方向键  ↑ ↓  历史命令导航
- Home / End  行首行尾跳转
- Delete  正向删除
- Esc  清空当前输入
- Tab  命令补全（预留）

仅修改 `read_line_from_keyboard` 内部实现，不动 `run_shell_loop` 的主循环结构。

## 2. 消费者定位

kshell 作为 text_input_event 管线的消费者：

- **单订阅者模型**：kshell 只有一个实例，只维护一个 text_input_event 读取游标。
- **使用 L2 batch 接口**：调用 `text_input_batch_read` + `text_input_wait_event`，自管 `read_seq`。
- **仅消费一条管线**：不监听多个事件源。初始化失败时降级到旧字符流接口。
- **Producer 线程**：`i8042_text_input_subscriber_main` 从 `ps_2_keyboard_event` ring 翻译并填充 `text_input_event` ring。
- **Consumer 线程**：kshell 的主循环线程（与 producer 不同线程，共享中心 ring）。

## 3. 行编辑器状态

在 `read_line_from_keyboard` 的栈上维护编辑器状态：

```cpp
struct line_editor_t {
    char*   line;       // 行缓冲区（指向调用方 buffer）
    size_t  cap;        // 最大容量
    size_t  len;        // 当前有效文本长度（不含 \0）
    size_t  cursor;     // 光标位置（0..len，字符索引）
};
```

不需要动态分配，所有状态在 `read_line_from_keyboard` 的栈帧上存活。

## 4. 核心循环

```cpp
size_t kshell_framework_t::read_line_from_keyboard(
    char* buffer, size_t max_len)
{
    if (!buffer || max_len == 0) return 0;

    line_editor_t ed;
    ed.line   = buffer;
    ed.cap    = max_len;
    ed.len    = 0;
    ed.cursor = 0;
    buffer[0] = '\0';

    // 持久化 seq 游标（static，跨调用保持位置）
    static uint64_t read_seq = 0;

    // 首次调用时对齐到当前发布位置
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
            case 0:
                handle_char(&ed, ev.data, &finished);
                break;
            case 1:
                handle_control(&ed, ev.data, &finished);
                break;
            }

            if (finished) {
                return ed.len;  // Enter 提交
            }
        }
    }
}
```

### 4.1 启动对齐

`static uint64_t read_seq = 0` 跨多次 `read_line_from_keyboard` 调用保持续接。首次调用时通过 `text_input_get_publish_seq()` 对齐到当前发布位置，避免回放历史按键。

### 4.2 批处理策略

每次 `batch_read` 最多取 16 个事件。由于调度器实时性差，消费者可能看到积压的多个事件。批量处理一个未满的 batch 后才重新进入等待，而不是逐事件 wait。

## 5. 事件处理函数

### 5.1 handle_char（type=0）

```cpp
static void handle_char(line_editor_t* ed, uint16_t data, bool* finished) {
    char ch = static_cast<char>(data & 0xFF);

    switch (ch) {
    case '\n':
        ed->line[ed->len] = '\0';
        *finished = true;
        // 不在此处 echo '\n'，由 handle_control 中的 ENTER 处理
        return;

    case '\b':
        if (ed->cursor > 0) {
            // 光标前删
            size_t move_count = ed->len - ed->cursor;
            memmove(&ed->line[ed->cursor - 1],
                    &ed->line[ed->cursor],
                    move_count);
            ed->cursor--;
            ed->len--;
            redraw_line(ed);
        }
        return;

    default:
        if (ed->len < ed->cap - 1) {
            // 在 cursor 处插入，右移后续
            size_t move_count = ed->len - ed->cursor;
            if (move_count > 0) {
                memmove(&ed->line[ed->cursor + 1],
                        &ed->line[ed->cursor],
                        move_count);
            }
            ed->line[ed->cursor] = ch;
            ed->cursor++;
            ed->len++;
            redraw_line(ed);
        }
        return;
    }
}
```

注意：
- 实际不会收到 type=0 的 `'\n'` / `'\b'`（这些已被 subscriber 同时转化为 type=1 事件），但保留处理作为防御。
- 行内插入/删除使用 `memmove` 移动后续字符。

### 5.2 handle_control（type=1）

```cpp
static void handle_control(line_editor_t* ed, uint16_t ctrl, bool* finished) {
    switch (ctrl) {
    case TEXT_CTRL_ENTER:
        ed->line[ed->len] = '\0';
        bsp_kout << kendl;
        *finished = true;
        return;

    case TEXT_CTRL_LEFT:
        if (ed->cursor > 0) {
            ed->cursor--;
            sync_cursor(ed);
        }
        return;

    case TEXT_CTRL_RIGHT:
        if (ed->cursor < ed->len) {
            ed->cursor++;
            sync_cursor(ed);
        }
        return;

    case TEXT_CTRL_HOME:
        ed->cursor = 0;
        sync_cursor(ed);
        return;

    case TEXT_CTRL_END:
        ed->cursor = ed->len;
        sync_cursor(ed);
        return;

    case TEXT_CTRL_DELETE:
        if (ed->cursor < ed->len) {
            size_t move_count = ed->len - ed->cursor - 1;
            if (move_count > 0) {
                memmove(&ed->line[ed->cursor],
                        &ed->line[ed->cursor + 1],
                        move_count);
            }
            ed->len--;
            redraw_line(ed);
        }
        return;

    case TEXT_CTRL_BACKSPACE:
        if (ed->cursor > 0) {
            size_t move_count = ed->len - ed->cursor;
            memmove(&ed->line[ed->cursor - 1],
                    &ed->line[ed->cursor],
                    move_count);
            ed->cursor--;
            ed->len--;
            redraw_line(ed);
        }
        return;

    case TEXT_CTRL_UP:
        history_navigate(ed, -1);
        return;

    case TEXT_CTRL_DOWN:
        history_navigate(ed, +1);
        return;

    case TEXT_CTRL_ESCAPE:
        ed->len = 0;
        ed->cursor = 0;
        redraw_line(ed);
        return;

    case TEXT_CTRL_TAB:
        try_autocomplete(ed);
        return;

    default:
        return;  // INSERT/PGUP/PGDN 忽略
    }
}
```

## 6. 显示策略

使用 `bsp_kout` 进行行内回显。基于 VGA 文本模式 / UEFI text console 的特性：

### 6.1 redraw_line — 完全重绘

在 text（字符被插入/删除）或退格时调用：

```cpp
static void redraw_line(line_editor_t* ed) {
    // 回到行首，写入 prompt + line + 清空残余
    bsp_kout << "\rkshell> ";

    for (size_t i = 0; i < ed->len; i++) {
        bsp_kout << ed->line[i];
    }

    // 用空格覆盖此行剩余内容（比 ed->cap 还长可能导致换行，按最大 256 处理）
    constexpr size_t LINE_CLEAR = 256;
    size_t current_total = 8 + ed->len;  // "kshell> " = 8 chars
    for (size_t i = current_total; i < LINE_CLEAR; i++) {
        bsp_kout << ' ';
    }

    // 回到行首重新输出，以便光标定位
    bsp_kout << "\rkshell> ";
    for (size_t i = 0; i < ed->len; i++) {
        bsp_kout << ed->line[i];
    }

    // 通过 backspace 移动光标到正确位置
    // 光标到行尾的距离 = ed->len - ed->cursor
    for (size_t i = ed->cursor; i < ed->len; i++) {
        bsp_kout << '\b';
    }
}
```

> **优化**：若光标未变（仅内容变），可只重绘内容并同步光标位置，省略清除步骤。

### 6.2 sync_cursor — 仅移动光标

在光标位置变化但文本不变时（LEFT/RIGHT/HOME/END）调用：

```cpp
static void sync_cursor(line_editor_t* ed) {
    // 当前光标在 ed->len 位置（刚刚输出的末尾）
    size_t cur_pos = ed->len;  // 当前真实光标
    size_t target = ed->cursor;

    if (target < cur_pos) {
        for (size_t i = target; i < cur_pos; i++) {
            bsp_kout << '\b';
        }
    }
    // 如果 target > cur_pos 理论上不会发生，因为光标不会超出文本
}
```

### 6.3 历史导航重绘

↑↓ 导航时将历史命令加载到编辑器缓冲区，然后调用 `redraw_line` 全量重绘。

## 7. 历史管理

```cpp
static constexpr size_t HISTORY_MAX   = 64;
static constexpr size_t HISTORY_LEN   = 256;  // 单条最大长度
static char history_pool[HISTORY_MAX][HISTORY_LEN];
static size_t history_count   = 0;   // 已使用槽数
static size_t history_write   = 0;   // 下一个写槽（环状）
static size_t history_browse  = 0;   // 当前浏览位置
static bool   browsing        = false; // 是否处于历史浏览状态

void add_to_history(const char* line, size_t len) {
    if (len == 0 || len >= HISTORY_LEN) return;

    // 与最后一条去重
    size_t last = (history_write == 0) ? HISTORY_MAX - 1 : history_write - 1;
    if (history_count > 0 &&
        strcmp(history_pool[last], line) == 0)
        return;

    strncpy(history_pool[history_write], line, HISTORY_LEN - 1);
    history_pool[history_write][HISTORY_LEN - 1] = '\0';
    history_write = (history_write + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
    browsing = false;
}
```

### 历史浏览导航

```cpp
static void history_navigate(line_editor_t* ed, int direction) {
    if (history_count == 0) return;

    if (!browsing) {
        // 首次进入历史：保存当前编辑行
        // strncpy(current_edit_snapshot, ed->line, HISTORY_LEN);
        browsing = true;
        history_browse = history_write;  // 指向最新
    }

    if (direction < 0) {
        // ↑：往回走
        if (history_browse == 0)
            history_browse = HISTORY_MAX - 1;
        else
            history_browse--;

        // 如果走到了未填充区域，回弹到最后一条
        // history_count 条已填充，分布是 [history_write - count, history_write)
        size_t oldest = (history_write >= history_count)
            ? history_write - history_count
            : HISTORY_MAX + history_write - history_count;
        // 简单处理：超过 count 次就停
    }

    // 从历史池拷贝到编辑器
    const char* src = history_pool[history_browse];
    size_t slen = strlen(src);
    if (slen >= ed->cap) slen = ed->cap - 1;
    memcpy(ed->line, src, slen);
    ed->len = slen;
    ed->cursor = slen;
    ed->line[slen] = '\0';
    redraw_line(ed);
}
```

## 8. Tab 命令补全

### 8.1 前置条件：命令树的全序性质

命令红黑树以字典序（`strcmp`）作为全序关系。该性质使前缀匹配可以直接在有序序列上高效完成：

1. 找到第一个字典序 >= 前缀的节点。
2. 顺序遍历后继节点，筛选前缀匹配的命令。
3. 遇到第一个字典序不再以前缀起始的节点时停止。

例如前缀 `"mm"`：
- 树中命令排序：`db`, `ds`, `help`, `mm`, `mmio`, `ms`, `pci`, `quit`
- `"mm"` 匹配到 `mm` 和 `mmio`，到 `ms` 停止

### 8.2 接口扩展

在 `kshell_framework_t` 中新增公共静态方法：

```cpp
class kshell_framework_t {
public:
    /**
     * @brief 查找所有以 prefix 开头的命令
     *
     * @param prefix       前缀字符串
     * @param out_matches  输出缓冲区（command_entry_t* 数组）
     * @param max_matches  缓冲区容量
     * @return int         实际匹配数（-1 内部错误，0 无匹配）
     */
    static int command_find_prefix(
        const char* prefix,
        command_entry_t** out_matches,
        int max_matches);
};
```

### 8.3 实现策略

```cpp
int kshell_framework_t::command_find_prefix(
    const char* prefix,
    command_entry_t** out_matches,
    int max_matches)
{
    if (!prefix || !out_matches || max_matches <= 0 || !m_command_tree)
        return -1;

    int count = 0;
    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0) return 0;  // 空前缀不匹配

    // 构造一个哨兵节点用于 tree.find 的 lower_bound 查找
    // 因为 find_node 只做精确匹配，我们需要手动从 begin 顺序遍历
    auto it = m_command_tree->begin();
    auto end = m_command_tree->end();

    // 跳到第一个 >= prefix 的节点
    for (; it != end; ++it) {
        int cmp = strcmp(it.key().name, prefix);
        if (cmp >= 0) break;
    }

    // 收集所有以 prefix 开头的节点
    for (; it != end; ++it) {
        const char* name = it.key().name;
        if (strncmp(name, prefix, prefix_len) != 0)
            break;  // 前缀不再匹配，后面字典序更大，肯定都不匹配了

        if (count < max_matches) {
            // 注意：RBTree iterator 的 key() 返回 const T&
            // 我们需要从树中获取可写指针
            // 方法：通过 find_node 再用节点内 data
            command_entry_t* entry = const_cast<command_entry_t*>(
                &it.key());
            out_matches[count++] = entry;
        }
    }

    return count;
}
```

> **注意**：如果 `RBTree::iterator::key()` 返回 `const T&` 且 `T`（即 `command_entry_t`）的实际存储地址就是树节点内的对象地址，则 `const_cast` 安全。若接口不允许，需在 `RBTree` 中新增 `lower_bound` API（推荐）。

### 8.4 Tab 完成逻辑

```cpp
static void try_autocomplete(line_editor_t* ed) {
    if (ed->len == 0) return;

    // 取当前输入行作为前缀
    char prefix[256];
    size_t prefix_len = ed->len;
    if (prefix_len > 255) prefix_len = 255;
    memcpy(prefix, ed->line, prefix_len);
    prefix[prefix_len] = '\0';

    // 查询匹配
    command_entry_t* matches[64];
    int count = kshell_framework_t::command_find_prefix(
        prefix, matches, 64);

    if (count <= 0) {
        // 无匹配：响铃
        bsp_kout << '\a';
        return;
    }

    if (count == 1) {
        // 唯一匹配：自动补全（替换整个行）
        const char* cmd_name = matches[0]->name;
        size_t name_len = strlen(cmd_name);
        if (name_len >= ed->cap) name_len = ed->cap - 1;
        memcpy(ed->line, cmd_name, name_len);
        ed->len = name_len;
        ed->cursor = name_len;
        ed->line[name_len] = '\0';
        bsp_kout << kendl;  // 换行
        bsp_kout << "kshell> " << ed->line;  // 重绘
        redraw_line(ed);
        return;
    }

    // 多个匹配：计算公共前缀
    // 先两两比较，取最小公共前缀
    char common[256];
    const char* first = matches[0]->name;
    size_t common_len = strlen(first);
    if (common_len > 255) common_len = 255;
    memcpy(common, first, common_len);

    for (int i = 1; i < count; i++) {
        const char* m = matches[i]->name;
        size_t j = 0;
        while (j < common_len && common[j] == m[j]) j++;
        common_len = j;
        if (common_len == 0) break;
    }

    // 如果公共前缀长于用户输入，补全到公共前缀
    if (common_len > prefix_len) {
        memcpy(ed->line, common, common_len);
        ed->len = common_len;
        ed->cursor = common_len;
        ed->line[common_len] = '\0';
        redraw_line(ed);
        return;
    }

    // 公共前缀就是用户输入（没有更多可补全）：列出所有匹配
    bsp_kout << kendl;
    for (int i = 0; i < count; i++) {
        if (i > 0) bsp_kout << "  ";
        bsp_kout << matches[i]->name;
    }
    bsp_kout << kendl;

    // 重新显示当前行
    redraw_line(ed);
}
```

### 8.5 三次 Tab 的行为

| 场景 | 行为 |
|------|------|
| 无匹配 | 响铃 `'\a'` |
| 唯一匹配 | 直接补全命令名 |
| 多匹配但有公共前缀超出用户输入 | 补全到公共前缀 |
| 多匹配且无更多公共前缀 | 换行列出所有匹配命令，重新显示行 |

### 8.6 文件变更

| 文件 | 变更 |
|------|------|
| `src/include/util/kshell.h` | 新增 `command_find_prefix` 声明 |
| `src/utils/kshell.cpp` | 实现 `command_find_prefix`、`try_autocomplete` |

## 9. .bashrc 效果示例

```
kshell> ls /pci
kshell> mm 0x1000
kshell> ^[[A     ← ↑ 回到 mm 0x1000
kshell> ms 0x2000 ← 编辑后回车执行
kshell>          ← 新行
kshell> m<TAB>  ← 自动补全
kshell> mm      ← 两个匹配 (mm, mmio)，公共前缀 mm
kshell> mm<TAB> ← 再 tab：列出所有
mm  mmio
kshell> mm      ← 重新显示
kshell> mm      ← 如果输入 mm<TAB> 时，mm 也匹配它自身
mm  mmio        ← 显示两个
```

## 10. 降级策略

当 `text_input_event` 管线不可用时（初始化失败），kshell 自动降级到旧 `i8042_blockable_keyboard_listening` 路径，保留基本打字功能：

```cpp
size_t kshell_framework_t::read_line_from_keyboard(
    char* buffer, size_t max_len)
{
    if (!is_text_input_available()) {
        return read_line_fallback(buffer, max_len);
    }

    // ... 新逻辑 ...
}

static bool is_text_input_available() {
    // text_input ring 初始化后，publish_seq 会被写
    // 如果 publish_seq 为 0 且 text_input_ring_readonly_view 为 nullptr，
    // 表示管线未初始化
    return text_input_ring_readonly_view != nullptr
        || text_input_get_publish_seq() > 0;
}
```

**回退路径**：直接使用现有的 `i8042_blockable_keyboard_listening`，无行编辑支持。

## 11. 文件修改清单

| 文件 | 变更 |
|------|------|
| `src/utils/kshell.cpp` | 重写 `read_line_from_keyboard`，新增 line_editor_t 状态机、历史管理、显示函数、try_autocomplete |
| `src/include/util/kshell.h` | 新增 `command_find_prefix` 静态方法声明 |

## 12. 依赖

必须先完成：
- [x] `i8042.h` 中的 `text_input_event` 结构体与控制事件码声明（已有）
- [x] `i8042_text_input_subscriber.cpp`（已有，本仓库同时提交）
- [ ] `text_input_subscriber_init()` 在系统初始化中被调用

依赖满足前，kshell 降级运行旧代码。

## 13. 测试清单

### 13.1 基础功能
- [ ] 普通打字 + 回车执行
- [ ] 中间插入字符：`hel|o` → 按左 → 打 `l` → `hello`

### 13.2 光标移动
- [ ] ← 光标左移一位
- [ ] → 光标右移一位
- [ ] Home 跳到行首
- [ ] End 跳到行尾
- [ ] 光标不超出 `[0, len]` 范围

### 13.3 删除
- [ ] Backspace 删除光标前字符，后续文本左移
- [ ] Del 删除光标处字符，后续文本左移
- [ ] 行首 Backspace / 行尾 Del 均不操作

### 13.4 历史
- [ ] ↑ 循环浏览历史
- [ ] ↓ 反向浏览历史
- [ ] 空行不入历史
- [ ] 连续相同命令去重
- [ ] 历史容量 64 条可循环覆盖

### 13.5 Tab 补全
- [ ] 无匹配时响铃
- [ ] 唯一匹配时自动填满命令名
- [ ] 多匹配时补全到公共前缀
- [ ] 公共前缀等于用户输入时列出所有匹配
- [ ] 空前缀不触发补全
- [ ] 补全后光标在行尾

### 13.6 其他
- [ ] Esc 清空当前输入行
- [ ] 行满时无法再输入
- [ ] 跨多次 `read_line` 调用 read_seq 连续

### 13.7 降级
- [ ] text_input 管线未初始化 → 使用旧 char_buffer 路径
- [ ] 降级后所有功能回退到纯字符模式
