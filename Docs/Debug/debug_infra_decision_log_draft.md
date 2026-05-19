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
00:0d.3  TB4 NHI#1              → ICM mailbox + XDomain (需第二台TB)
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
```
