# Panic QR dmesg dump 设计草案

> 最终版 — 2026-08-04

---
<!-- 审慎不落地的记录。设计方已确认大方向，实现细节可被覆盖。
     状态：draft，不承诺过编译、过测试。升级 spec 需过编译+测试+冷却。 -->

## 1. 背景与动机

现状 panic 的信息出口是 GOP 文本控制台 + 串口。GOP 只能输出可视化文本，
信息密度太低：一屏放不下崩溃前最后一刻的上下文，且要手动抄录。

目标：仿照 Linux `drm_panic`，panic 时把 **dmesg 压缩后编码进一个二维码**，
直接画到 GOP 帧缓冲上。不需要串口线、不需要第二台机器，手机拍照即可带回
完整（尾部）日志，宿主端解码还原。

**为什么是 QR 而不是保留文本：**
- 单码容量最大 2953B（v40-L），压缩后等效携带 10~60KB 原始日志（看压缩率）
- 拍照解码是成熟的手机能力，解码工具链宿主侧可做全套
- 完全不依赖 UEFI Runtime / 网络 / 串口，纯本地

## 2. 现状 panic 管线（已考古确认，作为集成前提）

```
入口（~30 处）：exceptions_handler、FPA/kpool/AddresSpace、scheduler、
                lapic/ioapic/DMAR、x86_vecs_deliver_mgr、kinit
  │
  ▼ Panic::panic(behaviors, message, context, panic_info, arg5)   src/arch/x86_64/panic.cpp:109
  panic_winner.add_ka(1) 抢 0                     ← 原子争胜者（prev==0 者干活）
  GlobalKernelStatus>=SCHEDUL_READY → broadcast_halt()   ← IPI_HALT(252) 冻结其它核
  每核 disable_blackbox(PT)                       ← 停处理器 trace，防 halt 后继续写
  胜者:
    GlobalKernelStatus=PANIC
    resources_shift()                            ← far retfq 重载 CS/段寄存器（GS 被拍平）
    填 will（magic/panic_seq/Whistleblower/KURD 或 err_locator）
    // write_will()  ← 当前被注释，未落地
    bsp_kout 打印: [KURD]/[ERR_LOCATOR] → message → PANIC ON → dumpregisters
                → RIP 符号 → else_trace 回溯 → task tid
  cli; hlt                                       ← 停机
```

**QR 渲染的插入点：胜者打印块之后、`cli; hlt` 之前。**

关键前提（决定设计约束）：
- `resources_shift` 已把 GS 拍平 → **panic 路径不能依赖任何 GS 槽位 / 带 pid 的锁**
- kout 在 `PANIC` 态跳过锁、统一走各后端 `panic_write`
- `DmesgRingBuffer` 持 2MB 环形 dmesg（`log_buffer`，kout `panic_write` 已持续灌入）
- `GfxPrim` 已 Init（backbuffer + Flush，BGRA32），可用 PutPixel/FillRect
- 胜者独占全机（其它核已 IPI_HALT）→ 快照 dmesg 可免锁

## 3. 已拍板决策

| 项 | 决策 | 理由 |
|----|------|------|
| 压缩 | **zstd**（Linux 内核内嵌版） | 压缩率最狠；linux-7.1.5 `lib/zstd` 源码在盘上且为 freestanding 设计 |
| 分发 | **单 QR 截断** | 贴近 Linux drm_panic，实现最简；装最新日志段 |
| EC 等级 | **L** | 最大容量 2953B，屏幕照片像素足够清晰 |

未拍板/待定项见 §9。

## 4. 数据流（目标形态）

```
panic → 胜者冻结CPU / 关PT / resources_shift → 现有打印
  → [新] DmesgRingBuffer::snapshot_latest()       免锁，取最新连续段到 staging
  → [新] zstd 压缩最新窗口
        （迭代：压缩 → 超 2953B 则输入减半重压，直到能装下或到下限）
  → [新] payload 封装（32B 头 + zstd 帧）
  → [新] QR 编码（byte 模式 / 自动选版 ≤v40 / RS 纠错 / 掩码打分）
  → [新] GfxPrim 绘制（白色底 + 居中 QR，module 按屏幕尺寸缩放）+ Flush
  → cli; hlt
```

## 5. Payload 格式（宿主/内核共同契约）

32 字节定长头 + zstd 帧，自描述，不依赖任何外部元数据：

```
offset  大小  字段              说明
0       8    magic            0x535057515A535452 ("SPWQZSTR")
8       1    version           0x01
9       1    flags             bit0=截断; bit1=zstd
10      2    reserved          =0
12      4    raw_len           原始 dmesg 字节数（未压缩）
16      4    comp_len          zstd 帧字节数
20      4    crc32             原始字节的 CRC32
24      4    panic_seq         will.panic_seq
28      4    reserved          =0
32      …    zstd 帧           压缩的 dmesg 尾部
```

截断标记置位时，表示"原始日志超过可编码窗口，只保留最新 raw_len 字节"。

## 6. 组件设计

### 6.1 内核侧 — `src/arch/x86_64/panic_qr/`（新增 OBJECT 库）

按模块纪律挂进 kernel.elf（`$<TARGET_OBJECTS:panic_qr_module>`），
与根 `SRC_COMMON` 正交，仅 KERNEL_MODE 编译。

| 文件 | 职责 |
|------|------|
| `qr_encode.{h,cpp}` | 自研 QR 编码器：Byte 模式、容量表自动选版 ≤40、GF(256) Reed-Solomon、8 掩码打分、format/version 信息。静态缓冲，不 malloc |
| `zstd_shim.{h,cpp}` | 静态 arena 分配器（BSS，约 4MB）+ 对 Linux zstd 的 `ZSTD_compress2` 封装；`zstd_deps.h` 替换为纯 freestanding 版 |
| `panic_qr_dump.{h,cpp}` | 入口 `panic_qr_render()`：快照 → 压缩截断循环 → payload → 编码 → GfxPrim 绘制 + Flush |
| `outsbmodules/zstd/` | vendored zstd 源码（linux-7.1.5 `lib/zstd` 的 `common/`+`compress/`，去 module 包装，保留 BSD-3/GPL 双许可头） |

### 6.2 zstd 接入要点

- 使用 Linux 内核内嵌版：`zstd_deps.h` 本来就是"替换 libc 依赖"的钩子
- 分配：`ZSTD_DEPS_NEED_MALLOC` 映射为 always-fail，全部走调用方提供的
  `ZSTD_customMem` → panic 静态 arena
- 只编 `common/` + `compress/`，kernel 侧不需要 decompress
- 需要写 `linux/*.h` 的 freestanding shim（NULL/limits/string/err 等），DEBUGLEVEL 置 0
- 压缩级别从 level 9 起步，windowLog 按 staging 尺寸封顶，实机调参
- 风险最大，见 §9

### 6.3 QR 编码器要点

- 只实现 byte 模式 + L 级纠错（当前需求），但容量表保留全等级（M/Q/H 预留）
- module matrix 上限 177x177（v40），静态 bool 数组
- 正确性靠 §8 的宿主往返测试兜底（opencv 解码作参照）
- 若自研验证失败，退路：移植 Nayuki qrcodegen（MIT）单文件

### 6.4 GOP 渲染要点

- `GfxPrim::Ready()` 守卫，未就绪则跳过 QR（退化只打文本）
- module 像素 = 由屏幕分辨率算最大整数倍，留 4-module quiet zone，居中
- 白底矩形 + 黑/白 module，最后 `Flush()`
- 不依赖 textconsole_GoP（其服务线程在 panic 下不保证运行），直接用 GfxPrim 原语

## 7. 宿主工具链（本次的核心交付之一）

`build_utils/panic_qr_decode.py`，零新增 pip 依赖：

| 步骤 | 手段 |
|------|------|
| QR 检测+解码 | `cv2.QRCodeDetector`（cv2 5.0 已在宿主机） |
| payload 校验 | magic / version / CRC32 |
| zstd 解压 | `ctypes` 调 `/usr/lib/libzstd.so`（宿主已有），备选 subprocess `zstd -d` |
| 输出 | 还原 dmesg 文本 + 头部信息（panic_seq / 截断标记 / 长度） |

## 8. 验证策略（足量测试）

1. **QR 编码器正确性**：宿主自测程序编译同一份 `qr_encode.cpp` → 出 PBM →
   `panic_qr_decode.py`（opencv）解码 → 字节比对
2. **zstd 正确性**：zstd 封装宿主编译 → 压样本 → ctypes 解压比对；与 `zstd` CLI 交叉验证
3. **QEMU 端到端**：kshell 命令 `panic qr-test <payload>` 渲染测试 QR →
   `screendump` → 走完整解码链 → 比对
4. **实机 panic**：真机触发 → 手机拍照 → 解码还原
5. **链路回归**：1+2 并入构建目标，宿主脚本自动跑

## 9. 开放问题 / 风险

- [ ] **zstd shim 是最大风险**：linux 头文件链入面广，需精简 shim。缓解：DEBUGLEVEL 0、
      只编 compress、`zstd_deps.h` 官方留口
- [ ] **压缩级别 / staging 尺寸**：level 9 vs 19 的压缩率与 arena 空间权衡，实机定
- [ ] **截断语义**：取最新多少日志？先定"迭代减半直到装下"，上限 staging 尺寸（拟 512KB）
- [ ] **QR 自研编码器正确性**：靠往返测试；不过则换 qrcodegen
- [ ] **panic 中 GfxPrim 可用性**：backbuffer 是否已就绪取决于 panic 时机，需 Ready() 兜底
- [ ] **与现有遗言机制的关系**：`write_will()` 未落地，QR 是否要一起补（设计方定）
- [ ] **截图/照片容错**：L 级 7% 纠错，屏幕摩尔纹/反光会不会压垮解码——实机验证
- [ ] **是否需要 panic_behaviors_flags 新增位**：`render_qr:1`（默认开）还是无条件渲染

## 10. 实施顺序（建议）

1. vendored zstd + freestanding shim + 宿主 zstd 交叉验证（§8.2）
2. QR 编码器 + 宿主往返测试（§8.1）
3. panic_qr_dump + DmesgRingBuffer::snapshot_latest + GfxPrim 渲染
4. Panic::panic 集成 + behaviors 位
5. 宿主解码工具链 `panic_qr_decode.py`
6. kshell 测试命令 + QEMU 端到端（§8.3）
7. 实机验证 + 按 §9 调参

## 11. 配套改动

- `src/include/panic.h`：`panic_behaviors_flags` 新增 `render_qr:1`
- `src/arch/x86_64/panic.cpp`：胜者块打印后插入 `panic_qr_render()`
- `src/utils/kcirclebufflogMgr.{h,cpp}`：新增免锁 `snapshot_latest()`（禁中断即可，
  胜者独占全机，不走 rwlock——`resources_shift` 后 GS 无效，锁的 pid 依赖会挂）
- `src/arch/x86_64/core_hardwares/CMakeLists.txt`（或根 CMake）：注册 `panic_qr_module`
