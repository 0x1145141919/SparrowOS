# SpDB 排期总纲（2026-09-09 定案）

> 主线目标：以 e1000e(I219-V18 @ 8086:550b, MTP)MAC 帧为物理层，构建 SpDB
> （SparrowOS Debug Bridge, EtherType 0x88B5），作为一条千兆"高性能 IO 脐带"，
> 替换 GOP+i8042 输入输出栈，承接大块内存转储注入 / 热添加热修补 / 后续解放 iGPU。
>
> 对端 = LpVice (192.168.50.2, Realtek RTL8168), 1000Mb/s full, Linux 侧
> AF_PACKET 收发包（零内核工作）。

## 排期主线（非严格瀑布，e1000e QEMU 前期可并行）

| 序 | 工程 | 产出 | 依赖 |
|----|------|------|------|
| 1 | **ravenfs_juvenile 移植工程** | 把 host 端 juvenile_god(8.5k SLOC) 移植成裸机内核侧 RavenFS，作"上线期区段规划器" + host 可读回的磁盘布局 | BlockDevice 块后端、树 v1.1 结构体对齐、裸机分配器/容器 shim |
| 1.5 | **dmesg 无缝转储**（暂定） | 正常上下文把 DmesgRingBuffer 自动落盘（kshell 征战 I219 时背后记录全日志） | 依赖 1 的写路径 |
| 2 | **panic_dump 工程** | 预注册区段表：boot 期用 FS truncate 占位 + 量出 extent 物理区段 → panic 现场按图(sou)索骥轮询裸写，零分配零锁零 FS | 依赖 1（规划器）+ NVMe 轮询写 |
| 3 | **e1000e QEMU 战役** | 裸机 e1000e 驱动 skeleton(PCIe probe/BAR/MSI-X/TX-RX 环/DMA) + MAC 帧收发(0x88B5) | 可独立并行（走通用 e1000e MAC 路径；I219 专属 bringup 留实体机） |
| 4 | **物理机 I219 战役** | MTP I219-V18 bringup（reset/PHY/flashless-NVM shadow-RAM）+ 复用 QEMU 环/DMA 逻辑 + SpDB 四流上线 | 依赖 2 + 3；经 LpVice 千兆脐带验证 |

## 关键已定决策

- **SpDB 帧协议不过早设计**（当前仅定基调：MAC 帧 + EtherType 0x88B5 + 流型 01 kshell/02 logcat/03 gdbstub/04 memdump）。
- **IOMMU 已关**（`DMAR.cpp`）→ DMA 走 identity/phys，和 NVMe PRP 同款，无 IOVA 映射负担。
- **panic 跑不了活 FS** → FS 只做 **boot 期区段规划器**；panic 现场按预注册 phys 区段表裸写。
- **panic dump 的 FS 化**= 占位文件（truncate 定大小）→ 量 extent → 拍平注册表 → panic 按表写。比硬编码 LBA 优雅，且 host 端 RavenFS 工具链现成读回。
- **pt_blackbox（每核 Intel PT 8MB 环形）**接入注册表后 = 裸机飞行记录仪，崩溃可取每核死前指令轨迹（带 TSC 时间戳），host PT-decode 是排查 I219/页表/调度崩溃的降维武器。

## 待定/风险（钉死前不做大动作）

1. **其它核 PT tail_offset 新鲜度**：IPI_HALT 只 cli;hlt，其它核未 `disable_blackbox`，其 struct.tail_offset 可能旧 → 胜者 dump 其它核 PT 前需处理（halt 路径先 disable 或读取时钳位）。
2. **memdump 占位 = 全量 DRAM 大小**：PSLaptop 32GB，实验盘容量需确认；不足则 v1 截断或零页/空闲页跳过（草案 v2）。
3. **extent → LBA 换算 + 注册表最小字段格式**（类型/起始 LBA/长度/核号），占位文件可能 direct(连续) 或 extent(多 run)，须按实际 run 记录。
4. **NVMe 写函数优化**：轮询裸写吃注册表（无 LBA 换算）；MDTS 未存，先定 256KB 块。

## 复用面（内核已有）

- `BlockDevice`（`block_device.h`）：NVMe 每个 namespace = ops.read/write(sector,count,buf,flags)，RavenFS 端口直接当块后端。
- PCIe 枚举 + BAR 映射 / MSI-X(`msix_vec_alloc`) / DMA(phys, PRP 同款) / 中断框架 / DMAR(driver, 已关 translation)。
- `initfs` 是 initramfs 用，非 RavenFS；内核 `ravenfs/disk_struct.h` 是 v1 旧结构，需对齐 v1.1。
