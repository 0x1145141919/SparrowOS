# i8042 键盘事件管线设计（先审阅后实现）

## 1. 背景与目标

当前 `i8042_cpp_enter` 仅做了扫描码字节读取与简单入队（而且 tail 递增有 bug），尚未形成可消费的键盘事件流。  
本设计目标是把它变成唯一入口的完整链路：

1. 从 `0x60` 读取扫描码字节（控制器已把 Set2 翻译为 Set1）。
2. 维护一个轻量状态机，按字节归并为按键 make/break 事件。
3. 翻译成 `ps_2_keyboard_event`。
4. 写入事件环形缓冲区。
5. 唤醒所有订阅者（等待队列上的线程）。

本阶段只输出设计，不改行为实现。

## 2. 约束与前提

1. 中断上下文必须短路径、不可阻塞。
2. 单键盘设备源（IRQ1），写端只有 `i8042_cpp_enter` 一个。
3. 读端可多线程，需靠现有 `tid_wait_queue` 协作。
4. 控制器启用了翻译（Set2 -> Set1），所以状态机以 Set1 字节流定义。
5. LED 控制（Caps/Num/Scroll）先与事件生产解耦，本轮只维护软件状态位，不在中断里发命令。

## 3. 状态机设计（扫描码字节流 -> 逻辑按键事件）

### 3.1 状态定义

新增 `i8042_kbd_parser_state`（全局静态）：

1. `e0_prefix_pending`：已收到 `0xE0`，下一字节按扩展键解释。
2. `e1_pause_pending_count`：收到 `0xE1` 后，Pause 序列剩余字节计数（Set1 下固定长度）。
3. `error_recovery`：检测到非法/截断序列后，丢弃到下一有效起点。

### 3.2 事件归并规则

1. 普通字节（非 `E0/E1`）  
`code & 0x80 == 0` 为 make（按下），否则 break（抬起，去掉高位）。
2. `E0 xx` 扩展字节  
`xx` 按同样规则区分 make/break，标记 `is_extended=1`。
3. `E1` 特殊序列（Pause）  
聚合完整序列后生成单个按下事件（Pause 常见无 break）；若不完整则记错误并恢复。
4. PrintScreen 兼容  
按 Set1 的 `E0 2A E0 37`（make）与 `E0 B7 E0 AA`（break）做组合识别，产出单个 key 事件。
5. 非法字节或状态冲突  
不上报普通 key，产出 `error` 事件（可选）或仅累计统计计数。

### 3.3 锁定键与修饰键更新时机

1. `Shift/Ctrl/Alt` 在 make/break 时更新“当前按下位图”。
2. `CapsLock/NumLock/ScrollLock` 在 make 时翻转软件态（break 不翻转）。
3. 事件中携带“事件产生后”的修饰键快照，便于消费者无状态处理。

## 4. `ps_2_keyboard_event` 结构体设计（按 64-bit 微秒时间戳调整）

建议重定义为固定 16 字节，字段语义明确、无位域 ABI 风险，并使用完整 64-bit 微秒时间戳：

```cpp
enum class ps2_key_action : uint8_t { make = 1, break_ = 2, repeat = 3, error = 255 };
enum class ps2_event_flags : uint16_t {
    none            = 0,
    extended_e0     = 1 << 0,
    synthetic       = 1 << 1, // 例如 PrintScreen/Pause 聚合产生
    overflow_drop   = 1 << 2, // 本次事件产生前发生过缓冲区覆盖
    parser_error    = 1 << 3
};

struct ps_2_keyboard_event {
    uint8_t  key_code;      // 归一化 set1 key code（去掉 break 高位）
    uint8_t  action;        // ps2_key_action
    uint8_t  modifiers;     // bit0 shift, bit1 ctrl, bit2 alt, bit3 caps, bit4 num, bit5 scroll
    uint8_t  reserved0;
    uint16_t flags;         // ps2_event_flags 位掩码
    uint16_t reserved1;
    uint64_t timestamp_us;  // ktime::get_microsecond_stamp()，事件最终成型时采样
};
static_assert(sizeof(ps_2_keyboard_event) == 16);
```

说明：

1. 不使用 C++ 位域，避免跨编译器布局不确定。
2. 既然来源固定为控制器翻译后的 Set1，移除 `key_space` 字段，精简前部元数据。
3. `timestamp_us` 必须在“描述符已完成填充、即将写入 ring”这一刻采样，避免把纯解析耗时混入事件时间点偏差。
4. 订阅者的“是否有新事件”判断不再依赖每事件 `sequence` 字段，改为 ring 全局发布计数（见第 5 节）。

## 5. 缓冲区设计（事件环，广播观察模型）

### 5.1 数据结构

1. `alignas(64) static ps_2_keyboard_event i8042_event_ring[4096];`
2. `u16ka i8042_event_tail_idx;`（生产者尾索引，始终指向“下一个将写入的槽”）
3. `u64ka i8042_event_publish_seq;`（发布序号，单调递增，用于 wait/notify 与越界判断）
4. `u32ka i8042_drop_counter;`（覆盖计数，表示至少有订阅者可能掉队）
5. 订阅者本地游标（非全局）：`subscriber_read_seq`（建议 64-bit）

### 5.4 对外可见性与内存权限约束

`i8042_event_ring` 不直接暴露其内核写入地址。对外仅暴露“重新映射后的只读虚拟地址”，流程固定为：

1. `i8042_event_ring` 本体保持 4096 对齐（便于页粒度映射控制）。
2. 使用 `KspacePageTable::v_to_phyaddrtraslation` 获取 `i8042_event_ring` 的物理基址。
3. 基于该物理基址，通过 `phyaddr_direct_map` 建立新的只读虚拟映射，缓存策略使用 `WB`。
4. 订阅者仅读取该只读映射视图 + 读取发布索引，不允许通过该映射写回事件缓冲（否则立马页错误并且判别为bug）。
5. IRQ 生产者继续使用原内核可写映射更新 ring，读写路径虚拟地址分离以刻意收敛权限。

### 5.2 满队列策略

采用 `drop-old`（覆盖最旧事件）：

1. 中断必须无阻塞，不能因消费者慢而停写。
2. 全局不维护“可被消费一次就删除”的读指针；环空间天然会覆盖历史槽位。
3. 订阅者用 `publish_seq - subscriber_read_seq > N` 判定自己掉队（被覆盖）。
4. 发生覆盖风险时，订阅者自行把 `subscriber_read_seq` 拉到安全窗口（例如 `publish_seq - N`），并记录一次本地丢失。

### 5.3 并发模型

1. 写端单生产者（IRQ），读端多消费者。
2. IRQ 只负责：写 `ring[tail]` -> 推进 `tail` -> `publish_seq++`。
3. 任意订阅者只推进自己的本地 `subscriber_read_seq`，不会修改全局环索引。
4. 这是“广播观察”，一个事件可以被多个订阅者各自读取一次。
5. 若多个执行实体共享同一订阅上下文，可用 CAS 竞争推进同一个共享 `subscriber_read_seq`（可选策略）。

## 6. 订阅与唤醒模型

### 6.1 队列对象

保留并规范两个队列语义：

1. `i8042_scancode_buffer_subscriber_queue`：后续可移除（本轮不再推荐直接对外暴露原始扫描码）。
2. `i8042_analyzed_buffer_subscriber_queue`：事件级订阅者等待队列（主用）。

### 6.2 唤醒时机

1. 每次成功产出一个 `ps_2_keyboard_event` 后唤醒一次全部订阅者。
2. 唤醒操作在 IRQ 尾部完成（EOI 前后都可，建议在 EOI 前，保持现状风格一致）。
3. 唤醒前先检查 `GlobalKernelStatus >= SCHEDUL_READY` 与队列非空。

### 6.3 防空唤醒建议

消费者使用 `block_if_equal(queue, &last_seen_seq, expected_seq)`：

1. 若序号没变化则睡眠。
2. 序号变化立即返回，避免 race（先检查后睡眠丢事件）。

## 7. 对外接口建议

在 `i8042.h` 新增/调整：

1. 新 `ps_2_keyboard_event` 定义（替换现有位域版）。
2. 新事件 ring 元数据导出（只读）：
   - `extern u16ka i8042_event_tail_idx;`
   - `extern u64ka i8042_event_publish_seq;`
3. 事件数据面导出（只读视图）：
   - 导出 `const ps_2_keyboard_event* i8042_event_ring_ro_view`（或等价句柄），其来源必须是 `phyaddr_direct_map` 的只读 WB 映射，而非原写映射地址。
3. 新消费接口（建议）：
   - `bool i8042_read_event_by_seq(uint64_t seq, ps_2_keyboard_event* out);`
   - `void i8042_wait_event(uint64_t last_publish_seq);`
   - `uint64_t i8042_get_publish_seq();`
4. 订阅者侧接口（建议）：
   - `bool subscriber_consume_next(subscriber_ctx* ctx, ps_2_keyboard_event* out);`
   - 内部逻辑：`if(ctx->read_seq == publish_seq) -> block_if_equal(...)`
   - 若 `publish_seq - ctx->read_seq > N`，执行掉队恢复策略后继续读取。
5. 原 `readonly_buff` 若保留，建议重定义为上述“只读重映射视图”语义，避免命名与权限语义错位。

## 8. `i8042_cpp_enter` 目标流程（实现蓝图）

1. `scancode = inb(0x60)`。
2. `parser.feed(scancode, &maybe_event)`。
3. 若产出事件：
   - 填充 modifiers/flags。
   - 在“描述符最终成型”时调用 `ktime::get_microsecond_stamp()` 填入 `timestamp_us`。
   - 写入 ring，推进 `i8042_event_tail_idx`。
   - `i8042_event_publish_seq++`。
   - 唤醒 `i8042_analyzed_buffer_subscriber_queue`。
4. `x2apic::x2apic_driver::write_eoi()`。

## 9. 错误处理与可观测性

新增统计变量（原子）：

1. `parser_error_count`：非法序列次数。
2. `ring_overflow_count`：环缓冲覆盖次数。
3. `event_generated_count`：成功事件数。

调试打印默认关闭，避免中断路径 `kout` 造成延迟放大。

## 10. 测试清单（实现后执行）

1. 基础按键：`a` 按下/抬起，确认 make/break 与 key_code。
2. 修饰键：`Shift + a`，确认 modifiers 快照变化。
3. 锁定键：`CapsLock` 连续按，确认翻转语义。
4. 扩展键：方向键、右 Ctrl（E0 前缀）。
5. 特殊键：PrintScreen、Pause（组合序列）。
6. 压力测试：快速连发，确认无死锁、订阅者掉队可恢复。
7. 多订阅者：同一事件被所有订阅者观察到，追上后正确阻塞等待。

## 11. 实施分阶段建议

1. 先落地事件结构与 ring（不做 Pause/PrintScreen 聚合）。
2. 再补 `E0/E1` 完整状态机和特殊序列。
3. 最后补消费 API 与订阅线程示例，移除旧扫描码对外接口。
