PCIe 观察命令设计规范（精简版）
1. 三层命令架构
命令	功能	输入	输出
pcie_segs	ECAM 段组概览	无	段组号、总线范围、地址
pcie_BDFs	段内设备拓扑	seg	BDF、Vendor:Device、Class、类型
pcie_BDF	设备详细信息	BDF	完整配置空间 + BAR + Capability
2. 参数快速参考

pcie_BDFs 过滤选项：

    --bus=N — 只显示指定总线

    --class=0xXXYY — 按 Class Code 过滤

    --vendor=0xXXXX — 按 Vendor ID 过滤

    --bridge-only — 只显示桥设备

    --tree — 树状拓扑

pcie_BDF 子视图：

    --bars — 只显示 BAR

    --caps — 只显示 Capability 链表

    --header — 只显示头 64 字节

    --raw=OFFSET:LEN — 原始 dump

BDF 格式（任一）：

    bus:dev.func → 1:0.0

    seg:bus:dev.func → 0:1:0.0

    bus,dev,func → 1,0,0

3. 安全等级

SAFE — 所有命令只读，无需确认
4. 实现依赖

    global_container — ECAM 段组链表

    node.vminterval.vbase — 虚拟基址

    直接访问 volatile 指针（MMIO）