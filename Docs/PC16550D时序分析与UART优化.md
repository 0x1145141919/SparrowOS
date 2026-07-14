# PC16550D 时序分析与 UART 驱动优化

日期：2026-07-09
背景：基于 TI PC16550D datasheet (SNLS378C) 的深度分析，
      结合 x86 实体机时序域与 QEMU/KVM 模拟差异的驱动优化记录。

---

## 1. PC16550D 物理模型

### 1.1 引脚功能群（44-pin PLCC / 40-pin PDIP）

| 功能群 | 引脚 | 说明 |
|--------|------|------|
| CPU 总线侧 | D0-D7, A0-A2, CS0-CS2, RD/RD, WR/WR | 数据、地址、片选、读写控制 |
| 时钟侧 | XIN/XOUT (1.8432 MHz 晶振), RCLK, BAUDOUT | 波特率发生器时钟源 |
| 串行侧 | SIN (串行输入), SOUT (串行输出) | RS-232 数据线 |
| Modem 侧 | DTR, RTS, DSR, CTS, DCD, RI | 握手信号 |
| 控制/状态 | MR (硬件复位), INTR, OUT1, OUT2, TXRDY/RXRDY | |
| 电源 | VDD (+5V), VSS (GND) | |

### 1.2 内部结构：发送路径

```
outb(THR) → [XMIT FIFO 16×8] → [THR 顶层] → [TSR 移位寄存器] → SOUT
                                              ↑
                                       硬件自动搬运，CPU 不可见
```

FIFO 模式下，THRE (LSR bit 5) = **XMIT FIFO 全空**，不是单个字节空。
TEMT (LSR bit 6) = XMIT FIFO + TSR 全空。

### 1.3 寄存器模型（基准地址 +0 ~ +7）

| 偏移 | DLAB=0 读 | DLAB=0 写 | DLAB=1 读写 | 章节 |
|------|-----------|-----------|-------------|------|
| +0 | RBR | THR | DLL | §8.6.1 |
| +1 | IER | IER | DLM | §8.6.6 |
| +2 | IIR | FCR | - | §8.6.4/§8.6.5 |
| +3 | LCR | LCR | LCR | §8.6.2 |
| +4 | MCR | MCR | MCR | §8.6.7 |
| +5 | LSR | - | - | §8.6.3 |
| +6 | MSR | - | - | §8.6.8 |
| +7 | SCR | SCR | SCR | §8.6.9 |

---

## 2. 完整初始化时序（Datasheet §8.6）

```c
// [1] IER = 0           — 关所有中断              §8.6.6
// [2] LCR | DLAB        — 除频访问锁存            §8.6.2
//     DLL = divisor & 0xFF
//     DLM = divisor >> 8
// [3] LCR = 0x03        — 8N1 + 关 DLAB
// [4] FCR = 0x07        — FIFO 开 + 清除 + 1B 触发 §8.6.4
// [5] MCR = 0x03        — DTR=1, RTS=1           §8.6.7 关键缺失
// [6] inb LSR            — 清残留错误位
// [7] inb RBR            — 清残留接收字节
```

关键改动：原代码 FCR=0xC7（14B trigger）→ 0x07（1B trigger，poll mode）。
          新增 MCR 初始化，原代码完全缺失。
          新增状态清除（inb LSR + inb RBR）。

---

## 3. x86 I/O 指令的时序域

### 3.1 outb 路径

```
CPU 4 GHz → store buffer (~32 entry) → DMI (8 GT/s)
  → PCH (LPC host FSM) → LPC (33 MHz, 1 cycle ≈ 120 ns)
  → Super I/O (16550 core) → WR 上升沿锁存 THR
```

outb 返回时机：CPU 等 DMI ACK (~20 ns)，不等落到 16550。
PCH 可以 posted write（SDM Vol 1 §20.6 承认）。
IN 必须同步往返到 16550，双方都是。

**IN 比 OUT 贵 50× 以上。**

### 3.2 SDM 保证

SDM Vol 1 §20.1：
> Writes to I/O ports are guaranteed to be completed before the next
> instruction in the instruction stream is executed.

SDM Table 20-1：OUT 是 serializing instruction（等所有 pending store + 等当前
store 完成才执行下条）。

但是：保证的是"离开 CPU 边界"而不是"落到 16550"。
PCH 可以 posted write，但 PCI ordering rule 保证：
  - IN 之前必须 drain 前面所有 posted write
  - 连续 OUT 保持顺序

### 3.3 时间域跨越

| 域 | 频率 | 周期 |
|----|------|------|
| CPU 内核 | 4 GHz | 0.25 ns |
| DMI 链路 | 8 GT/s | 125 ps/bit |
| PCH 内部 | ~400 MHz | 2.5 ns |
| LPC 总线 | 33 MHz | 30 ns |
| 16550 串行 | 16×115200 = 1.8432 MHz | 542 ns (bit) |

---

## 4. QEMU/KVM vs 实体机时序差异

### 4.1 QEMU serial.c 的发送模拟

关键发现：`char_transmit_time`（86.8 μs）**只用于接收侧**。
`serial_xmit` 直接调用 `qemu_chr_fe_write()`，没有延迟。
FIFO 在 QEMU 里是"无延迟 drain"的。

### 4.2 三种环境的差异

| 指标 | 实体机 | KVM | QEMU TCG |
|------|--------|-----|----------|
| outb 完成 | ~20 ns | ~1-5 μs (VM exit) | ~100-500 ns |
| IN 往返 | ~1 μs | ~2-10 μs | ~100-500 ns |
| THRE 等待 | ~87 μs/字节 | ~0 (立即可读) | ~0 |
| 逐字节 poll | 每字节 87 μs | VM exit 占大头 | 极快 |
| batch 16 | 1.4 ms | ~20 μs | 忽略 |

### 4.3 实体机 vs 虚拟机的策略选择

实体机：tx_buf 路径（batch + yield + sleep 回避忙等）
虚拟机：polling_putc 直写（THRE 瞬间回，batch 反而因 87 μs sleep 变慢）

通过 CPUID leaf 0x40000000 判断环境：
- ENV_BARE_METAL → 实体机
- ENV_KVM        → KVM
- ENV_TCG        → TCG (QEMU)

---

## 5. 运行时优化策略

### 5.1 软件 TX buffer

```c
static char g_tx_buf[16];       // 匹配 16550 XMIT FIFO 深度
static uint8_t g_tx_buf_count;

serial_tx_buf_putc(char c):
  buf[count++] = c
  if (count == 16 || c == '\n')
    serial_tx_buf_flush();

serial_tx_buf_flush():
  // 实体机: while THRE→yield → 16 outb → sleep(n×87μs)
  // 虚拟机: → 16 outb (THRE 无延迟)
```

### 5.2 双轮子设计

| 接口 | 用途 | 特点 |
|------|------|------|
| polling_putc/puts | panic/early boot | 逐字节 while THRE，零依赖 |
| serial_tx_buf_{putc,puts,p_num} | 运行时线程独享 | batch + yield + sleep |
| uart_runtime_submit_{string,num,char} | 多生产者 ring buffer | 入 ring + wakeup 服务线程 |

### 5.3 不可丢字符

内核日志的不可丢性质：
- hex 地址数字少一位 → 整条日志灾难
- 空格丢了 → token 边界消失
- 任何优化牺牲正确性都不值得

batch 16 + IN 是验证过的**正确性等价于逐字节**的最佳折中。

---

## 6. ACPI 串口发现（DBG2/DBGP）

### 6.1 表结构

DBG2 (Debug Port Table 2)：
```
Header → InfoOffset → DeviceInfo[] →
  PortType=0x8000 (16550), Subtype=0x0001 (w/ FIFO)
  BaseAddress: SpaceID=1 (SystemIO), Address=0x3F8
  AddressSize=0x20 (32 bytes, ports 0x3F8-0x3FF)
```

DBGP (Debug Port Table)：
```
Header → InterfaceType=0 (16550)
  BaseAddress: SpaceID=1, Address=0x3F8
```

### 6.2 启动链重构方向

```
init.elf:
  Phase 1: ACPI DBG2/DBGP → 发现 UART → 完整初始化 → 写入 arch_specify
kernel.elf:
  从 arch_specify 拿配置 → 验证 LCR/FCR → 注册 backend → 运行时线程
  （不做重复初始化）
```

### 6.3 备用方案：debugcon (QEMU 0xE9)

仅 QEMU 存在的 I/O 端口 0xE9。outb 直接送 chardev，零模拟开销。
实机上 0xE9 无设备认领，outb 静默消失（安全 void）。

---

## 7. 关键教训

1. **PCH 可以 posted write** — outb 返回不保证落到 16550，但 PCI ordering 保序
2. **16550 FIFO 的 THRE 在 FIFO 模式下 = "全空"** — 逐字节 poll 浪费了 FIFO
3. **QEMU serial.c 不模拟发送延迟** — 任何性能优化在 QEMU 下都无意义，功能正确性可验证
4. **IN 比 OUT 贵 50×** — 最小化 IN 次数是优化的核心方向
5. **影子 FIFO 不可靠** — 16550 没有"还剩几字节"这个信息，纯时间推测无法保证正确性
6. **实体机 IO 时序在 KVM 下完全扭曲** — VM exit 开销使 outb 变慢，但无发送延迟又使 THRE 变快
