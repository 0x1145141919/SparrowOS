# text_input_event 管线设计：统一输入事件层

## 1. 动机

现有 `kbd_char_event` → `i8042_blockable_keyboard_listening` 架构只有一条字符流，消费者**完全无法区分"普通字符"与"控制事件"**：

- 上/下/左/右方向键被静默丢弃，无法实现导航
- Del / Home / End / PgUp / PgDn 全部消失
- Esc 也被吞掉（Set1 里 Esc 是 `0x01`，映射表里没有它）

`text_input_event` 用 `event_type` 区分字符与控制事件，统一描述"用户做了什么"。

## 2. 数据结构

### 2.1 text_input_event

已在 `i8042.h` 中声明：

```cpp
struct text_input_event{
    uint8_t event_type;  // 0:普通字符事件；1：文本控制事件
    uint8_t reserved0;
    uint16_t data;       // type=0: Unicode code point；type=1: control code
    uint64_t decisive_event_seq; // 来源键盘事件序号（用于回溯）
};
static_assert(sizeof(text_input_event) == 16);
```

### 2.2 event_type 定义

| event_type | 含义 | data 语义 |
|-----------|------|-----------|
| 0 | 普通字符事件 | 字符 Unicode 码点（当前 ASCII 范围） |
| 1 | 文本控制事件 | 见下方 `text_control_code` 枚举 |

### 2.3 控制事件码

```cpp
// data 值（event_type == 1）
constexpr uint16_t TEXT_CTRL_UP       = 0x0001;
constexpr uint16_t TEXT_CTRL_DOWN     = 0x0002;
constexpr uint16_t TEXT_CTRL_LEFT     = 0x0003;
constexpr uint16_t TEXT_CTRL_RIGHT    = 0x0004;
constexpr uint16_t TEXT_CTRL_HOME     = 0x0005;
constexpr uint16_t TEXT_CTRL_END      = 0x0006;
constexpr uint16_t TEXT_CTRL_PGUP     = 0x0007;
constexpr uint16_t TEXT_CTRL_PGDN     = 0x0008;
constexpr uint16_t TEXT_CTRL_INSERT   = 0x0009;
constexpr uint16_t TEXT_CTRL_DELETE   = 0x000A;  // Forward delete
constexpr uint16_t TEXT_CTRL_ESCAPE   = 0x000B;
constexpr uint16_t TEXT_CTRL_TAB      = 0x000C;
constexpr uint16_t TEXT_CTRL_ENTER    = 0x000D;
constexpr uint16_t TEXT_CTRL_BACKSPACE= 0x000E;
```

说明：
- `TEXT_CTRL_ENTER` 和 `TEXT_CTRL_TAB` 同时也会通过 type=0 产生字符 `'\n'` / `'\t'`。消费者可按需过滤。
- `TEXT_CTRL_BACKSPACE` 同理，type=0 仍然产出 `'\b'`。
- **一张表包含两种产出**：多数消费者只需看见 type=0 字符；需要行编辑的消费者（kshell）同时处理两种类型。

## 3. 管线拓扑

```
i8042 IRQ → ps_2_keyboard_event ring (已有)
                │
                ├──→ kbd_char_subscriber (已有, 向下兼容)
                │       └── kbd_char_event ring
                │
                └──→ text_input_subscriber (新增)
                        └── text_input_event ring (新增)
                                │
                                ├──→ 消费者 A（batch read）
                                ├──→ 消费者 B（batch read）
                                └──→ ...
```

### 3.1 为什么不直接改造 char subscriber？

- `kbd_char_event` 可能已有其他消费者，保持向后兼容。
- `text_input_event` 语义更丰富，为后续输入法、Unicode 扩展做好准备。

## 4. 新增线程：`i8042_text_input_subscriber_main`

### 4.1 线程上下文

```cpp
struct text_input_subscriber_ctx {
    uint64_t event_read_seq;        // 读取 i8042_event_publish_seq 的游标
    uint64_t input_publish_local;   // 产出计数（调试）
    uint64_t event_drop_recover;    // 掉队恢复计数
};
```

### 4.2 主循环

```cpp
void* i8042_text_input_subscriber_main(void*) {
    uint64_t read_seq = i8042_get_publish_seq();

    while (true) {
        uint64_t pub = i8042_get_publish_seq();
        if (read_seq == pub) {
            i8042_wait_event(read_seq);
            continue;
        }
        if ((pub - read_seq) > i8042_buffer_max_size)
            read_seq = pub - i8042_buffer_max_size;

        while (read_seq < pub) {
            ps_2_keyboard_event raw{};
            if (i8042_read_event_by_seq(read_seq, &raw)) {
                produce_text_events(read_seq, raw);
            }
            read_seq++;
        }
    }
}
```

### 4.3 翻译逻辑

`produce_text_events` 接收一个 `ps_2_keyboard_event`，判断是否为可产生事件的按键（make / repeat），然后执行：

#### 步骤 A：控制类按键（preempt char translation）

| 物理键 | 条件 | 产出 control code |
|--------|------|-------------------|
| ESC (`0x01`) | — | `TEXT_CTRL_ESCAPE` |
| ↑ (`0x48`, E0) | Extended E0 | `TEXT_CTRL_UP` |
| ↓ (`0x50`, E0) | Extended E0 | `TEXT_CTRL_DOWN` |
| ← (`0x4B`, E0) | Extended E0 | `TEXT_CTRL_LEFT` |
| → (`0x4D`, E0) | Extended E0 | `TEXT_CTRL_RIGHT` |
| Home (`0x47`, E0) | Extended E0 | `TEXT_CTRL_HOME` |
| End (`0x4F`, E0) | Extended E0 | `TEXT_CTRL_END` |
| PgUp (`0x49`, E0) | Extended E0 | `TEXT_CTRL_PGUP` |
| PgDn (`0x51`, E0) | Extended E0 | `TEXT_CTRL_PGDN` |
| Ins (`0x52`, E0) | Extended E0 | `TEXT_CTRL_INSERT` |
| Del (`0x53`, E0) | Extended E0 | `TEXT_CTRL_DELETE` |

#### 步骤 B：同时产生 type=0 + type=1 的键

这些键兼有"字符输出"和"控制语义"：

| 物理键 | 条件 | type=0 产出 | type=1 产出 |
|--------|------|-------------|-------------|
| Enter (`0x1C`) | — | `'\n'` | `TEXT_CTRL_ENTER` |
| Enter (keypad, `0x1C`, E0) | Extended E0 | — | `TEXT_CTRL_ENTER` |
| Tab (`0x0F`) | — | `'\t'` | `TEXT_CTRL_TAB` |
| Backspace (`0x0E`) | — | `'\b'` | `TEXT_CTRL_BACKSPACE` |

产出顺序固定：**先 type=0，后 type=1**，两事件使用相同 `decisive_event_seq`。

#### 步骤 C：普通字符翻译（fallback）

未被 A/B 捕获的按键，走现有 `kbd_char_event` 的字符映射逻辑：

```cpp
text_input_event ev;
ev.event_type = 0;
ev.reserved0 = 0;
ev.data = static_cast<uint16_t>(ch);   // Unicode 码点
ev.decisive_event_seq = seq;
```

- `data` 字段使用 `uint16_t`，为 UTF-16 BMP（U+0000 ~ U+FFFF）预留。
- 当前实现只映射 ASCII，后续可扩展。

### 4.4 ESC 键的特殊处理

Esc（`0x01`）在 Set1 中同时是单个按键和部分扩展序列的前导字节。因 i8042 解析器已在 `flags` 中用 `ps2_event_flag_extended_e0` 标记了 E0 扩展，`text_input_subscriber` 可以直接判断：若 `key_code == 0x01 && !ExtendedE0` → `TEXT_CTRL_ESCAPE`。

## 5. 中央环形缓冲区

### 5.1 元数据

```cpp
constexpr uint16_t text_input_ring_size = 1024;
alignas(4096) static text_input_event
    text_input_ring[text_input_ring_size];

u16ka text_input_event_tail_idx;      // 下一个写槽（生产者）
u64ka text_input_publish_seq;         // 发布序号
u64ka text_input_drop_count;          // 覆盖丢弃计数
tid_wait_queue* text_input_subscribers_queue;   // 全局等待队列
```

### 5.2 只读视图

与 `kbd_char_event` 一致：建立 `phyaddr_direct_map` 只读映射，对外暴露 `text_input_ring_readonly_view`。

```cpp
extern const text_input_event* text_input_ring_readonly_view;
```

### 5.3 决定性事件索引策略

`decisive_event_seq` 填写产出该事件时使用的 i8042 事件序号。对于同时产出 type=0 + type=1 的键，两次发布使用**相同**的 `decisive_event_seq`，保证跨层可回溯。

## 6. 接口设计：两层读取 API

现有调度器实时性差（时间片不可预测），消费者被唤醒后必须能够**一次批量拉取**全部积压事件，避免逐次消费导致的高延迟回绕。

### 6.1 Level 1：单事件读取（基本，轻量消费者）

延续 `kbd_char_event` 风格的逐事件接口：

```cpp
// 获取当前发布序号
extern "C" uint64_t text_input_get_publish_seq();

// 按序号读取单个事件（带 seq-publish_seq 一致性校验）
extern "C" bool text_input_read_event_by_seq(uint64_t seq, text_input_event* out);

// 阻塞等待新事件（基于全局 publish_seq）
extern "C" void text_input_wait_event(uint64_t last_publish_seq);
```

适用场景：调试工具、简单转发、单事件循环。

### 6.2 Level 2：批量读取（推荐，核心接口）

```cpp
/**
 * @brief 从中心 ring 批量读取事件，一次性排出积压
 *
 * @param start_seq      起始 seq
 * @param out_events     输出缓冲区
 * @param max_count      最大读取数
 * @return uint32_t      实际读取数（可能为 0）
 *
 * @note 处理 ring 回绕：如有环回绕，分两次拷贝
 * @note 不阻塞，只是尽量从中心 ring 读取
 */
extern "C" uint32_t text_input_batch_read(
    uint64_t start_seq,
    text_input_event* out_events,
    uint32_t max_count);
```

实现要点：

```cpp
uint32_t text_input_batch_read(
    uint64_t start_seq,
    text_input_event* out_events,
    uint32_t max_count)
{
    uint64_t pub = text_input_publish_seq.load(acquire);
    if (start_seq >= pub) return 0;

    uint64_t avail = pub - start_seq;
    if (avail > text_input_ring_size) {
        // 掉队：只读最新 ring_size 个
        start_seq = pub - text_input_ring_size;
        avail = text_input_ring_size;
    }

    uint32_t to_read = (avail < max_count) ? (uint32_t)avail : max_count;
    uint32_t idx = (uint32_t)(start_seq % text_input_ring_size);
    uint32_t first_seg = text_input_ring_size - idx;
    if (first_seg > to_read) first_seg = to_read;

    // 第一段：从 idx 到 ring 尾部
    memcpy(out_events, &text_input_ring[idx], first_seg * sizeof(text_input_event));

    // 第二段：从 ring 头部（如果有回绕）
    if (first_seg < to_read) {
        uint32_t second_seg = to_read - first_seg;
        memcpy(out_events + first_seg, text_input_ring,
               second_seg * sizeof(text_input_event));
    }

    return to_read;
}
```

**通用消费者模式**：

```cpp
// 消费者线程主循环（自管 read_seq）
static uint64_t my_read_seq = 0;

void consumer_main() {
    // 首次：对齐到当前发布序号
    if (my_read_seq == 0)
        my_read_seq = text_input_get_publish_seq();

    while (true) {
        text_input_event batch[64];
        uint32_t n = text_input_batch_read(my_read_seq, batch, 64);

        if (n == 0) {
            text_input_wait_event(my_read_seq);
            continue;
        }

        for (uint32_t i = 0; i < n; i++) {
            // 处理 batch[i]
        }
        my_read_seq += n;
    }
}
```



## 7. 初始化与文件落地

### 7.1 初始化顺序

```cpp
void text_input_subscriber_init() {
    // 1. 分配等待队列
    // 2. 建立 text_input_ring 的只读映射
    // 3. 创建 i8042_text_input_subscriber_main 线程
}
```

建议在 `i8042_char_subscriber_init()` 之后调用：

```cpp
i8042_interrupt_enable();
i8042_char_subscriber_init();    // 已有
text_input_subscriber_init();    // 新增
```

### 7.2 i8042.h 新增声明

```cpp
// text_input_event 结构体（已存在）
struct text_input_event;

// 控制事件码（已存在）

// Level 1：单事件
extern const text_input_event* text_input_ring_readonly_view;
extern u16ka text_input_event_tail_idx;
extern u64ka text_input_publish_seq;
extern u64ka text_input_drop_count;
extern tid_wait_queue* text_input_subscribers_queue;

extern "C" bool   text_input_read_event_by_seq(uint64_t seq, text_input_event* out);
extern "C" uint64_t text_input_get_publish_seq();
extern "C" void   text_input_wait_event(uint64_t last_publish_seq);
extern "C" void   text_input_subscriber_init();

// Level 2：批量
extern "C" uint32_t text_input_batch_read(
    uint64_t start_seq,
    text_input_event* out_events,
    uint32_t max_count);
```

### 7.3 文件落地

```
src/arch/x86_64/core_hardwares/
├── i8042.cpp                          # IRQ 事件发布（已有）
├── i8042_char_subscriber.cpp          # kbd_char_event 管线（已有）
└── i8042_text_input_subscriber.cpp    # text_input_event 管线（新增）

src/include/arch/x86_64/core_hardwares/
└── i8042.h                            # 追加 text_input 相关声明
```

### 7.4 兼容性保证

旧接口 `i8042_blockable_keyboard_listening` 和 `kbd_char_event` 管线完整保留，不受影响。

## 8. 退化与错误处理

| 场景 | 行为 |
|------|------|
| text_input ring 初始化失败 | `text_input_get_publish_seq()` 返回 0 |
| 中心 ring 掉队 | `text_input_drop_count++`，`batch_read` 跳过头 |
| 映射表找不到对应关系 | 不产生事件，静默跳过 |

## 9. 测试清单

### 9.1 基础输入
- [ ] 普通打字：`abc123` → 产生 6 个 type=0 事件
- [ ] Shift 符号：`!@#` → 正确映射
- [ ] CapsLock + Shift：`aA` → 按异或规则

### 9.2 控制事件
- [ ] ↑ → `TEXT_CTRL_UP`
- [ ] ↓ → `TEXT_CTRL_DOWN`
- [ ] ← → `TEXT_CTRL_LEFT`
- [ ] → → `TEXT_CTRL_RIGHT`
- [ ] Home → `TEXT_CTRL_HOME`
- [ ] End → `TEXT_CTRL_END`
- [ ] Del → `TEXT_CTRL_DELETE`
- [ ] PgUp → `TEXT_CTRL_PGUP`
- [ ] PgDn → `TEXT_CTRL_PGDN`
- [ ] Esc → `TEXT_CTRL_ESCAPE`

### 9.3 双类型事件
- [ ] Enter → type=0 `'\n'` + type=1 `TEXT_CTRL_ENTER`，decisive_event_seq 相同
- [ ] Backspace → type=0 `'\b'` + type=1 `TEXT_CTRL_BACKSPACE`
- [ ] Tab → type=0 `'\t'` + type=1 `TEXT_CTRL_TAB`

### 9.4 批量读取
- [ ] `batch_read` 一次读出多个积压事件
- [ ] `batch_read` 处理 ring 回绕（连续读超过 ring 尾部）
- [ ] 掉队场景：`start_seq` 落后超过 ring_size，自动跳到安全窗口
- [ ] 多个消费者各自管理 read_seq，互不干扰

### 9.5 降级与兼容
- [ ] text_input 初始化失败 → 消费者降级使用 |
- [ ] 新旧两条管线（kbd_char_event / text_input_event）同时工作互不干扰
- [ ] `i8042_blockable_keyboard_listening` 仍可正常使用

## 10. 后续扩展方向

1. **Ctrl+按键组合**：作为独立事件产出（Ctrl+C→中止，Ctrl+D→EOF，Ctrl+W→删词）
2. **Unicode 扩展**：`data` 升级为完整 UTF-16 码点，配合输入法组合
3. **键盘布局切换**：一套映射表不再足够，需要布局插件
