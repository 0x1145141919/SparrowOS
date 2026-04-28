kshell x86 架构命令设计规范
1. 命令总览
命令	功能	风险等级	优先级
cpuid	CPUID 查询	SAFE	P1
cpuinfo	CPU 综合信息	SAFE	P1
rdmsr	读 MSR 寄存器	SAFE	P0
wrmsr	写 MSR 寄存器	DANGEROUS	P0
rdtsc	读时间戳计数器	SAFE	P1
hpet	HPET 定时器查询	SAFE	P2
inb/inw/inl	IO 端口读	SAFE	P0
outb/outw/outl	IO 端口写	DANGEROUS	P0
apic	APIC 状态查询	SAFE	P2
cr	控制寄存器查询	SAFE	P1
2. 安全约束
风险等级	确认机制
SAFE	无需确认
DANGEROUS	显示醒目警告，要求输入 YES 确认，支持 -f 跳过

    危险操作需提供当前值供用户对比

    必须捕获 #GP 异常（无效 MSR、无效端口等），返回错误而非崩溃

3. Phase 0 命令（P0）
rdmsr — 读 MSR

    参数：<address> [format]

    format：full（默认，带位字段解析）/ raw / bits

    输出：原始值 + 位字段解释（对常见 MSR）

    输出示例：APIC_BASE 显示 xAPIC/x2APIC 模式、基地址、BSP 标志

wrmsr — 写 MSR

    参数：<address> <value> [-f]

    要求输入 YES 确认

    输出：写入前显示当前值，写入后显示成功/失败

inb / inw / inl — IO 端口读

    参数：<port> [count] [format]（format：hex/dec/bin/ascii）

    常用端口自动添加注释（如 0x60 → Keyboard Data）

    支持连续读取，输出 hex dump + ASCII

outb / outw / outl — IO 端口写

    参数：<port> <value> [-f]

    危险端口（如 0x64 写 0xFE、0xCF9、0x70）要求输入特殊确认词（如 RESET）

    输出：写入前显示警告，写入后显示结果

4. Phase 1 命令（P1）
cpuid — CPUID 查询

    参数：<leaf> [subleaf] [format]

    format：full（默认，解析寄存器+特性位）/ raw / feature

    支持十六进制（0x）和十进制

    常见 leaf 自动解析：0（厂商字符串）、1（特性）、7（扩展特性）、0x80000001（扩展）、0x80000002-0x80000004（品牌字符串）

cpuinfo — CPU 综合信息

    参数：[detail_level]（brief/normal/full/verbose）

    整合多个 CPUID leaf 的信息

    输出：厂商、品牌字符串、拓扑（逻辑 CPU/核心/线程数）、缓存层级、特性集（ISE/虚拟化/安全/电源管理）

rdtsc — 读 TSC

    参数：[mode]（single/delta/loop <n>）

    需要 TSC 频率信息（从 tsc_fs_per_cycle 或估算）

    输出：原始值、时间转换（纳秒/微秒）、可靠性标志

cr — 控制寄存器查询

    参数：<register>（0/2/3/4）

    输出：原始值 + 位字段解释

    CR2 需显示页错误地址，CR3 需显示 PML4 物理地址

5. Phase 2 命令（P2）
hpet — HPET 查询

    参数：[mode]（status/timestamp/compare）

    输出：基地址、修订号、计数器位数、当前时间戳、时钟周期

apic — APIC 状态查询

    参数：[mode]（status/timer/lvt/error）

    输出：APIC 模式（xAPIC/x2APIC）、APIC ID、版本、TPR/PPR、SVR、LVT 配置、定时器状态

6. 数据来源（仅引用，不展开代码）

    cpuid_tmp 类（封装 CPUID 指令）

    rdmsr() / wrmsr_func()

    rdtsc()、tsc_fs_per_cycle、is_tsc_reliable

    HPET_driver_only_read_time_stamp

    inb/outb 等 IO 端口函数

    x2apic_driver、query_apicid()、query_x2apicid()

    控制寄存器读取函数（read_cr0() 等）