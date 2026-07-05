# KVM + GDB 跨世界调试笔记

## 背景

SparrowOS 的启动链涉及三个独立的"世界"，每个世界有自己独立的内存布局和页表：

```
世界 1: OVMF 固件 (UEFI firmware)
        自己的页表，自己的代码空间

世界 2: PlayOSLoader (EFI 应用, PE32+ 格式)
        由 OVMF 在 DXE 阶段动态加载，地址由固件决定
        入口: UefiMain()

世界 3: kernel (init.elf + kernel.elf)
        先 identity map 启动 (kernel_start)
        然后 mem_init 重建完整页表（高位映射）
```

## 问题

### VS Code / MIEngine 的隐含假设

VS Code 的 C++ debug adapter (MIEngine/cppdbg) 以及 GDB 本身都假设：

> *"program 的符号地址就等于目标运行时该代码的地址，且该地址在整个 session 中不变"*

这对 99.99% 的用户态程序成立——ELF 加载器把程序放到链接地址，页表由 OS 管理对用户透明。

### 实际发生的断点时序

DAP (Debug Adapter Protocol) 协议初始化时序：

```
1. initialize       — MIEngine 返回 capabilities
2. launch           — MIEngine 启动 GDB，target remote :1234
                     → 执行 setupCommands（含 add-symbol-file kernel.elf）
3. setBreakpoints   — 用户编辑器里的断点一股脑通过 DAP 下发
                     → MIEngine 立即调用 Bind()
                     → 发 MI 命令 -break-insert -f init_init.cpp:75
                     → GDB 用已加载的 kernel.elf 符号表解析地址
                     → 发 Z0,<VA>,1 远程协议包给 QEMU
                     → QEMU 通过 guest 当前页表（OVMF 页表！）写 0xCC
4. configurationDone — MIEngine 放行 guest 执行
```

**问题在于第 3 步时，guest 还在世界 1（OVMF 固件），而断点地址属于世界 3（kernel）。**

### 三套页表互不兼容

| 时间点 | Guest 代码 | 页表 | 0xCC 写到哪里 |
|---|---|---|---|
| setBreakpoints | OVMF | OVMF 映射固件空间 | 世界 3 的 VA ←世界 1 的页表→ 要么未映射(#PF)，要么写到固件数据页 |
| UefiMain 运行 | PlayOSLoader | OVMF 的 | 还是不对 |
| kernel_start | 刚跳到 kernel | identity map (VA==PA) | 如果 kernel VA 在 identity 段，0xCC 写到对的物理页（前提是之前 0xCC 没写错） |
| mem_init 后 | kernel 新页表 | 全新高位映射 | 旧 0xCC 在新映射下可能根本不可达 |

更糟的是：GDB 的 `-f`（pending breakpoint）机制对用户态程序有效是因为"符号暂时不在已加载模块里"，等模块加载后再绑定。但 SparrowOS 的符号表在连接时就加载了（`add-symbol-file kernel.elf`），GDB 认为地址已解析，直接发 `Z0`——结果写入的是**错误的物理页**。

### 根源示意图

```
VS Code 编辑器断点
    │  setBreakpoints DAP 请求
    ▼
MIEngine
    │  Bind() → -break-insert -f ...
    ▼
GDB
    │  符号表解析 → 0xFFFFFF800000XXXX
    │  Z0,0xFFFFFF800000XXXX,1
    ▼
QEMU gdbstub
    │  cpu_memory_rw_debug() 走 GUEST 当前页表
    ▼
Guest 内存 (当前: OVMF 页表)
    │  VA 0xFFFFFF80XXXXXXXX → 未映射 / 固件区
    ▼
    ❌ 断点无效
```

## 相关源码参考

### MIEngine (microsoft/MIEngine)

断点立即下发：
- `OpenDebugAD7/AD7DebugSession.cs:2581` — `HandleSetBreakpointsRequestAsync` 中调用 `pendingBp.Bind()`
- `MIDebugEngine/AD7.Impl/AD7PendingBreakpoint.cs:166` — `Bind()` → `BindWithTimeout()` → `BindAsync()`
- `MIDebugEngine/Engine.Impl/Breakpoints.cs:110-119` — `PendingBreakpoint.Bind(file, line, ...)` 发 `-break-insert`
- `MICore/CommandFactories/MICommandFactory.cs:448` — `BuildBreakInsert()` 构造 `-break-insert -f`
- `MIDebugEngine/Engine.Impl/DebuggedProcess.cs:543-615` — `Initialize()` 执行 setupCommands

GDB 连接时序：
- `OpenDebugAD7/AD7DebugSession.cs:1110-1230` — `HandleLaunchRequestAsync`
- `src/OpenDebugAD7/AD7DebugSession.cs:1487` — `configurationDoneTCS`

### QEMU gdbstub

- `gdbstub/gdbstub.c:2062` — `gdb_handle_packet()`, switch 分派 Z/z 包
- `gdbstub/gdbstub.c:1166` — `handle_insert_bp()` → `gdb_breakpoint_insert()`
- `gdbstub/system.c:643-644` — `gdb_breakpoint_insert()` → `ops->insert_breakpoint()`
- `accel/kvm/kvm-accel-ops.c:111` — `ops->insert_breakpoint = kvm_insert_breakpoint`
- `accel/kvm/kvm-all.c:3847` — `kvm_insert_breakpoint()` 写 0xCC, 调 `KVM_SET_GUEST_DEBUG`
- `target/i386/kvm/kvm.c:6131` — `kvm_arch_insert_sw_breakpoint()` 写 0xCC 到 guest 内存
- `target/i386/kvm/kvm.c:6240` — `kvm_handle_debug()` 处理 KVM_EXIT_DEBUG
- `target/i386/kvm/kvm.c:6293` — `kvm_arch_update_guest_debug()` 设 KVM_GUESTDBG_USE_SW_BP

## 可行的调试方案

### 方案 1: 内联 int3（最糙但有效）

在 `UefiMain` 和 `kernel_start` 入口各插一条：

```c
__asm__ volatile("int3");
```

配合 KVM guest debug：需要在 QEMU 启动时加上 `-s` 参数。当 guest 执行到 `int3` 时，KVM 产生 `KVM_EXIT_DEBUG`。由于断点不在 QEMU 的 `kvm_sw_breakpoints` 列表里，`kvm_handle_debug()` 会转发给 guest 自己的 #BP handler——如果 OVMF 的 #BP handler 不存在会 triple fault。

**更好的做法**：在 `KERNEL_GUEST_DEBUG` 模式下，KVM 设置了 `KVM_GUESTDBG_USE_SW_BP`，所有 #BP 都会被 KVM 拦截并退出到 QEMU，不经过 guest IDT。GDB stub 收到后发送 `T05` 停止包。这样 int3 可以直接停住。

### 方案 2: 两阶段手动 GDB

```bash
# QEMU 启动
qemu-system-x86_64 ... -accel kvm -s -S ...
```

```gdb
# 终端 1: GDB 阶段 1 — 等 PlayOSLoader
(gdb) target remote :1234
(gdb) # 设硬件断点在已知的 PlayOSLoader 加载地址
(gdb) hbreak *0xPHYSICAL_ADDR_OF_UEFI_MAIN
(gdb) continue
```

```gdb
# 终端 1: GDB 阶段 2 — 此时 kernel 已加载
(gdb) # 读 LastLoadedElfEntryPoint 获取 kernel entry
(gdb) # 或用物理地址硬件断点
(gdb) hbreak *$KERNEL_ENTRY_PA
(gdb) continue → 停到 kernel_start
```

### 方案 3: VS Code 绕行

由于 VS Code 无法控制断点下发时机，可以：

1. 启动时不设任何断点
2. `-s -S` 启动 QEMU
3. VS Code 附加调试后，手动在 DEBUG CONSOLE 用 `-exec` 命令下断点
4. 或者将 VS Code 作为纯 GDB UI：用 `lldb.miDebuggerPath` 脚本包装 gdb，在连接后先不自动下断点

### 方案 4: 修改 launch.json 裸连

不要设 `program`，也不要加 `add-symbol-file` setupCommands。先用纯 GDB 连上，手动加载符号：

```json
{
    "name": "QEMU GDB Debug (manual)",
    "type": "cppdbg",
    "request": "launch",
    "program": "",  // 不设 binary
    "miDebuggerServerAddress": "localhost:1234",
    "stopAtEntry": false,
    "setupCommands": []
}
```

然后在 VS Code 的 DEBUG CONSOLE 里手动：

```
-exec target remote :1234
-exec add-symbol-file path/to/kernel.elf
# 等跳转到 kernel 后再设断点
```

### 方案 5: GDB 脚本自动化阶段

编写 `debug_stages.gdb`：

```gdb
# Stage 1: 在 PlayOSLoader 入口停
# （需要知道加载地址，或者用 OVMF 的 LoadImage 断点）
hbreak *0xBASE_ADDR
commands
  silent
  # Stage 2: kernel 已加载，下真实断点
  hbreak *0xKERNEL_ENTRY
  continue
end
continue
```

## 总结

SparrowOS 的调试困境源于：

1. **多内存布局** — OVMF、PlayOSLoader、kernel 三者各有独立页表和地址空间
2. **VS Code/MIEngine 的急迫性** — 连接阶段就一股脑下发所有断点
3. **GDB 符号过早解析** — `-break-insert -f` 时符号表已加载 → GDB 认为地址已确定 → 直接发 Z0 到 target → 写入错误物理页

这不是任何单个工具（GDB/QEMU/KVM/VS Code）的 bug，而是它们共享的隐含假设与 SparrowOS 架构的不兼容。解决方案是用硬件断点 (`hbreak`) 配合物理地址，或在代码中嵌入 `int3` 陷阱作为分阶段门控。
