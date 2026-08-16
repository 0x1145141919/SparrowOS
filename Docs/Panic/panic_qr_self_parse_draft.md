# Panic QR 自解析（on-device）设计草案

> 最终版 — 2026-08-10

---
<!-- 审慎不落地的记录。设计方已确认大方向（两环境分流：KVM/TCG 宿主 gdb，实体机 kernel.elf 自解析），
     实现细节可被覆盖。状态：draft，不承诺过编译、过测试。升级 spec 需过编译+测试+冷却。
     本文档承接 panic_qr_dmesg_dump_draft.md（QR 基础管线），聚焦内核侧自解析扩展。 -->

## 1. 背景与动机

panic_qr_dmesg_dump_draft.md 只解决"dmesg 尾部经 QR 带走"。本草案回答：**实体机（bare metal）崩溃后，如何把栈回溯 + 局部变量 + 源码上下文全部带出**。

**决定性事实：实体机内存打捞难度逆天。**
- 没有 serial / 网络 / UEFI Runtime，QR 是唯一信息信道
- 若内核只 dump 原始帧数据，解码方必须持有**精确匹配的 kernel.elf + 构建树**，失去自包含性
- 实体机现场没有构建环境 → **kernel.elf 必须自力更生**：自己解析 DWARF + 读源码，生成自包含的注释报告，QR 装结果

**KVM/TCG 不重复劳动：**
- 宿主有 gdb（`-s` attach 到 halt 的 CPU）擦屁股解析栈，且 serial 有全量日志
- 策略树对 KVM/TCG 直接**禁用 QR 渲染**（见 §6）

**参照源：`~/PS_git/gdb`**（binutils-gdb 19.0.50-git，GPLv3，与本项目同为 GPLv3）。
许可证结论：法律零阻塞（同为 GPLv3）；做法上**参照语义重写、不逐字拷 GPL 文件**，
保留"干净模块"边界（将来想抽出模块独立立项/论文不被锁死）。dw_read 全部自有代码，头注释标注 gdb 出处即可。

## 2. 两环境两机制（已拍板）

```
KVM/TCG（开发回环）:  strategy_tree → QR_DISABLED
                      panic 走现有文本打印 → serial 全量
                      宿主 gdb 解析栈 → 无需内核 DWARF
Bare metal（实体机）:  strategy_tree → QR 按分辨率布局
                      kernel.elf 自解析：on-device DWARF+源码 → 自包含注释报告
                      QR 携带报告 → 手机拍照 → 宿主解码即得（零 build-tree 依赖）
```

## 3. 现状管线（已考古确认，集成前提）

```
入口（~30 处）：exceptions_handler、FPA/kpool/AddresSpace、scheduler、
                lapic/ioapic/DMAR、x86_vecs_deliver_mgr、kinit
  │
  ▼ Panic::panic(behaviors, message, context, panic_info, arg5)   src/arch/x86_64/panic.cpp:109
  panic_winner.add_ka(1) 抢 0                     ← 原子争胜者（prev==0 者干活）
  GlobalKernelStatus>=SCHEDUL_READY → broadcast_halt()   ← IPI_HALT(252) 冻结其它核
  每核 disable_blackbox(PT)                       ← 停处理器 trace
  胜者:
    GlobalKernelStatus=PANIC
    resources_shift()                            ← far retfq 重载 CS/段寄存器（GS 被拍平）
    填 will（magic/panic_seq/Whistleblower/KURD 或 err_locator）
    // write_will()  ← 当前被注释，未落地（panic.cpp:137）
    bsp_kout 打印: [KURD]/[ERR_LOCATOR] → message → PANIC ON → dumpregisters
                → RIP 符号 → else_trace 回溯 → task tid
  cli; hlt                                       ← 停机
```

**自解析插入点：胜者打印块之后、`cli; hlt` 之前（panic.cpp:183）。**

关键前提：
- `resources_shift` 已把 GS 拍平 → **自解析全链路禁 reentrant 锁 / `fast_get_processor_id`**
- 胜者独占全机（其它核已 IPI_HALT）→ 快照免锁（禁中断即可）
- `else_trace` 已走 rbp 链（`StackFrame{rbp, rip}`，kptrace.cpp:161），每帧有返回地址 rip + rbp
- `panic_context::x64_context` 有最内层帧完整寄存器（GPR/rsp/rbp/rip）
- Debug 构建 = `-g -O0 -gdwarf-4`，无 `-fomit-frame-pointer`，`-mgeneral-regs-only`
- 帧指针在 → 每帧 CFA = **rbp + 16**（常量关系，无需解析 .debug_frame）

## 4. 内核侧自解析核心：`dw_read`（~1500-1700 行）

新模块 `src/arch/x86_64/panic_qr/`（OBJECT 库 `panic_qr_module`，随 kernel.elf）。

### 4.1 O0 红利（决定架构的实测事实）

build/kernel.elf 中 **`.debug_loc` 仅 341B**：
- O0 下变量位置几乎全在 `.debug_info` 内联的 `DW_OP_fbreg` 常量偏移
- **不需要 location-list 解释器** → 运行时 DWARF 读取器大幅简化

### 4.2 组件与参照

| 组件 | 参照源（gdb） | 内核实现 |
|------|--------------|----------|
| DWARF 常量/opcode/forms | `include/dwarf2.h`（独立 C 头） | 自有枚举（按 DWARF 规范） |
| LEB128 解码 | `dwarf2/leb.h`（纯自包含） | ~40 行 |
| abbrev 表 + DIE 遍历 | `dwarf2/read.c` `read_die_and_children` | 迭代式、限深限宽 ~320 行 |
| 行表状态机 PC→file:line | `dwarf2/line-header.c` | v4 状态机 ~250 行 |
| 变量位置求值 | `dwarf2/expr.h` `dwarf_expr_context::execute_stack_op` | 最小集：fbreg/reg/breg/const/uconst/addr ~200 行 |
| CU/subprogram 区间 | `.debug_ranges` | ~80 行 |
| 类型打印（标量/指针/数组结构 hexdump） | `valprint.c` 语义 | ~250 行 |

### 4.3 解析流程（胜者，resources_shift 后）

```
else_trace rbp 链 {rip,rbp} ≤64 帧
每帧 rip → .debug_ranges 找 CU → 找 subprogram(DW_AT_name/file/decl_line/frame_base)
  → .debug_line → file:line
  → initramfs /src/<relpath> 读 1~3 行源码（§5）
  → 含 RIP 的 lexical block（可多层嵌套）→ 变量 DIE(name/type/location)
  → 位置求值 → CFA+offset 读栈内存 → 类型打印 name=value
最内层帧：寄存器变量也可解（x64_context 完整寄存器在）
外层帧：仅栈驻留变量（DW_OP_fbreg）；寄存器驻留变量已失 → 标注 optimized-out ★限制
```

### 4.4 位置求值最小集

只支持以下 DW_OP（DWARF4 表达式中变量位置最常见形态）：
- `DW_OP_fbreg`（偏移量相对于 frame_base = CFA = rbp+16；若 subprogram frame_base 为 `DW_OP_breg6` 则相对于 rbp）★主导
- `DW_OP_regN`（最内层帧可解，外层帧标注 register-only）
- `DW_OP_bregN`、`DW_OP_const/uconst`、`DW_OP_addr`
- `DW_OP_piece` / `DW_OP_entry_value` 等复杂形态：不实现，标注 unsupported

### 4.5 类型打印 v1

- 标量：按 base type 的 signed/unsigned + byte size 打印（int/uint/char/指针/float）
- 指针：打印 hex 值
- 数组/结构体/类：打印类型名 + 地址 + 前 N 字节 hexdump（不递归字段）
- `DW_AT_const_value` 直接打印常量

### 4.6 正确性保障

- gdb 参照语义 + 4.7 交叉验证（预留）
- 全部迭代式、限深限宽、固定静态缓冲（不 malloc）

## 5. 源码读取路径（已核实可行）

**initramfs 常驻是硬依赖**（不再是未来预留）：
- `initramfs_mark_used`（init_init.cpp:201）原位锚定 + `init_bcb_juvenile::mark_used` 钉死物理区（init_init.cpp:219）→ 永不回收
- 交接包 `initramfs_file`（movable，phase_3b.cpp:100-105）→ 内核拿到 ramfs 物理基址
- `Kspace_phyaddr_access_window` = [0,dram_top)→Kspace VA 窗口（boot.h:111）→ **panic 期经窗口直接读 initramfs 任意物理地址**
- 需在 boot 期把 `ramfs phys base` 存入全局（如 `g_panic_src_phys`）；movable 描述符消费后仍有效（物理被钉死 + 窗口全映射）

**initramfs ABI 契约**（Docs/initramfs/initramfs_specification.md，已核实 fs_format.h）：
- 三段式 [header][metadata][data]；`initramfs_header` 8×u64（magic=0x34a6346adb9e0fe2）
- `file_entry{file_size, file_path_in_metadata_offset, file_in_dataseg_offset}`，路径 NUL 结尾相对路径
- 内核侧可仿 `initramfs_lookup`（src/init/initramfs_lookup.cpp）写一个基于 Kspace 窗口的 panic 版

**源码打包形态（已拍板）**：258 个源文件作为**独立条目 `/src/<relpath>`** 进 initramfs.img，
与编译路径一致（`/src/arch/x86_64/panic.cpp` ↔ `/home/PS/.../src/arch/x86_64/panic.cpp`）。
这是 VFS 的芽孢：未来每个文件即一个 inode（initfs 已有 inode/簇体系，见 src/fs/fs_document.md）。

## 6. 策略树（QR 布局）

```
g_env（exec_env_detect.h，probe_env 已初始化并多处使用）
  ENV_KVM | ENV_TCG → QR_DISABLED（serial/hypervisor 接住；宿主 gdb）
  ENV_BARE_METAL → 按 GfxPrim::GetInfo() 分辨率：
    ≥2560×1600 → 3×2 网格（6 片 ≈17KB）
    ≥1920×1080 → 2×2 网格（4 片 ≈11.5KB）
    ≥1280×720  → 2×1 网格（2 片）
    ≥640×480   → 单 QR v40 + 截断优先（崩溃帧+最内层）
    else       → 单 QR v-mid 重截断
```
数值为初值，实机调参（拍摄容错/摩尔纹见 §12）。

**强制渲染逃生口（已拍板为正式入口）**：
- kshell `panic qr-test` + behaviors `render_qr` 强制走与裸机**完全相同**的 `panic_qr_render()` 路径
- KVM 测试即裸机验证，不另起炉灶

## 7. Payload v0.2（承接 panic_qr_dmesg_dump_draft.md §5，扩展 build-id）

```
0   8  magic "SPWQZSTR"    8   1  version=0x02
9   1  flags                10  2  fragment_index
12  2  fragment_count       14  2  header_len=56
16  4  raw_len              20  4  comp_len
24  4  crc32                28  4  panic_seq
32  20 build_id[20]         52  4  kernel_final_state
56  …  zstd 帧（注释报告 UTF-8 文本）
```
- flags：bit0=截断，bit1=zstd，bit2=多片，**bit3=含raw段（预留扩展，见 §10）**
- build-id：kernel.elf `--build-id=sha1`，宿主据此验证报告对应构建
- 多片：每 QR = [56B 头(fragment_index/count)] + zstd 帧一个 chunk；宿主按序拼回后解压一次

## 8. 构建资产管线（P0）

1. 脚本生成 initramfs_config.json files[] 追加段：`src/**` → `/src/<relpath>`（258 文件，~15MB）
2. initramfs_builder 重建 initramfs.img（build_utils/initramfs_builder，JSON 配置驱动）
3. 确认 build-id 从 kernel.elf `.note` 段提取（`--build-id=sha1` 已启用）

## 9. 宿主工具（零新增 pip：opencv 5.0.0 + libzstd 1.5.7 + zstd CLI 已在机）

| 工具 | 职责 |
|------|------|
| `build_utils/panic_qr_decode.py` | opencv 多 QR 检测/解码 → 按 fragment_index 组装 → 校验（magic/version/CRC32/build-id）→ ctypes libzstd 解压 → 输出报告文本 |
| `build_utils/panic_report_gt.py` | （预留）KVM 崩溃 → 宿主 gdb 生成地面真值报告 ↔ 强制 QR 渲染的内核报告 diff，逐帧比对变量值 |

pyelftools 未安装（且纪律要求零新增 pip）→ 宿主侧不引入。

## 10. 架构原则：链路别绑定死（已拍板）

1. **分析核心 ↔ 传输通道解耦**：dw_read + frame_walker 产出**纯内存报告缓冲**；QR 渲染、serial raw dump、
   未来任何信道都是报告缓冲的插拔消费者（抽象 `panic_report_sink`）。
2. **payload 头留扩展位**：flags `bit3=含raw段` 为"报告 + 原始帧数据双段"预留，v0.2 不破坏。
3. **强制路径是正式入口**：`panic qr-test` / `render_qr` 与裸机同一代码路径，KVM 测试即裸机验证。

## 11. 阶段划分（建议顺序）

- **P0** 构建资产：src→initramfs 打包 + config 生成 + build-id 提取 + `g_panic_src_phys` 全局接线
- **P1** panic_qr 基座：vendored zstd/shim + QR 编码器 + 宿主往返测试（panic_qr_dmesg_dump_draft.md §6/§8）
- **P2** 内核侧 `dw_read`：DWARF4 最小读取器 + 位置求值 + 类型打印（gdb 参照）
- **P3** 内核侧集成：else_trace 扩展 → 报告文本 → payload → 策略树 → GfxPrim 渲染 + `panic qr-test` 强制位
- **P4** 宿主：panic_qr_decode.py + （预留）panic_report_gt.py 交叉验证
- **P5** 实机调参（压缩级别/截断/QR 布局/屏幕拍摄容错）

P0 不涉及风险最重的 dw_read，可先起步。

## 12. 风险

- **外层帧寄存器驻留变量不可解**：O0 下常见，标注 optimized-out；最内层帧完整。实机评估接受度
- **panic 栈安全**：dw_read 迭代式、限深限宽、静态缓冲；跑在肇事任务栈上（可能很浅）
- **resources_shift 后 GS 拍平**：全链路禁 reentrant 锁 / fast_get_processor_id
- **gdb 参照≠可链接**：gdb 内部件深度耦合 target/regcache/ui-out → 只移植算法与语义
- **屏幕拍摄容错**：L 级 7% 纠错 vs 摩尔纹/反光（panic_qr_dmesg_dump_draft.md §9 已列）→ 实机验证
- **压缩/截断调参**：报告原始文本 vs zstd 压缩率 vs 单码 2953B 上限 → 实机定

## 13. 开放问题 / 待定

- [ ] initramfs 资产生命周期：movable 描述符消费后，`g_panic_src_phys` 读取路径的最终接线确认
- [ ] 源码行预算：每帧取 1 行还是 3 行；截断优先级排序（崩溃帧>最内层>外层符号链）
- [ ] KVM 交叉验证（panic_report_gt.py）是否落地：设计方"到时候再说"，接口已预留
- [ ] Release 构建降级：无 `-g` → 无 DWARF → 降级为裸符号回溯 + dmesg（QR 仍可用）
- [ ] `panic_behaviors_flags` 新增 `render_qr:1`（默认随策略树，强制位供测试）

## 14. 关联文档

- `Docs/Panic/panic_qr_dmesg_dump_draft.md` — QR 基础管线（zstd/编码器/渲染/宿主解码）
- `Docs/initramfs/initramfs_specification.md` — initramfs ABI 契约（三段式 + file_entry）
- `src/fs/fs_document.md` / `fs_file_orgnize.md` — initfs 文件系统（VFS 芽孢的未来形态）
- `~/PS_git/gdb` — 移植参照源（binutils-gdb 19.0.50-git，GPLv3）
