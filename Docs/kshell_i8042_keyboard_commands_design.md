kshell i8042 键盘诊断命令设计规范
1. 命令总览
命令	功能	优先级
kbdstatus	键盘系统总体状态	P0
kbdevents	事件统计（速率、类型分布）	P0
kbdchars	字符缓冲区统计	P1
kbdmodifiers	修饰键状态	P1
KDB	锁定键快速查询	P1
kbdsubscribers	订阅者队列状态	P2
kbdmonitor	实时事件/字符监控	P2
kbdtest	交互式输入测试	P2
i8042regs	控制器寄存器状态	P3
2. 安全约束

所有命令均为只读操作，无副作用，不需要确认机制。
3. Phase 0 命令
KDB — 锁定键快速查询

    无参数

    刻意设计为大小写不敏感
    
    输出：Caps Lock、Num Lock、Scroll Lock 的开/关状态

    额外显示一行提示：Note: LED indicators are NOT maintained by the system

    kbdmodifiers 与 KDB 的区别：前者包含 Shift/Ctrl/Alt 及历史，后者仅三个锁定键、输出更短
kbdstatus — 键盘系统总体状态

    输出：事件环形缓冲区使用情况、字符缓冲区使用情况、订阅者队列状态、健康度评估

    详细程度：brief / normal（默认）/ full

    健康度规则：

        Healthy：overflow=0, error<10, drop<10

        Warning：error 或 drop 在 10–50 之间

        Critical：overflow>0 或 error/drop ≥ 50

kbdevents — 事件统计信息

    显示模式：summary（默认）/ rate / errors

    rate 模式需计算：总事件数 / 时间跨度，以及最近 1000 事件的瞬时速率、最大突发（events/10ms）

    errors 模式需列出最近 5 条错误的 Seq、时间戳、错误类型

4. Phase 1 命令
kbdchars — 字符缓冲区统计

    显示模式：summary（默认）/ distribution / drops

    distribution 模式需打印 Top 20 字符的频率和 ASCII 条状图

    drops 模式需列出最近 5 次丢弃事件的时间、字符、原因

kbdmodifiers — 修饰键状态

    显示模式：current（默认）/ history / map

    current 模式输出：Shift/Ctrl/Alt 的按下/释放、Caps/Num/Scroll 的开/关

    history 模式需记录最近 10 次修饰键切换事件



5. Phase 2 命令
kbdsubscribers — 订阅者队列状态

    指定队列名：all（默认）/ scancode / analyzed / char

    输出：每个队列的等待线程 ID 列表、队列长度

    访问队列时必须加锁

kbdmonitor — 实时监控

    监控模式：events（默认）/ chars / both

    参数：监控数量（默认 10，0 表示无限）

    需支持中断退出（如按 ESC）

    输出格式：表格（Seq、键码/字符、动作、修饰键等）

kbdtest — 交互式测试

    参数：测试时长（秒，默认 10，0 表示直到按 ESC）

    输出：实时显示每次按键的详细信息，结束时输出统计（总按键数、字符数、错误数）

    错误判断：扫描码无效、解析错误、翻译失败

6. Phase 3 命令
i8042regs — 控制器寄存器状态

    读取端口 0x60 和 0x64

    输出：Status Register（位字段解释）、Control Register（通过命令 0x20 读取）

    不发送可能改变状态的命令

7. 数据来源（仅引用，不展开代码）

    i8042_buffer_max_size、i8042_char_buffer_max_size

    i8042_event_publish_seq、i8042_ring_overflow_count、i8042_parser_error_count

    i8042_char_publish_seq、i8042_char_drop_count

    tid_wait_queue 的遍历和锁操作

    i8042_read_event_by_seq、i8042_char_read_event_by_seq

    inb(0x64)、outb(0x64, 0x20) + inb(0x60)