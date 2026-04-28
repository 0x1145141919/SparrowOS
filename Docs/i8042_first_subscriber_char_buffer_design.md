# i8042 首个订阅者设计：原始键盘事件到字符缓冲区（广播模型）

## 1. 目标

在现有 `i8042_cpp_enter` 事件发布链路上增加“第一个订阅者”：

1. 订阅 `ps_2_keyboard_event`（只读视图 + 发布序号）。
2. 将可打字事件翻译为字符流（ASCII 优先）。
3. 写入独立字符环形缓冲区。
4. 像 `i8042_cpp_enter` 一样，用“发布序号变化 -> 唤醒所有订阅者”的广播模型对外提供字符数据。

本设计聚焦核心打字能力，不引入 PrintScreen/ACK-Resend 复杂度。

## 2. 范围

### 2.1 本轮实现范围

1. 字母、数字、常见符号、空格、回车、制表、退格。
2. `Shift` + 基础符号映射。
3. `CapsLock` 与字母大小写规则。
4. 忽略非文本导航键（方向键、Home、End 等）对应的字符产出。

### 2.2 暂不实现

1. 输入法/多字节编码。
2. 终端编辑语义（例如行缓冲删除策略）以外的复杂行编辑。
3. PrintScreen 组合语义。
4. ACK/Resend 命令状态机。

## 3. 总体模型

链路分为两级广播：

1. 级别 A：`i8042` 生产 `ps_2_keyboard_event` 并广播（已存在）。
2. 级别 B：字符订阅者消费 A，生产 `kbd_char_event` 并广播（本设计新增）。

关键点：两个级别都不采用“全局消费 tail 抢占”模式；消费者各自维护 `read_seq`。

## 4. 第一个订阅者线程

建议新增内核线程：`i8042_char_subscriber_main`。

### 4.1 线程上下文

```cpp
struct kbd_char_subscriber_ctx {
    uint64_t event_read_seq;      // 读取 i8042_event_publish_seq 的游标
    uint64_t char_publish_local;  // 本线程产出字符计数（调试/统计）
    uint64_t event_drop_recover;  // 事件层掉队恢复次数
};
```

### 4.2 主循环

1. 读取 `publish_seq = i8042_get_publish_seq()`。
2. 若 `event_read_seq == publish_seq`，调用 `i8042_wait_event(event_read_seq)` 阻塞。
3. 若 `publish_seq - event_read_seq > i8042_buffer_max_size`，判定掉队：
   - `event_read_seq = publish_seq - i8042_buffer_max_size`
   - `event_drop_recover++`
4. 对每个 `seq in [event_read_seq, publish_seq)`：
   - `i8042_read_event_by_seq(seq, &ev)`
   - 翻译 `ev -> (0..n) 个字符事件`
   - 发布到字符 ring
5. `event_read_seq = publish_seq`，继续循环。

## 5. 字符翻译策略

## 5.1 仅处理按下语义

1. `action == make`：可产出字符。
2. `action == repeat`：可产出重复字符（符合长按打字）。
3. `action == break`：不产出字符。
4. `action == error`：不产出字符，仅统计。

### 5.2 modifiers 使用

使用事件内快照 `modifiers`：

1. `shift` 位决定 Shift 符号表。
2. `caps` 位仅作用于字母键。
3. `caps` 与 `shift` 对字母采用异或规则（常规键盘行为）。

### 5.3 文本映射

分两张静态表：

1. `base_map[128]`：未按 Shift 的字符。
2. `shift_map[128]`：按 Shift 的字符。

约束：

1. 非可打印键映射为 `0`（不产出）。
2. 回车统一为 `'\n'`（是否转 `\r\n` 由更上层决定）。
3. 退格产出 `'\b'`。

## 6. 字符缓冲区设计（广播模型）

### 6.1 事件结构

```cpp
enum kbd_char_flags : uint16_t {
    kbd_char_flag_none         = 0,
    kbd_char_flag_repeat       = 1 << 0,
    kbd_char_flag_from_keypad  = 1 << 1,
    kbd_char_flag_drop_hint    = 1 << 2
};

struct kbd_char_event {
    char     ch;            // 当前先用 ASCII
    uint8_t  reserved0;
    uint16_t flags;
    uint32_t source_key;    // 来源 key_code + 扩展位（调试/回溯）
    uint64_t decisive_event_seq; // 该字符最终成型所依赖的决定性键盘事件序号
};
static_assert(sizeof(kbd_char_event) == 16);
```

### 6.2 ring 元数据

```cpp
constexpr uint16_t kbd_char_ring_size = 1024; // 可按压测调大
alignas(4096) static kbd_char_event kbd_char_ring[kbd_char_ring_size];

u16ka kbd_char_tail_idx;        // 下一个写槽
u64ka kbd_char_publish_seq;     // 发布序号
u64ka kbd_char_drop_count;      // 覆盖提示计数
tid_wait_queue* kbd_char_subscribers_queue;
```

说明：

1. 生产者只有“第一个订阅者线程”一个，天然单写端。
2. 消费者多读端，各自维护 `read_seq`。
3. 不提供全局 pop 接口。

### 6.3 对外只读视图

和 `i8042_event_ring_readonly_view` 一致：

1. `kbd_char_ring` 物理基址通过 `KspacePageTable::v_to_phyaddrtraslation` 获取。
2. 用 `phyaddr_direct_map` 建立 `PG_R + WB` 只读重映射。
3. 对外只导出只读虚拟地址视图与发布序号，不导出可写地址。

### 6.4 决定性事件索引策略

`kbd_char_event.decisive_event_seq` 的含义：

1. 记录“产出该字符时使用的 i8042 事件序号”。
2. 对于普通键，通常就是当前被消费的 `event_read_seq`。
3. 若后续扩展为组合输入（例如 dead key 或 compose），则写入最终决定字符输出的那条键盘事件序号。
4. 这样可以做跨层回溯：`char_event -> ps_2_keyboard_event`。

## 7. 广播与阻塞语义

字符层提供与 i8042 层同构接口：

1. `uint64_t kbd_char_get_publish_seq()`
2. `bool kbd_char_read_event_by_seq(uint64_t seq, kbd_char_event* out)`
3. `void kbd_char_wait_event(uint64_t last_publish_seq)`

消费者范式：

1. 维护本地 `read_seq`。
2. 若追上发布序号则 `wait_event(read_seq)`。
3. 若掉队（`publish_seq - read_seq > ring_size`）则回拨到安全窗口。

## 8. 与 i8042.cpp 的衔接点

1. 在 `i8042_interrupt_enable()` 完成后，创建 `i8042_char_subscriber_main` 线程。
2. 线程创建成功后初始化字符 ring 与其只读映射。
3. i8042 层无需知道字符层细节，只需保证：
   - `i8042_event_publish_seq` 正常推进
   - `i8042_wait_event` 可用
   - `i8042_read_event_by_seq` 一致

## 8.1 文件落地组织（按本轮约束）

按你的建议，字符订阅者不新增独立头文件，复用 `i8042.h` 暴露必要接口与结构体；实现放到新源文件：

1. 头文件复用：`src/include/arch/x86_64/core_hardwares/i8042.h`
2. 新源文件：`src/arch/x86_64/core_hardwares/i8042_char_subscriber.cpp`
3. `i8042.cpp` 仅保留键盘 IRQ 事件发布逻辑；字符层逻辑在新源文件独立编译。
4. 若后续字符层接口变多，再考虑从 `i8042.h` 拆分出专用输入子模块头文件。

## 9. 失败与恢复策略

1. 字符层掉队：仅丢历史字符，不阻塞后续输入。
2. 映射失败：可临时回退到内核直接地址（仅内核态订阅者），并记录 warning。
3. 队列未初始化：订阅接口返回失败，不 panic（输入可降级）。

## 10. 测试清单

1. 基础打字：`abc123`。
2. Shift 符号：`!@#$%^&*()`。
3. Caps/Shift 交互：`a/A`、`Caps + a`、`Caps + Shift + a`。
4. 长按重复：确认 `repeat` 能持续产出字符。
5. 多订阅者：两个消费者读取同一字符流，均可观察到同一序列。
6. 掉队恢复：人工放慢某订阅者，验证越界后可恢复到安全窗口。
