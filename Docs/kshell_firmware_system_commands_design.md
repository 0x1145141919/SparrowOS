kshell 固件系统命令设计规范
1. 命令分类
类别	命令	风险等级
UEFI 时间	uefitime, uefisettime	SAFE / WARNING
UEFI 电源	uefireboot, ueficreset, uefishutdown	DANGEROUS
ACPI 列表	acpistat, acpilist, acpifind	SAFE
ACPI 转储	acpidump	SAFE
ACPI 解析	acpimadt	SAFE
2. 时间命令
uefitime — 查询 UEFI 时间

    调用 EFI_RT_SVS::rt_time_get()

    支持输出格式：full（默认）、simple、timestamp

    时区偏移转换为 UTC±n 显示

uefisettime — 设置 UEFI 时间

    需要确认

    日期格式：YYYY-MM-DD

    时间格式：HH:MM:SS

    前置验证：闰年、月份天数、时分秒范围

    参数 -f 可强制跳过确认

3. 电源命令（均需确认）
命令	UEFI 调用	确认词
uefireboot	rt_reset(EfiResetWarm)	REBOOT
ueficreset	rt_reset(EfiResetCold)	REBOOT
uefishutdown	rt_reset(EfiResetShutdown)	SHUTDOWN

    无需实现 -f 跳过确认

    执行前显示警告信息

4. ACPI 命令
acpistat — ACPI 状态概览

输出：段地址（物理/虚拟）、XSDT 条目数、关键表存在性、健康状态
acpilist — 列出 ACPI 表

输出格式表格：Index、Signature、Length、Revision、OEM ID、OEM Table ID
支持详细程度：brief / normal（默认）/ full
acpifind — 查找 ACPI 表

输出：物理地址、虚拟地址、表头信息
acpidump — 转储 ACPI 表内容

    参数：<signature> [offset] [length] [format]

    格式：hex / ascii / both

    实施前计算并显示校验和的有效性

    受 offset / length 约束

acpimadt — 解析 MADT 表

输出：Local APIC 地址、条目统计、处理器/IO APIC 计数
支持模式：summary（默认）/ entries / apic