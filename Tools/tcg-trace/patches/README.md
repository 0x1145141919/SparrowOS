# QEMU 本地补丁

## `qemu-ioport80-magic-bp.patch` — ioport80 魔法断点 / 停机

**效果**：guest 写 `outb(0x80, 0xDB)` → QEMU `vm_stop(RUN_STATE_DEBUG)`：
**暂停整机、进程不退出** → QMP 仍可用 → 可 `dump-guest-memory` 抓
**全 RAM + 全 CPU 寄存器**。这是 SparrowOS「首爆即冻结、现场可 dump」的底座。

- **基线**：QEMU `v11.0.2`（`e545d8bb9d`），改动 `hw/i386/pc.c` 的 `ioport80_write`，4+1 行。
- **为什么不用 `isa-debug-exit` / ACPI 关机**：那两者会让 QEMU 进程直接结束 → QMP 消失 →
  RAM/寄存器全丢，只剩 `.trace`。断点只暂停，两者兼得。
- **端口冲突**：`0x80` 是 POST code 口，固件理论上可能写 `0xDB`。实测 OVMF（本仓库启动链）
  **不触发**（补丁构建全程跑到 `kshell>`）。换固件时需重测。

### 应用 / 构建

```bash
# 源码树
cd /home/PS/PS_git/qemu
git checkout v11.0.2                     # 基线
patch -p1 < <kernel>/Tools/tcg-trace/patches/qemu-ioport80-magic-bp.patch
./configure --target-list=x86_64-softmmu  # <按本机既有配置>
ninja -C build
# 产物：build/qemu-system-x86_64  （版本串带 -dirty）
```

### ⚠️ 部署纪律（本品被坑过）

**不要把编译产物 `cp` 进 `/usr/bin`。** 手工部署的二进制会被 `pacman -Syu` 直接覆盖
（2026-08-29 Arch 升 qemu 11.1.1，覆盖了 7/12 的补丁构建，此后整轮 WRAITH 狩猎跑的
都是**没断点的原版**）。

正确做法二选一：

1. **显式路径（推荐，零依赖）**：编译产物留在源码树，用
   `QEMU_BIN=/home/PS/PS_git/qemu/build/qemu-system-x86_64 ./tcg-trace.sh …`
   —— `tcg-trace.sh` 已支持 `QEMU_BIN` 环境变量，跑前请用 `--selfcheck` 自检。
2. **本地包 + 钉版本**：把补丁打进本地 PKGBUILD，配 `IgnorePkg = qemu-system-x86`
   防 Arch 覆盖。

### 自检

```bash
QEMU_BIN=<编译产物> ./tcg-trace.sh --selfcheck
# 通过 ⇒ 断点活着（探针镜像 outb(0x80,0xDB) 后 QMP status == debug）
```
