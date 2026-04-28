kshell 内核 Shell 设计规范
1. 核心定位

内核态交互式 Shell，用于系统调试、状态查询、紧急干预。

    运行环境：内核线程，无用户态支持

    输入：通过 i8042_blockable_keyboard_listening 阻塞获取

    输出：统一使用 bsp_kout

2. 架构
text

输入层 → 解析器 → 命令分发 → 命令执行
         (词法)      ↓
                  命令管理器（红黑树）

3. 核心组件
3.1 命令管理器

    数据结构：红黑树（复用 Ktemplats::RBTree）

    键：命令名字符串

    操作：注册、注销、查找

    生命周期：注册时 command_entry_t 指针由调用方提供并保证长期有效

3.2 命令签名
cpp

KURD_t cmd_xxx(const line_t* line);

    参数解析由命令自行完成

    返回值使用标准的 KURD_t 错误码体系

    通过 bsp_kout 输出信息

3.3 命令元数据

命令注册时需要提供：
字段	说明
name	命令名
description	简短描述
handler	函数指针
risk	SAFE / WARNING / DANGEROUS
need_confirm	危险命令是否需用户确认
4. 解析规范

    支持三种数字前缀：0x（十六进制）、0b（二进制）、无前缀（十进制）

    空格作为词法分隔符

    每行最大长度 4096，最大词数 64（静态预分配）

    忽略空行

5. 安全规范
危险等级	行为
SAFE	只读，直接执行
WARNING	打印警告，直接执行
DANGEROUS	打印警告，要求输入 yes 确认后执行

    所有写操作命令必须标为 DANGEROUS 并经过确认

    内核地址范围检查由命令自行决定是否进行

6. 帮助系统

    help 命令遍历红黑树，按名称排序输出所有命令

    输出格式：命令名 + 描述 + [危险标记]

7. 模块化注册规范

各子系统在自身初始化阶段，自行调用 command_regist 注册命令：