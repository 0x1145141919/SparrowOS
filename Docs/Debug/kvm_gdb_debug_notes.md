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

## 真正的问题

### VS Code / MIEngine 的"毕其功于一役"

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
                     → QEMU 尝试走 guest 页表插入 0xCC
                     → 翻译失败 → 断点写入永久失败
4. configurationDone — MIEngine 放行 guest 执行
```

**VS Code/MIEngine 把所有断点集中在连接初期一次性下发。** 那时 guest 还停在 OVMF 固件入口，kernel 的页表尚未激活。断点插不进去，GDB 返回失败——然后 VS Code 不再重试。

### 断点插入失败的原因

KVM 下 Z0 软件断点的插入路径：

```
GDB → Z0,0xFFFFFF800000XXXX,1 → QEMU gdbstub
  → kvm_insert_breakpoint(cpu, GDB_BREAKPOINT_SW, addr, len)
  → kvm_arch_insert_sw_breakpoint()
  → cpu_memory_rw_debug(cs, bp->pc, &int3, 1, true)  // 写 0xCC
      → x86_cpu_translate_for_debug()  // 手动漫游 guest 页表
         → 逐级读 PML5E/PML4E/PDPE/PDE/PTE
         → 遇到非 Present 条目 → 翻译失败
      → 返回 -1
  → 返回 -EINVAL
→ GDB 收到错误响应，该断点永久标记为已失败
```

关键细节：`x86_cpu_translate_for_debug()` 在 QEMU 进程内**手动漫游页表**（从 guest 物理内存读取页表项），不是通过 CPU MMU 触发缺页。OVMF 的页表中没有 kernel VA (0xFFFFFF800000XXXX) 的映射 → 翻译无条件失败 → **0xCC 根本没有被写入任何物理页**。

不存在"writes 0xCC to wrong physical page via wrong page table"的场景。结果是**断点压根没打进去**，而非写到了错的地方。

### 错误归因纠正

```
旧理解（错误）:
  "QEMU 通过 OVMF 页表把 0xCC 写到固件数据页，污染了内存"

实际:
  "cpu_translate_for_debug 在 OVMF 页表中找不到 kernel VA 的映射，
   翻译失败 → 0xCC 未被写入任何地方 → 断点永久丢失"
```

### 正确的断点窗口期

Z0 断点要成功，必须同时满足两个条件：

1. **kernel.elf 已在物理内存中** — add-symbol-file 的符号地址映射到真实的物理页
2. **kernel 的页表已激活** — `cpu_translate_for_debug()` 能通过当前 CR3 走通 VA→PA 翻译

这个窗口期只有从 `kernel_start` 开始（identity map 阶段）到永远。但 VS Code 只在最开头下发一次断点，**永远错过了这个窗口**。

```
     OVMF 世界           PlayOSLoader         kernel_start → forever
  ════════════════╤══════════════════╤══════════════════════════════
                  │                  │
  VS Code 在这    │                  │     Z0 唯一有效窗口在此
  一股脑下所有    │                  │     （VA 翻译可走通）
  断点 → 全失败   │                  │
```

## 相关源码参考

### MIEngine (microsoft/MIEngine)

- `OpenDebugAD7/AD7DebugSession.cs:2581` — `HandleSetBreakpointsRequestAsync` 中调用 `pendingBp.Bind()`
- `MIDebugEngine/AD7.Impl/AD7PendingBreakpoint.cs:166` — `Bind()` → `BindWithTimeout()` → `BindAsync()`
- `MIDebugEngine/Engine.Impl/Breakpoints.cs:110-119` — `PendingBreakpoint.Bind(file, line, ...)` 发 `-break-insert`
- `MICore/CommandFactories/MICommandFactory.cs:448` — `BuildBreakInsert()` 构造 `-break-insert -f`
- `MIDebugEngine/Engine.Impl/DebuggedProcess.cs:543-615` — `Initialize()` 执行 setupCommands

GDB 连接时序：
- `OpenDebugAD7/AD7DebugSession.cs:1110-1230` — `HandleLaunchRequestAsync`
- `src/OpenDebugAD7/AD7DebugSession.cs:1487` — `configurationDoneTCS`

### QEMU gdbstub + KVM 断点路径

- `gdbstub/gdbstub.c:2062` — `gdb_handle_packet()`, switch 分派 Z/z 包
- `gdbstub/gdbstub.c:1166` — `handle_insert_bp()` → `gdb_breakpoint_insert()`
- `gdbstub/system.c:640` — `gdb_breakpoint_insert()` → `ops->insert_breakpoint()`
- `accel/kvm/kvm-accel-ops.c:111` — `ops->insert_breakpoint = kvm_insert_breakpoint`
- `accel/kvm/kvm-all.c:3847` — `kvm_insert_breakpoint()`，参数类型 `vaddr`
- `target/i386/kvm/kvm.c:6131` — `kvm_arch_insert_sw_breakpoint()` 用 `cpu_memory_rw_debug()` 写 0xCC
- `system/physmem.c:4030` — `cpu_memory_rw_debug()` → `cpu_translate_for_debug()` → `address_space_rw()`
- `target/i386/helper.c:255` — `x86_cpu_translate_for_debug()` 手动走页表漫游
- `target/i386/kvm/kvm.c:6240` — `kvm_handle_debug()` 处理 KVM_EXIT_DEBUG
- `target/i386/kvm/kvm.c:6293` — `kvm_arch_update_guest_debug()` 设 KVM_GUESTDBG_USE_SW_BP

### 端口 IO 跳出 BQL

- `accel/kvm/kvm-all.c:3523` — `KVM_EXIT_IO` 标注 `/* Called outside BQL */`
- `accel/kvm/kvm-all.c:3532` — `KVM_EXIT_MMIO` 也 outside BQL



## 总结

SparrowOS 的调试困境核心原因：

1. **VS Code/MIEngine 毕其功于一役** — 所有断点在最开始一次性下发，错过 kernel 页表激活后的唯一有效窗口
2. **GDB 符号过早解析** — `add-symbol-file kernel.elf` 加载符号表后，`-break-insert -f` 立即解析地址并发 Z0。但此时 OVMF 页表中无 kernel VA 映射 → `cpu_translate_for_debug` 翻译失败 → 断点永久丢失
3. **VS Code 不重试失败断点** — GDB 返回错误后，MIEngine 标记该断点为失败，不会在 CR3 切换后重新尝试

解决方案的核心思路：**用不依赖页表的机制在正确窗口打开时停住 guest（而非在最开始全量下发），然后在那个时刻手动下发 Z0/hbreak 断点。** 以下方案 6 提供了这个机制。

## 方案 6: 端口强制断点 (IO Port Magic Break)

### 原理

修改 QEMU 的 `ioport80_write` handler，使其在匹配到特定 magic 值时调用 `vm_stop(RUN_STATE_DEBUG)`，触发 GDB stub 发送 T05 停止包。

```
Guest: outb(0xDB, 0x80)    ← 任意内存布局下均可执行
  │  KVM_EXIT_IO → QEMU 端口分发
  ▼
ioport80_write(0x80, 0xDB, 1)
  │  data == DBUG_BREAK 匹配
  │  bql_lock(); vm_stop(RUN_STATE_DEBUG); bql_unlock();
  ▼
gdb_vm_state_change(): 检测 state→STOPPED
  │  allow_stop_reply == true  (由 GDB 的 'c' 命令设)
  ▼
发 T05 给 GDB → 弹提示符
```

端口魔法不需要维护任何 GDB stub 状态。GDBState 的 `allow_stop_reply` 在 guest 运行期间（GDB 发了 `c`/`vCont` 之后）为 `true`。KVM 的 IO handler 运行在 vCPU 线程、不在 BQL 内，所以 `ioport80_write` 必须手动 `bql_lock()` 后再调 `vm_stop()`。

### x86 IO 端口宽度语义

真实 x86 硬件上每个 IO 端口是 8 位宽（Intel SDM Vol.1 §17）。`OUT` 指令的 operand size 决定访问的连续端口数：

| 指令 | 访问的端口 | 写入的字节 |
|---|---|---|
| `outb(al, 0x80)` | 0x80 | byte 0 (al[7:0]) |
| `outw(ax, 0x80)` | 0x80-0x81 | 0x80←byte0, 0x81←byte1 |
| `outl(eax, 0x80)` | 0x80-0x83 | 0x80←byte0, 0x81←byte1, 0x82←byte2, 0x83←byte3 |

### Magic 端口协议

```c
#define DBUG_BREAK   0xDB   // 无条件断点
#define IO_DEBUG_PORT 0x80

// 在代码中使用:
outb(DBUG_BREAK, IO_DEBUG_PORT);
```

Magic 值 0xDB：OVMF 不走此口（UEFI 调试用串口），SeaBIOS POST code 范围 0x00-0xFF 但 SparrowOS 用 OVMF。

### QEMU 修改

只改 `hw/i386/pc.c`：

```c
static void ioport80_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    if (data == DBUG_BREAK) {
        bql_lock();
        vm_stop(RUN_STATE_DEBUG);
        bql_unlock();
    }
}
```

### SparrowOS 启动链上的锚点位置

port magic 的插入点覆盖整个启动链。每个锚点停住后可以下 Z0/hbreak 断点用于下一阶段调试。

| # | 锚点位置 | 源文件 | 插入点 | 当时页表 | 停住后可做什么 |
|---|---------|--------|--------|---------|---------------|
| ① | init.elf 入口 | `init_entry.asm` `_init_entry:` | `outb(0xDB, 0x80)` 放在 `mov cr3, rax` 之后 | 恒等映射 (VA=PA 0~4TB) | 调试 init 自身启动流程(GDT/IDT/Phase 1-3a/3b/4) |
| ② | CR3→KMMU 后, 跳 kernel 前 | `init_init.cpp` `phase_45_finalize()` | 放在 `4.5-3: mov cr3, root` 之后, `4.5-6: init_jump_to_kernel` 之前 | KMMU (恒等+高半) **← 关键窗口** | **下 Z0 断点于 kernel.elf 任意符号** (此际 kernel VA 首次可翻译) |
| ③ | kernel_start 入口 | `kinit.cpp` `kernel_start()` | `outb(0xDB, 0x80)` 作为函数第一行 | KMMU | 调试 kernel 早期初始化(中断/调度器/...) |
| ④ | AP 启动入口 | 各 AP 唤醒点 | 各 AP 入口处 | KMMU | 调试多核启动 |

#### ⭐ 锚点② 详解（唯一关键窗口）

此锚点的角色是抓住 Z0 断点从"不可用"变为"可用"的那个精确时刻。在此之前(锚点①)，恒等映射仅覆盖 VA=PA 空间，kernel.elf 的符号地址 `0xFFFFFF80XXXXXXXX` 不在其中 → `x86_cpu_translate_for_debug` 翻译失败 → Z0 无法插入。在此之后，KMMU 页表同时映射了恒等区和高半区 → Z0 翻译成功。

```
恒等映射活跃          KMMU 活跃
│                    │
▼ 锚点①             ▼ 锚点②              ▼ kernel_start
───────────────────────────────┬─────────────────
  Z0(高半地址)→失败    │  Z0(高半地址)→成功  Z0(高半地址)→成功
                       │
                 CR3 切换
```

停住后的标准操作：

```gdb
# 此刻 CR3 已切到 KMMU，kernel.elf 的 VA 映射就绪
(gdb) break init_init.cpp:101          # 成功！
(gdb) break kinit.cpp:kernel_start     # 成功！
(gdb) continue                         # 继续执行，断点命中
```

### 全流程：一次典型的调试会话

```
QEMU: -s -S -accel kvm
GDB:  target remote :1234
      不下任何断点（VS Code 断点也全禁用）
      continue

Guest 跑
  │
  ├── [_init_entry 入口]
  │     outb(0xDB, 0x80)    ← 可选锚点①
  │     CR3 ← root_table (恒等映射)
  │     init() → Phase 1-4...
  │
  ├── [phase_45_finalize 4.5-3]
  │     mov cr3, root (KMMU)
  │     outb(0xDB, 0x80)    ← ⭐ 锚点② — 关键窗口
  │     │  此时下 Z0 断点于 kernel 符号全部成功
  │     │  init_jump_to_kernel → kernel.elf
  │     ▼
  │
  ├── [kernel_start 入口]
  │     outb(0xDB, 0x80)    ← 可选锚点③
  │     中断接管, 调度器初始化...
  │
  │  标准 Z0 断点触发 → 调试全链路工作
```

#### 使用示例

```c
// src/init/init_init.cpp, phase_45_finalize(), 4.5-3 之后, 4.5-6 之前:
    asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");
    outb(0xDB, IO_DEBUG_PORT);  // ← 锚点②: 关键窗口，停住后部署 Z0 断点

// 或直接插在 4.5-6 最后:
    init_jump_to_kernel(&ctx);
    // (never reached)
```

```asm
; src/init/init_entry.asm, _init_entry:
_init_entry:
    mov rax, root_table
    mov cr3, rax
    outb 0xDB, 0x80              ; ← 锚点①: init.elf 入口
```

### 三层对比

| 特征 | int3 (Z0) | hbreak (DR) | port magic (outb 0x80) |
|---|---|---|---|
| 依赖页表写 0xCC | ✅ 需要当前页表映射 VA | ❌ 只需物理地址 | ❌ 不依赖 |
| 需 GDB 事先配置 | ✅ Z0 包插入 kvm_sw_breakpoints | ✅ 设 DR0-DR3 | ❌ 无状态，触发即停 |
| 影响 KVM 性能 | ✅ KVM_GUESTDBG_ENABLE 降速 | ✅ KVM_GUESTDBG_ENABLE 降速 | ❌ 不设 KVM_SET_GUEST_DEBUG |
| 有效窗口 | kernel 页表激活后 | 任意世界（物理地址） | 任意世界任意时刻 |
| 用途 | 正式调试 | 轻量/物理断点 | **窗口门控 + 时机确认** |

### 相关源码

- `hw/i386/pc.c` — `ioport80_write` 定义
- `accel/kvm/kvm-all.c:3847` — `kvm_insert_breakpoint()` 入口
- `target/i386/kvm/kvm.c:6131` — `kvm_arch_insert_sw_breakpoint()` 用 `cpu_memory_rw_debug()` 写 0xCC
- `system/physmem.c:4030` — `cpu_memory_rw_debug()` 实现
- `target/i386/helper.c:255` — `x86_cpu_translate_for_debug()` 手动页表漫游
- `system/cpus.c:741` — `vm_stop()` 实现
- `gdbstub/gdbstub.c:987-997` — `run_cmd_parser` 设 `allow_stop_reply`
