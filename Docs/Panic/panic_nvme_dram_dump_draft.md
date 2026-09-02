# Panic NVMe 全量 DRAM Dump 设计草案

> 状态：draft — 调研规划，未落地。升级 spec 需过编译+测试+冷却。
> 本文档使以下三篇进入"降级/被替代"通道：
> - `Panic_Memory_Broadcast.md`（PT 链式救援）→ 被全量 dump 替代（panic 现场 PT 缓冲仍在 RAM 里）
> - `panic_qr_dmesg_dump_draft.md`（dmesg QR 带走）→ 被替代（dmesg 原始数据在 dump 里，宿主直接提取）
> - `panic_qr_self_parse_draft.md`（设备端 DWARF 自解析）→ 被替代（宿主 gdb + 全量 RAM 镜像，保真度更高）

## 1. 动机

三篇旧方案都是"无存储信道"假设下的产物：QR 单码容量 ~3KB（多片网格也就十几 KB），
PT 救援要靠 warm reset 保 DRAM + 空闲页搭链。现在 NVMe 驱动已可用（CTRL_READY），
panic 时直接把**整个物理 DRAM** 写到盘上，一次拿到：

- 内核镜像（含 .data/.bss 全部全局量）
- 全部页表（CR3 根表 → 4 级结构，宿主可离线重建 VA↔PA 映射）
- 两大内存元数据：`pages_arr`（page_frame_state_mgr 账本）、FPA BCB 位图/统计、kpoolmemmgr 堆
- 全部线程栈（task_pool 内所有 task 内核栈）
- **PT 环形缓冲区**（每核 8MB，`global_pt_blackboxes`）
- dmesg 环形缓冲（`DmesgRingBuffer`，2MB）、VM_intervals 表、`will` 遗言、panic 现场寄存器

宿主侧拿 kernel.elf（含 DWARF）+ 全量 RAM 镜像 = Linux kdump / Windows crash dump 级别的
事后分析能力，且**完全离线、零拍摄、零解码链**。

## 2. 可行性调研（已考古确认）

### 2.1 panic 管线现状（插入点）

```
panic()  [src/arch/x86_64/panic.cpp:109]
  ├─ panic_winner.add_ka(1) 抢 0
  ├─ broadcast_halt()          ← IPI_HALT(252) → 其它核 cli;hlt（中断关死，不会被 MSI-X 唤醒）
  ├─ disable_blackbox(本核 PT)  ← 停录，tail_offset 已刷
  ├─ 胜者: GlobalKernelStatus=PANIC
  │    resources_shift()       ← far retfq 重载 CS + ds/es/ss/fs/gs=16 → GS 被拍平
  │    填 will（magic/panic_seq/Whistleblower/KURD）
  │    bsp_kout 打印（kout 在 PANIC 态跳过锁）
  │    ★ [新] panic_nvme_dump_ram()   ← 插入点：打印块之后、cli;hlt 之前
  └─ cli; hlt
```

### 2.2 panic 现场可用资源

| 资源 | 状态 | 说明 |
|------|------|------|
| 中断 | IF=0 | 完成通知只能轮询 CQ phase bit，不能等 MSI-X |
| GS | 拍平（=0） | **全链路禁 `fast_get_processor_id` / 带 pid 的锁** |
| 调度器 | 已停 | `cmd_submit_and_process`（block_if_equal + bq）**不可用**，需轮询变体 |
| 主窗口 | 就绪 | `[0,dram_top)` 恒等映射，`PHYACC_VA` 全物理域可用（NVMe 驱动刚重构完也吃这红利） |
| 物理段表 | 就绪 | `phymem_segments`（write_will 已用过，panic 安全，纯内存解引用） |
| NVMe 队列 | 就绪 | CTRL_READY 时 SQ/CQ ring、doorbell、PRP 构建器全部在位 |
| 其它核 | cli;hlt | 不会干扰；NVMe 完成写 CQ ring 的是**设备**（DMA），与停核无关 |
| ktime | 可用 | panic 打印已用 `now`，进度显示/时间戳可依赖 |

### 2.3 NVMe 驱动现状（本次 dump 的复用面）

- ring 缓冲（ASQ/ACQ、I/O SQ/CQ）已走主窗口恒等映射（8-21 重构后 `vpn=PHYACC_VA>>12`），
  `vbase()` 解引用 = 1GB 大页直访 —— dump 轮询读 CQ 天然受益
- `sq_dorbell_write` = MMIO 写 + `mfence`（panic 安全）
- 完成语义：`cq_interrupt_handler` 的 phase 位翻转逻辑（head_idx / unprocessed_entry_expect）
  可直接参照实现轮询版
- **MDTS 没读没存**：`identify_ctrl` 读到 `mdts`（Identify 数据 0x4D）但没落地，
  `BlockDevice::one_time_*_limit` 从未赋值 → dump 分块要么补读 MDTS，要么保守固定 256KB
- PRP list 页当前走 `FreePagesAllocator::alloc`（PRPs.cpp）—— **panic 下不可复用**
  （FPA 可能正是肇事者；且锁依赖 GS）→ dump 需静态 BSS staging 页

### 2.4 时序估算（32GB 全量）

- WD SN5000 2TB 持续写 ~2.8GB/s（spec）；轮询模式 + 256KB 分块实估 1.5~2.5GB/s
- 传输 ≈ 13~21s；命令开销 32GB/256KB=131072 条 × ~10µs ≈ 1.3s
- **总计 ~15~20s**，可接受（屏幕打进度）。压缩（zstd）v1 不做：单核压缩 ~0.5GB/s，
  慢于裸写，且代码段不可压；列为 v2 选项（针对空闲页/零页先行跳过收益更高）

## 3. 总体架构

```
panic 胜者（打印后）
  │
  ├─ 1. 找第一个 CTRL_READY 的 NVMe 控制器（node_array），找不到 → 降级旧机制
  ├─ 2. 写 dump header 到静态 staging（4KB）：magic/panic_seq/will 快照/context 寄存器/CR3/
  │      dram_top/main_window_vbase/段表(PA 区间列表)/build-id/tsc
  ├─ 3. 按段表逐段下发 NVMe WRITE（I/O 队列 qid=1，轮询完成）：
  │      LBA = nsze - 预留扇区 + 偏移；数据 = PRP 指向的物理 RAM 页
  ├─ 4. 尾 FLUSH（opcode 00h）→ 数据落盘（否则重启后宿主读到的是 volatile cache 旧数据）
  └─ 5. 回 panic 主流程 cli;hlt
```

### 3.1 轮询写路径（核心难点，~150 行）

不复用 `cmd_submit_and_process`（依赖 bq/调度器）。直接用现成 ring 状态：

```
lock-free 轮询提交（禁锁、禁 GS）：
  sq = sqs[1]（第一个 I/O SQ，io_queue_init 成功即存在）
  cid = sq.tail_idx（固定复用同一 slot，绕开 flying_slots 账本——panic 是终态，不还原）
  sq_ring[cid] = WRITE 命令（opcode 01h, NSID, SLBA, NLB, DPTR1/2=PRP）
  sq.tail_idx = (tail+1) % num_entries；sq_dorbell_write(1, tail)
  轮询 cqs[1].cq_ring[head]：phase == unprocessed_entry_expect 且 cmd_id==cid → 完成
  更新 cq.head_idx + cq_dorbell_write（维持 phase 翻转语义，否则队列写满卡死）
```

- 命令写 SQ ring 后 `mfence` 已由 doorbell 函数保证，控制器可见
- CQ 完成是设备 DMA 写内存，x86 一致性协议保证 CPU 读到（正常中断路径同理，无需 clflush）
- 复用 slot 要处理 ring 回绕：head 追到 ring 尾时 phase 翻转逻辑照抄 `cq_interrupt_handler`
- 可选加固：先 mask 该队列 MSI-X vector（`msix_table[vec].vector_control |= 1`）
  —— 停止核已 cli;hlt 不会醒，mask 是双保险

### 3.2 零分配 PRP staging

- 静态 BSS：`alignas(4096) uint64_t prp_staging[8][512]`（16KB = 8 个 PRP list 页）
- 4KB list 页可存 511 个 entry + 1 链指针 → 单命令最大 ~2MB 数据（mps=4KB 时）
- 分块大小 = min(MDTS, 2MB, 4KB 对齐)；MPS=4KB（驱动 CC.MPS 默认 0）→ 全部 RAM 页天然对齐
- BSS 页按页映射 → 物理页天然 4KB 对齐，满足 PRP list 页要求

### 3.3 段表与 dump 范围

- 源：`phymem_segments`（全部 RAM 段，MMIO 空洞天然不在内——避免对设备地址发起读）
- 顺序：段表固定顺序写盘，header 记录每段 [PA, 长度, 偏移]，宿主按表还原
- 覆盖：内核镜像/页表/元数据/任务栈/PT/dmesg 全部自动在内（它们都是 RAM 段里的分配）

## 4. 磁盘布局与前置条件

```
┌──────────────────────────── 磁盘 LBA 空间 ────────────────────────────┐
│  OS 分区（用户数据，勿碰）        │  预留 dump 区（最后 R 扇区）      │
│                                  │  [header 4KB] [RAM 段 1..N]       │
└──────────────────────────────────┴───────────────────────────────────┘
  dump 起始 LBA = nsze - R          R = align_up(dram_top, 1MB) + 1MB
```

- **前置条件（一次性）**：宿主侧把磁盘尾部预留 R 字节空闲（缩分区/留 raw 尾区），
  SparrowOS 与宿主工具约定同一常数（如 `PANIC_DUMP_RESERVED = 36GB`，2TB 盘余量充足）
- 重复 panic 覆盖同一位置；header 里 panic_seq/时间戳标识最新一份
- 不做文件系统介入：raw 扇区写，NVMe 命名空间 LBA 空间直接访问

## 5. Dump 头格式（4KB 定长，宿主契约）

```
offset  大小  字段
0       8    magic "SPWDRMP1"
8       4    version=0x01
12      4    header_len=4096
16      8    panic_seq（will.panic_seq）
24      8    tsc_at_panic
32      8    dram_top
40      8    main_window_vbase（宿主重建恒等 VA 的关键锚点）
48      8    cr3（胜者 CR3 → 离线页表走查）
56      8    total_dump_bytes
64      8    segment_count
72      8    reserved
80      …    段表条目 ×N：{pa, length, file_offset}（8+8+8=24B/条）
…            尾部 512B：will 快照 + panic_context（寄存器全量）
4096    …    原始 RAM 段数据
```

## 6. 宿主分析工具链（零新增 pip）

`build_utils/panic_dump_tools/`：

| 工具 | 职责 | 依赖 |
|------|------|------|
| `panic_dump_extract.py` | 读设备尾部 → 校验 header → 还原段表 → 按需 carve：PT 缓冲（按 `global_pt_blackboxes` 符号+镜像内值）、dmesg（`DmesgRingBuffer::buff`）、任务栈（task_pool 走查）、页表（CR3 4 级走查） | readelf/nm subprocess + struct（零新增） |
| `panic_dump_gdb.py` | 页表走查 → 生成 ELF core（PT_LOAD 覆盖内核映射 VA 区）→ `gdb kernel.elf core` 全量事后：栈/局部变量/DWARF 源码级 | 同上 + gdb |
| `pt_carve.py` | 每核 PT ring 线性化（tail_offset 起）→ `ptxed` 解码 | ptxed（宿主已有 gdb 树） |

符号解析：用 `readelf -s`/`nm` 拿符号表（pyelftools 未装且纪律禁新增 pip，不引）。

## 7. 与旧机制的关系

| 场景 | 主路径 | 降级 |
|------|--------|------|
| panic 时 NVMe CTRL_READY | **全量 dump** | — |
| panic 早于 NVMe 初始化（boot 期） | 旧机制 | PT 链式救援（warm reset 保 DRAM 前提不变）/ QR |
| dump 中途失败（控制器卡死等） | 已写部分 + header 标记 incomplete | 宿主可解析已落盘部分；遗言 will 仍在 RAM |
| KVM/TCG 开发回环 | 无实体盘 | 照旧 serial + gdb（dump 代码按 `g_env` 禁用） |

- `Panic_Memory_Broadcast.md` 的 warm reset 保 DRAM 测试不再阻塞主线（dump 不需要重启救援）
- QR 两篇保留为"无盘"兜底，不删（panic 早于 NVMe 或 NVMe 挂掉的场景仍有用）

## 8. 阶段划分（建议）

- **P0** 轮询写路径骨架：`NVMe_Controller` 加 friend（或公开 panic 接口）+ `panic_nvme_dump.cpp`
  + 静态 PRP staging + 单命令轮询（先固定 256KB 分块，不读 MDTS）→ QEMU/实体机 `panic qr-test` 式强制入口验证
- **P1** 段表枚举 + header + 全量循环 + FLUSH + 进度输出（bsp_kout）
- **P2** 磁盘预留区（宿主侧缩分区/约定常数）+ 实机全量 dump + 时间实测（调分块/队列并行）
- **P3** 宿主工具链：extract.py（carve PT/dmesg/栈）→ gdb core 生成
- **P4**（可选）MDTS 动态读取、多 I/O 队列并行下发（8 SQ 并发 → 更快）、零页/空闲页跳过、zstd 压缩

## 9. 风险与开放问题

- [ ] **MDTS 未落地**：分块保守 256KB 安全（几乎所有盘 ≥ 256KB）；实测后决定是否读 0x4D
- [ ] **qid=1 队列状态污染**：panic 终态可接受；但要确认 io_queue_init 至少建成 1 对 I/O 队列
- [ ] **phase 回绕**：轮询逻辑必须逐字对齐 `cq_interrupt_handler` 的翻转规则
- [ ] **FPA 元数据一致性**：dump 时不锁 FPA/pages_arr，读到的是崩溃瞬间账本（这正是我们想要的）
- [ ] **预留区被覆盖**：磁盘尾部是否空闲是宿主侧前置责任，kernel 侧只按约定常数写
- [ ] **Warm reset 保 DRAM**：dump 场景不依赖，但 PT 解码依赖 tail_offset 快照正确（disable_blackbox 已做）
- [ ] **时间预算**：~15-20s 屏幕等待，进度输出每 256MB 或 2s 一次
- [ ] **多控制器/多盘**：v1 只取第一个 READY 控制器；预留区必须在该盘上（本机单盘无此问题）

## 10. 关联文档

- `Docs/Panic/Panic_Memory_Broadcast.md`（降级）、`panic_qr_dmesg_dump_draft.md`（降级）、`panic_qr_self_parse_draft.md`（降级）
- `src/arch/x86_64/panic.cpp`（插入点）、`src/arch/x86_64/core_hardwares/NVMe/`（驱动复用面）
- `src/include/memory/init_memory_info.h`（phymem_segments）、`src/include/abi/boot.h`（主窗口）
- `src/include/arch/x86_64/intel_processor_trace.h`（PT 黑匣子）
