#!/bin/bash
# vm_gdb_attach.sh — 从 QEMU gdbserver 获取 ravenfs 模块地址并启动 GDB
#
# 用法:
#   1. QEMU 以 -s 启动（Debug 模式已有 -s -S）
#   2. 在 guest 里：sudo insmod ravenfs.ko
#   3. 在 host 运行本脚本

VM_PORT="2222"
VM_USER="ps"
MODULE="ravenfs"
MODULE_SRC="/home/PS/PS_git/OS_pj_uefi/kernel/ravenfs/${MODULE}.ko"
GDB="/usr/bin/gdb"
VMLINUX="/usr/lib/modules/$(uname -r)/build/vmlinux"

set -euo pipefail

echo "=== 获取模块各 section 地址 ==="

# 通过 SSH 从 guest 读取模块 sections
get_section() {
    ssh -p "$VM_PORT" "$VM_USER@localhost" \
        "cat /sys/module/$MODULE/sections/$1" 2>/dev/null || echo "0"
}

TEXT_ADDR=$(get_section .text)
DATA_ADDR=$(get_section .data)
BSS_ADDR=$(get_section .bss)

if [ "$TEXT_ADDR" = "0" ] || [ "$TEXT_ADDR" = "0x0" ] || [ -z "$TEXT_ADDR" ]; then
    echo "错误: 无法获取模块地址，确认已 insmod？"
    echo "  ssh -p $VM_PORT $VM_USER@localhost 'sudo insmod ~/ravenfs_test/ravenfs.ko'"
    exit 1
fi

echo "  .text  $TEXT_ADDR"
echo "  .data  $DATA_ADDR"
echo "  .bss   $BSS_ADDR"

# 生成 GDB 命令脚本
GDB_SCRIPT=$(mktemp)
cat > "$GDB_SCRIPT" << EOF
set architecture i386:x86-64
file $VMLINUX

# 连 QEMU gdbserver
target remote :1234

# 加载模块符号
add-symbol-file $MODULE_SRC $TEXT_ADDR -s .data $DATA_ADDR -s .bss $BSS_ADDR

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
echo "ravenfs 已加载。可用断点:\n"
echo "  b ravenfs_fill_super\n"
echo "  b ravenfs_readdir\n"
echo "  b ravenfs_btree_search\n"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
EOF

echo "=== 启动 GDB ==="
exec $GDB -x "$GDB_SCRIPT"
