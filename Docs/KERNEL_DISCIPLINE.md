# KERNEL_DISCIPLINE.md — SparrowOS 内核工程纪律

> 考古来源：CMakeLists.txt + cmake 模块 + linker scripts + include 目录结构 + 代码样本 + git log
>
> 状态：draft（等待确认）

---

## 一、构建系统纪律

### 1.1 工具链

| 工具 | 要求 | 来源 |
|------|------|------|
| CMake | ≥ 3.15 | `cmake_minimum_required(VERSION 3.15)` |
| C | C23 | `-std=c23` |
| C++ | C++23 | `-std=c++23` |
| 汇编 | NASM, elf64, DWARF debug | `toolchain.cmake` |
| 外部库 | gnu-efi (submodule) | `outsbmodules/gnu-efi` |

### 1.2 模块化构建模式

```
每个子系统 = CMake OBJECT 库 → 链接到 kernel.elf / init.elf
```

标准模块结构：

```
src/<module>/
  ├── CMakeLists.txt       ← 定义 Xxx_module OBJECT 库
  ├── file1.cpp
  ├── file2.cpp
  └── ...
```

根 CMakeLists.txt 通过 `add_subdirectory()` 引入各模块，然后通过 `$<TARGET_OBJECTS:Xxx_module>` 注入 kernel.elf 和 init.elf。

**强制编译选项（每个 OBJECT 库必须包含）：**

```
-nostdlib
-ffreestanding
-fno-use-cxa-atexit
-fno-exceptions
-fno-threadsafe-statics
-fno-builtin
-fno-rtti
-fno-PIC
-mgeneral-regs-only
-mno-red-zone
-fno-stack-protector
-DKERNEL_MODE
-DPGLV_4
-DPRINT_OUT              # 某些模块
$<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
$<$<CONFIG:Release>:-O2>
```

### 1.3 两 ELF 世界

| 镜像 | 入口点 | 链接脚本 | 职责 |
|------|--------|----------|------|
| `init.elf` | `_init_entry` | `init.ld` | UEFI boot → 探测硬件、构建页表、加载 kernel.elf |
| `kernel.elf` | `_kernel_Init` | `kld.ld` | 运行时内核：调度器、驱动、内存管理 |

各自独立编译、独立链接。kernel.elf 的基地址通过 `PAGE_LEVEL_4 = 0xFFFF800000000000` 映射到高半区。

### 1.4 链接纪律

- `-nostdlib -static -lgcc`
- `-Wl,--build-id=sha1`（内核）
- `--wrap=__stack_chk_fail`（栈保护劫持到 panic）
- 所有 `.ld` 文件放在根目录，模块级仅在特殊情况下自建（如 `pid_module.ld`）

---

## 二、目录结构与命名纪律

### 2.1 源文件目录

```
src/
  ├── arch/x86_64/                 ← 架构相关（x86-64 专用）
  │   ├── boot/                    ← 启动入口（kinit、kernel_entry.asm、mem_init）
  │   ├── core_hardwares/          ← 硬件驱动（子目录：NVMe、PCIe、i8042、x86_arch）
  │   ├── Interrupts/              ← 中断子系统
  │   ├── Processor/               ← 处理器管理
  │   └── panic.cpp                ← 恐慌处理
  ├── firmware/                    ← ACPI、UEFI RT 服务
  ├── fs/                          ← 文件系统（initramfs、initfs）
  ├── include/                     ← 全局头文件
  │   ├── abi/                     ← ABI 稳定接口（boot.h、os_error_definitions.h、arch_code.h）
  │   ├── arch/x86_64/             ← 架构相关头文件
  │   ├── firmware/                ← 固件相关
  │   ├── init/                    ← init.elf 专用
  │   ├── memory/                  ← 内存管理
  │   ├── Scheduler/               ← 调度器
  │   └── util/                    ← 工具类（kout, lock, Ktemplats, bitmap...）
  ├── init/                        ← init.elf 源码
  ├── kmods/                       ← 内核模块框架
  ├── memory/                      ← 内存管理实现
  ├── scheduler/                   ← 调度器实现
  ├── tests/                       ← 测试代码
  ├── utils/                       ← 工具实现
  └── virtualzition_about/         ← 虚拟化相关
```

### 2.2 头文件路径

**全局头文件根：** `src/include/`

所有 `.cpp` 文件引用头文件时，相对于 `src/include/` 路径：

```cpp
#include "Scheduler/task.h"              // ← src/include/Scheduler/task.h
#include "arch/x86_64/abi/base.h"        // ← src/include/arch/x86_64/abi/base.h
#include "util/lock.h"                   // ← src/include/util/lock.h
#include "memory/AddresSpace.h"          // ← src/include/memory/AddresSpace.h
```

### 2.3 命名约定

| 项目 | 风格 | 示例 |
|------|------|------|
| **文件/目录** | snake_case | `per_processor_scheduler.cpp`, `kpoolmemmgr.cpp` |
| **类名** | PascalCase | `NVMe_Controller`, `per_processor_scheduler` 混合（历史遗留） |
| **方法/成员函数** | snake_case | `set_ready()`, `get_tid()`, `pop_head()` |
| **字段** | snake_case | `belonged_processor_id`, `min_wakeup_stamp` |
| **常量** | UPPER_SNAKE_CASE | `LOCKED`, `UNLOCKED`, `BQ_ID_INVALID` |
| **枚举/枚举值** | 混合模式 | `event_type_t { init, run_kthread, wait_io }` |
| **命名空间** | 单层小写 / 嵌套 | `NVMe::`, `HPET::regs::`, `Ktemplats::` |
| **宏/编译开关** | UPPER_CASE | `KERNEL_MODE`, `PGLV_4`, `PRINT_OUT` |

### 2.4 头文件规范

```
#pragma once              ← 强制（不使用 #ifndef 守卫）
#include <...>             ← 系统头（极少）
#include "..."             ← 项目头，相对 src/include/
```

---

## 三、编码风格纪律

### 3.1 零标准库依赖

SparrowOS 是 freestanding 内核，**禁止使用 libc/libc++/STL**：

| 标准特性 | 替代方案 |
|----------|----------|
| `malloc`/`free` | `kpoolmemmgr` 内核分配器 |
| `<vector>` | `Ktemplats::list_doubly`, `rb_map`, `sparse_table_2level` |
| `<mutex>` | `spinlock_cpp_t`, `reentrant_spinlock_cpp_t`, `spinrwlock_cpp_t` |
| `<string>` | 裸指针 + 长度 |
| `new`/`delete` | `placement new`（`src/init/init_heap_v3.cpp`），placement new 全局对象 |
| `<iostream>` | `kout` 子系统 |
| 异常/栈展开 | `-fno-exceptions` |
| RTTI | `-fno-rtti` |

### 3.2 KURD 错误模型（五层）

```
module_code → in_module_location → event_code → result → reason
```

所有内核函数返回 `KURD_t（uint64_t 编码）`。详见 `Docs/ModuleErrorTree.md`。

### 3.3 锁纪律

锁类型（定义在 `src/include/util/lock.h`）：

```
spinlock_cpp_t                    ← 简单自旋锁
reentrant_spinlock_cpp_t          ← 可重入自旋锁（pid + 深度编码）
spinrwlock_cpp_t                  ← 读写自旋锁
interrupt_guard                   ← 中断开关 guard
spinlock_interrupt_about_guard    ← 关中断的自旋锁 guard
spinrwlock_interrupt_about_*_guard ← 关中断的 RW 锁 guard
```

已知锁序约束（来自历史记录 & 注释）：
```
bq_lock (RW) > qlock (per-BQ spinlock) > task_lock > sched_lock
cq_wq_lock > sq_lock               // NVMe
wq_lock → task_lock                // block_queue (两方向统一)
```

### 3.4 禁止清单

```
❌ RTTI（dynamic_cast, typeid）
❌ C++ 异常（try/catch/throw）
❌ 栈保护（-fstack-protector）
❌ 线程局部存储
❌ PIC/PIE
❌ 红区（-mno-red-zone）
❌ libc/libc++ 标准库函数
❌ 全局 C++ 构造函数（静态初始化顺序未定义）
→ 使用 placement new 手动初始化全局对象
```

允许的 libgcc 符号：
```
❓ __stack_chk_fail → 通过 --wrap 劫持到 panic
✅ lgcc（整数除法、memcpy 等底层辅助）
```

### 3.5 ASM 纪律

- 汇编后缀 `.asm`（通过 `set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS asm)` 注册）
- NASM elf64 格式：`-f elf64`
- 汇编搜索路径：`-I${CMAKE_CURRENT_SOURCE_DIR}/src/ -I${CMAKE_CURRENT_SOURCE_DIR}/src/arch/x86_64/boot/`
- 支持 DWARF debug symbols
- 标准的 `.asm` 文件模式：
  - 内核入口：`src/arch/x86_64/boot/kernel_entry.asm`
  - 异常入口：`src/init/Interrupts/Sysdef_exception_entries.asm`
  - 处理器管理：`src/arch/x86_64/Processor/runtime_processor_regist.asm`, `fast_get_ids.asm`
  - 原子操作：`src/arch/x86_64/Processor/cmpxchg16b.asm`

---

## 四、头文件目录索引

### ABI 层（`include/abi/`）

| 文件 | 内容 |
|------|------|
| `os_error_definitions.h` | KURD 五层模型：`module_code`, `domain`, `level_code`, `result_code`, `err_domain` 命名空间 |
| `boot.h` | init.elf → kernel.elf 信息传递结构 |
| `arch_code.h` | 架构代码标识符 |

### 架构相关（`include/arch/x86_64/`）

| 文件 | 内容 |
|------|------|
| `abi/base.h` | 基础类型定义 |
| `abi/GS_complex.h` | GS 段复合结构体 |
| `abi/GS_Slots_index_definitions.h` | GS 槽位索引 |
| `abi/msr_offsets_definitions.h` | MSR 偏移定义（`namespace msr`）|
| `abi/pgtable45.h` | 4/5 级页表（`namespace PML4E`, `PDPTE` 等）|
| `abi/pt_regs.h` | 处理器上下文/寄存器帧 |
| `boot.h` | 架构引导信息 |
| `mem_init.h` | 内存初始化接口 |
| `intel_processor_trace.h` | Intel PT 调试 |
| `exec_env_detect.h` | KVM/TCG 环境检测 |

### 核心硬件（`include/arch/x86_64/core_hardwares/`）

| 文件 | 内容 |
|------|------|
| `NVMe/base.h` 及相关 | NVMe 驱动 |
| `PCIe/base.h` + `prased.h` | PCIe 枚举与解析 |
| `i8042.h` | 键盘控制器 |
| `PortDriver.h` | 串口驱动 |
| `lapic.h`, `ioapic.h` | APIC |
| `HPET.h` | 高精度定时器 |
| `DMAR.h` | DMA 重映射 |
| `tsc.h`, `rtc.h` | 时间管理 |

### 内存管理（`include/memory/`）

| 文件 | 内容 |
|------|------|
| `kpoolmemmgr.h` | 内核池分配器 |
| `AddresSpace.h` | 地址空间 |
| `FreePagesAllocator.h` | 空闲页分配器 |
| `phyaddr_accessor.h` | 物理地址访问 |

### 调度器（`include/Scheduler/`）

| 文件 | 内容 |
|------|------|
| `task.h` | 任务结构体、状态机 |
| `task_pool.h` | 任务池（RBTree）|
| `per_processor_scheduler.h` | 每 CPU 调度器 |
| `bq_system.h` | Block Queue 系统 |
| `kthread_abi.h` | 内核线程 ABI |

---

## 五、linker 布局

### kernel.elf（`kld.ld`）

```
ENTRY(_kernel_Init)
KImgvbase = PAGE_LEVEL_4 (= 0xFFFF800000000000)
KImgphybase = 0x2000000
Koffset = KImgvbase - KImgphybase       // 虚拟-物理偏移
__KImgSize = 128M

内存区域：
  VRAM (rwx): origin = KImgvbase, length = 128M
  init  (rx): origin = 0x4000, length = 1M (AP bootstrap)

段：
  .ap_bootstrap_*   → init 区域     ← AP 启动代码（物理地址）
  .text              → VRAM
  .rodata            → VRAM
  .data              → VRAM
  .bss               → VRAM
  .heap (NOLOAD)     → VRAM (4M)
  .klog (NOLOAD)     → VRAM (2M)
```

### init.elf（`init.ld`）

```
ENTRY(_init_entry)
INIT_BASE = 0x100000000 (4GB)
INIT_IMAGE_SIZE = 16M
INIT_STACK_SIZE = 64K
INIT_HEAP_SIZE = 2M
```

---

## 六、外部依赖管理

| 依赖 | 来源 | 管理方式 |
|------|------|----------|
| gnu-efi | `outsbmodules/` git submodule | ExternalProject（`Build_IN_SOURCE TRUE`, 跳过 configure）|
| tinySTL | `outsbmodules/` | 内核独立编译（部分测试用）|
| EDK2 (PlayOS bootloader) | `../edk2/PlayOSLoaderPkg` | `build_bootloader.sh` 外部脚本编译 |

### 已知外部引用（从 CMakeLists.txt 可见）

- `/usr/include/efi/` — 某些模块私有头文件路径（遗留，待清理？）
- `-lgcc` — 链接到 libgcc

---

## 七、测试约定

测试代码位于：

```
src/tests/
  ├── kthreads/                  ← 内核线程测试
  ├── test_kpoolmemmgr/         ← 内存池测试
  └── CMakeLists.txt            ← 测试构建
build/
  ├── bcb_replay                ← BCB 回放测试
  ├── BCB_test                  ← BCB 测试
  ├── fs_test                   ← 文件系统测试
  ├── kspace_vm_table_test      ← 虚拟内存表测试
  ├── test_mem                  ← 内存测试
  └── test_set_dump             ← 测试集 dump
```

用户态测试通过独立 CMake 目标编译，链接到宿主编译器而非内核工具链。

---

## 八、模块化错误定位

所有内核模块的错误码按 `Docs/ModuleErrorTree.md` 五层模型组织：

```
module_code ∈ { kernel, init, memory, NVMe, PCIe, x86_arch, i8042, ... }
```

参见 `src/include/abi/os_error_definitions.h` → `namespace module_code`。

---

## 九、已知技术债务 / 遗留约定

1. **`-fno-use-cxa-atexit`** — 禁用全局析构，内核不优雅退出，只有 panic
2. **`PRINT_OUT` 宏** — 控制 kout 输出的条件编译开关，某些模块有、某些模块没有，未统一
3. **`#include <efi.h>`** — 某些内核模块直接引用 EFI 头，耦合了 init 世界的 ABI
4. **`linker_for_kernel.ld`** — 存在但未使用（`kld.ld` 是活跃的）
5. **`src/include/init/`** 与 **`src/include/arch/x86_64/init/`** 的边界模糊，`init` 和 `arch/x86_64/init` 均有同层头文件
6. **`outsbmodules/tinySTL`** — 似乎不再活跃，确认是否需要保留

---

## 十、角色边界

| 职责 | 负责方 |
|------|--------|
| 构建配置、接口签名 | 设计方确认，AI 可生成草案 |
| 模块 CMakeLists.txt 编写 | AI 可生成，设计方审核 |
| KURD 模块错误树编排 | 设计方编排拓扑，AI 生成枚举代码 |
| 纪律文档维护 | AI 考古推断→draft→设计方确认→spec |
| 外部依赖决策 | 设计方拍板 |

---

*本文档基于代码考古结果编写。所有纪律点均需设计方确认后升为 spec。*
