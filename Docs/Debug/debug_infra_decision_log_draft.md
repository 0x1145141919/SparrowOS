# SpDB 调试基础设施：决策过程日志（草案）

日期：2026-05-17 22:46 ~ 00:47
硬件上下文：Lenovo ThinkBook 16 Gen 6+ (Meteor Lake-P)
项目：SparrowOS UEFI x86_64 kernel

---

## 1. 起点：Thunderbolt DMA 调试

**想法**：利用 TB4 NHI 控制器的 DMA 能力，直接读/写目标机物理内存，
绕过 SparrowOS 的软件栈，实现类似 JTAG/NTB 的硬件级调试器。

**验证结果**：

### NHI 不是 NTB — 硬件架构决定了
- `lspci -s 00:0d.3` → Intel MTL-P TB4 NHI#1，BAR0 = 256KB 控制寄存器
- Linux 源码隧道模型只有 PCI/DP/DMA/USB3 四种，**没有 NTB**
- `ring_desc` 只有 `phys`（本机 DMA 地址），没有 `remote_addr`/`rkey`
- 不支持标准 RDMA（无单边 READ/WRITE 操作码）

### 无法进入 EP_MODE
- MTL NHI 运行于 CM_MODE（ICM 固件接管）
- 源码中 `nhi_fw_mode`：SAFE_MODE / AUTH_MODE / EP_MODE / CM_MODE
- `icm_firmware_init()` 的 switch-case 里 EP_MODE 被当作"wrong mode"拒绝
- NHI ring 归 ICM 固件所有，SparrowOS 无法直接操作 ring 寄存器

### 可用路径
- ICM mailbox 驱动（~200行）：force_power → FW_READY → ALLOW_ALL_DEVS
- XDomain DMA tunnel（需第二台 TB 机器）
- 两段式消息传递（非单边 RDMA）

### 结论
TB 调试可行但门槛高：ICM 交互 + 第二台 TB 机器硬件需求。

---

## 2. DCI (Direct Connect Interface)

**发现**：BIOS 中 DCI Enable / DCI Clock Enable 已打开，
ThinkBook 16 Gen 6+ 支持 Intel DCI OOB 调试。

**评估**：

| 维度 | 评价 |
|------|------|
| 能力 | JTAG 级：halt CPU、硬件断点、任意内存读写 |
| 物理层 | 普通 USB-C 线 → Intel DCI OOB dongle → debug 主机 USB-A |
| 硬件投资 | Intel DCI OOB dongle (￥1000-1500) |
| 安全风险 | DCI 绕过所有 IOMMU/TPM/Secure Boot，物理接触即 root |
| 成本效益 | 专用硬件，只在 SparrowOS 调试场景有用 |

**决策**：**否决。** 启动停止需要进 BIOS + 插拔 dongle；硬件过于专用；
安全风险大于收益。除非漏洞修复需要核武器级别的访问，否则不用。

---

## 3. USB DBC (xHCI Debug Capability)

**发现**：BIOS 中 USB Dbc Enable Mode = Enabled。00:0d.0 USB4 xHCI 可开启 DBC。

**评估**：
- 零硬件成本（一根 Type-C 线）
- 类似超级 UART，比 UART 快
- 但没有 halt CPU / 硬件断点能力

**决策**：**搁置。** 不是最优的调试基座。

---

## 4. Trace Hub (00:1f.7)

**发现**：00:1f.7 Non-Essential Instrumentation, Meteor Lake-P Trace Hub。

**评估**：
- MIPI STP 单向 trace dump
- 不能做网卡或双工通信
- 协议僵化，hack 成本高

**决策**：**否决。**

---

## 5. e1000e I219-LM + SpDB (最终选择)

**决策**：以 e1000e MAC 帧为物理层基座，构建 SpDB（SparrowOS Debug Bridge）
私有调试协议。

### 选择理由

| 理由 | 说明 |
|------|------|
| 协议栈最浅 | MAC 帧直发，不需要 TCP/IP 栈 |
| 代码量最小 | TX/RX descriptor ring + DMA 搬运 ≈ 400 行 |
| 上手时机 | PCI 枚举完成后即可用，不依赖其他子系统 |
| 硬件需求 | QEMU 可验证，实体机只需一根网线 |
| 协议深度 | e1000e MAC 帧 + SpDB 帧头（magic/stream/type/seq） |
| 可扩展 | MAC 层可叠加 TCP/IP 或其他流 |
| 多机需求 | 只有一台机器时 QEMU 验证，有第二台时直连跑 |

### SpDB 协议分层

```
e1000e I219-LM (物理层)
  └── Ethernet MAC 帧 (EtherType 0x88B5)
       └── SpDB 帧头
            ├── stream 0x01: kshell 管道
            ├── stream 0x02: logcat (DmesgRingBuffer)
            ├── stream 0x03: gdbstub 远程调试
            └── stream 0x04: memdump 大块物理内存
```

### 开发优先级

```
Phase 1 (现在，QEMU 可做):
  └── e1000e 初始化 + MAC 帧收发
  └── SpDB 帧协议定义 + host 侧解析工具

Phase 2 (实体机直连后):
  └── SpDB gdbstub 通道
  └── SpDB memdump 通道

Phase 3 (可选):
  └── TB ICM mailbox 代码 (有第二台 TB 机器后再测)
  └── USB DBC 通道
```

---

## 附录 A：关键硬件信息

```
00:0d.0  USB4 xHCI              → USB DBC 候选
00:0d.1  USB xDCI (7ec1)        → DWC3 从设备控制器（TSCC_ENABLE_xDCI 解锁，2026-09-06 发现，见附录 C）
00:0d.3  TB4 NHI#1              → ICM mailbox + XDomain (需第二台TB)
00:14.1  USB Device Controller  → PCH xDCI (7e7e)，DWC3 从设备控制器（enable_xDCI 解锁，2026-09-06 发现，见附录 C）
00:1f.6  I219-LM Ethernet       → SpDB 物理层 (选)
00:1f.7  Trace Hub              → 否决
```

## 附录 B：参考源码路径

```
Linux thunderbolt 驱动: ~/PS_git/custom-kernel/linux-vfio/src/linux-7.0.6/drivers/thunderbolt/
Linux e1000e 驱动:     ~/PS_git/custom-kernel/linux-vfio/src/linux-7.0.6/drivers/net/ethernet/intel/e1000e/
内核 init 协议:        ~/PS_git/OS_pj_uefi/kernel/Docs/init_v2/init_protocal_v2_initelf_specification.md
DMAR 初始化:           ~/PS_git/OS_pj_uefi/kernel/src/arch/x86_64/core_hardwares/x86_arch/DMAR.cpp
内核主 init:           ~/PS_git/OS_pj_uefi/kernel/src/arch/x86_64/boot/kinit.cpp
内存初始化:            ~/PS_git/OS_pj_uefi/kernel/src/arch/x86_64/boot/mem_init.cpp

---

## 附录 C：xDCI（DWC3 USB Device Controller）补充评估

日期：2026-09-06
来源：BIOS 解锁两个"从设备"控制器后，Linux host 侧观察 + kernel 源码（linux-7.1.5）ID 表对照

### 发现

BIOS 两项开关解锁了两颗 USB device（从设备）控制器，Linux 下均被 `dwc3-pci` 接管并注册 UDC：

| PCI | ID | 名称 | 解锁开关 | UDC |
|-----|-----|------|----------|-----|
| 00:14.1 | 8086:7e7e (rev 20) | PCH USB Device Controller | enable_xDCI | dwc3.10.auto |
| 00:0d.1 | 8086:7ec1 (rev 10) | USB Type-C 子系统 xDCI | TSCC_ENABLE_xDCI（命名推断） | dwc3.9.auto |

映射推断依据：TSCC ≈ Type-C Subsystem Controller，0d.x 簇在 lspci 命名即 "USB Type-C Subsystem"；若实际映射相反请修正。

Linux 源码 ID 表：7e7e = `INTEL_MTL`，7ec1 = `INTEL_MTLP`（drivers/usb/dwc3/dwc3-pci.c）。
属性：`dr_mode = "peripheral"`（dwc3_pci_intel_swnode 硬编码）→ 纯 device 角色，无 host/OTG。

### 与 0d 簇其他成员的关系（Type-C/USB4 子系统全景）

```
00:0d.0  xHCI (7ec0)   → 管 usb1/usb2（20Gbps Type-C 口组）
00:0d.1  xDCI (7ec1)   → DWC3 从设备控制器 ← 本次解锁
00:0d.2  NHI0 (7ec2)   → 缺失（+ 空桥 07.0-[08-31]、06.2-[03-07]）→ USB4/TB 本体未全开
00:0d.3  NHI1 (7ec3)   → 被 Linux thunderbolt 驱动以 ICM 模式接管（印证第 1 节结论）
```

### 能力

DWC3 全功能外设控制器：可呈现任意 USB 类（ECM/RNDIS 网卡、ACM 串口、mass storage、HID、FunctionFS 自定义协议）——比 xHCI DBC（第 3 节，debug-only 通道）通用得多，属"DBC 加强版数据面"。

### 评估（对 SpDB = 裸机调试基座场景）

| 维度 | 结论 |
|------|------|
| 驱动成本 | 裸机 DWC3 device 全栈（global reg + PHY + event ring + ep0 SETUP + CDC 类）≈ 2-4k 行，远超 e1000e ~400 行 |
| 隐性依赖 | Type-C 口 device 角色需 TCPC/mux 配合；物理布线到哪个口未知（Linux 由固件/ACPI 管，裸机全自研） |
| 上手时机 | 晚：时钟/PHY/角色协商前提多，抓不到早期崩溃 |
| QEMU 可验证 | 不可（无 device-mode 控制器模型）→ 打不了 QEMU-first 迭代循环 |
| 可移植性 | 绑死 MTL + BIOS 开关 |
| 调试能力 | 无 halt/断点，与 DBC 同类 |

### 决策

**维持 SpDB (e1000e) 为调试基座**；xDCI 归入**搁置**（与 xHCI DBC 同类理由，且理由更充分：成本更高、依赖更深）。
未来若做"本机当 USB 外设"产品功能（模拟键盘/存储/NIC/串口），xDCI 是现成基座——功能开发优先级排调试基建之后，需接受平台绑定。

### 备注（Linux host 侧，与 SparrowOS 无关）

configfs 绑 ECM/RNDIS 到 `dwc3.9.auto` 后，本机可被手机/另一台 PC 识别为 USB 网卡——零成本工具玩法（手机反向 SSH 等），不影响上述决策。

---

### 追加：2026-09-09 裸机端实证——固件把 C 口钉死 data-host（决定性）

**结论更新：xDCI 从"搁置"升级为"确认否决"（作为裸机调试基座 / 本机 USB 外设功能均不可行），SpDB(e1000e) 为唯一调试基座。**

#### 实测链条（详见 workspace memory `DCI-USB-device-2026-09-09-2003.md`）

1. **固件 UCSI OPMode 报告 0x61**（GET_CONNECTOR_CAPABILITY）：port0/port1 均 = DFP + USB2 + USB3、Provider-only，**无 UFP/DRP**；且运行时 pr 能翻 [sink]（与外供电逻辑真实），但数据角色 dr **永远 [host]**——固件把 C 口建模成"数据永远 host"
2. **PD 场景也不翻**：C2C 接 PD 2.0 对端（port1 报 pd2+usb_power_delivery），dr 仍 host；PD 规范 sink→UFP 的默认角色被固件无视 → 数据角色钉死是固件策略，不是线/对端/legacy 问题
3. **对端无 PD 能力**（LpVice Vostro 3400）：其 C 口是 USB3.2 Gen1 纯数据口，Linux 下无 typec/UDC 设备、dmesg 零 C 口事件 → 不具备当 PD host 对端的条件
4. **OS 改 setup 变量被固件写保护**：efivarfs 写 Setup+0x8B6 返回 errno=1 EPERM（EFI_WRITE_PROTECTED）→ 裸机/OS 阶段无法改 BIOS 口角色，只能 BIOS 界面内改（固件自写）；被 UI 隐藏的角色项（如 USBC connector manager selection / UCSI 版本 0x8B6）更无从下手
5. **BIOS 逆向**(IFR/VarStore)翻遍 TCSS Platform Setting 表单：无直接的"端口数据角色"配置项；唯一相关 `USBC DataRole Swap Platform Disable`(Setup+0xB12) 已设 False，仅解锁 PD DR-swap 允许性，不影响 OPMode 声明

#### 对上述评估表的修正

- 冗余项："隐性依赖 Type-C 口 device 角色需 TCPC/mux 配合"→ 实测**固件层就切不动**，非 TCPC/mux 问题，而是 UCSI/EC 固件把口钉死
- 驱动成本 2-4k 行 → 仍是桎梏，但**更致命的是固件根本不放行 device 角色**：即使裸机写全 DWC3 栈，口角色仍由固件（CC/mux/data_role）控制，裸机无法绕过
- 结论：xDCI 在本机型 = **固件钉死，无法作为调试基座或 USB 外设功能**。若未来要本机当 USB 随从，需 BIOS 隐藏项改口角色（被 UI 隐藏+OS 写保护→需固件环境工具/改镜像刷写，高风险）或换支持机型

```
