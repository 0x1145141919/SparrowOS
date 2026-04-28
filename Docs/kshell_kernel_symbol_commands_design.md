kshell 内核符号查询命令设计规范
1. 命令总览
命令	功能	优先级
ksymstat	符号表状态	P0
ksymlist [pattern]	符号列表查询	P0
ksymaddr <address>	地址→符号解析	P0
ksymname <symbol_name>	符号→地址解析	P0
kstack [depth]	当前栈回溯	P1
kptrace <tid> [depth]	指定线程栈回溯	P1
kthreadcheck	线程入口检查	P1
ksymsize <symbol_name>	符号大小估算	P2
ksymcrossref <address>	交叉引用分析	P3
ksymsearch <pattern> [type]	高级符号搜索	P2
2. 安全约束

所有命令均为只读操作，无副作用，不需要确认机制。
3. Phase 0 命令
ksymstat — 符号表状态

输出：物理/虚拟基址、符号条目数、总大小、地址范围、类型分布（函数/变量/常量）

数据来源：ksymmanager::get_phybase(), get_virtbase(), get_entry_count()
ksymlist [pattern] [count] — 符号列表

    支持通配符 *（任意字符匹配）

    参数 count：默认 50，0 表示全部

    输出格式：Index、Address、Type、Name

    Type 显示：FUNC / VAR / CONST

ksymaddr <address> — 地址解析

    输入地址必须为十六进制（0x 前缀）

    输出：符号名、符号地址、偏移量、下一个符号及其距离

    未命中时打印有效范围

ksymname <symbol_name> [mode] — 符号名解析

    匹配模式：exact（默认）/ prefix / contains

    精确匹配输出单个符号的地址

    模糊匹配输出所有匹配项的表

4. Phase 1 命令
kstack [depth] — 当前栈回溯

    遍历 RBP 链，深度默认 20，最大 128

    每帧输出 RIP 及其对应的符号+偏移

    安全验证：8 字节对齐、非空 RIP、无回环

kptrace <tid> [depth] — 指定线程栈回溯

    通过 TID 获取线程控制块的 RBP

    遍历栈帧，输出格式同 kstack

    额外显示线程状态（Running/Sleeping 等）

kthreadcheck — 线程入口检查

    检查当前栈是否包含 allthread_true_enter 或其附近地址

    输出：YES + 入口函数地址和所在帧索引，或 NO + 上下文提示

5. Phase 2 命令
ksymsize <symbol_name> — 符号大小估算

    通过当前符号到下一个符号的地址差估算大小

    输出：字节数 + 粗略指令数（按 4 字节/指令估算）

ksymsearch <pattern> [type] — 高级符号搜索

    支持简单正则：^（开头）、$（结尾）、.*（任意）

    类型过滤：func / var / all（默认）

    输出匹配的表，格式同 ksymlist

6. Phase 3 命令
ksymcrossref <address> — 交叉引用分析

    输出包含该地址的符号及其范围

    提示：完整交叉引用需要 DWARF 调试信息，当前仅显示包含关系

7. 数据来源（仅引用）

    ksymmanager::get_entry_near_addr(addr) — 二分查找

    ksymmanager 静态成员：symbol_table、entry_count

    kptrace_current_stack_has_kthread_entry()

    self_trace()、else_trace(rbp)