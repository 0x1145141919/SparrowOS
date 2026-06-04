#!/bin/bash
# sync_to_vm.sh — 编译 ravenfs 并推送至 QEMU 虚拟机 test env
#
# 用法: ./sync_to_vm.sh [ravenfs|mkfs|all]
#   ravenfs  只编译并推送内核模块
#   mkfs     只推送 mkfs 工具
#   all      推送全部（默认）

VM_HOST="localhost"
VM_PORT="2222"
VM_USER="ps"
VM_DIR="/home/${VM_USER}/ravenfs_test"

KERNEL_DIR="/home/PS/PS_git/OS_pj_uefi/kernel"

set -euo pipefail

target="${1:-all}"

case "$target" in
    ravenfs|all)
        echo "=== 编译 ravenfs.ko ==="
        make -C "$KERNEL_DIR/ravenfs" -j$(nproc) 2>&1 || { echo "编译 ravenfs 失败"; exit 1; }
        ;;
esac

case "$target" in
    mkfs|all)
        echo "=== 编译 mkfs.ravenfs ==="
        make -C "$KERNEL_DIR/user_utils" mkfs.ravenfs 2>&1 || { echo "编译 mkfs 失败"; exit 1; }
        ;;
esac

echo "=== 推送至虚拟机 ($VM_USER@$VM_HOST:$VM_PORT) ==="
ssh -p "$VM_PORT" "$VM_USER@$VM_HOST" "mkdir -p $VM_DIR" 2>/dev/null || {
    echo "SSH 连接失败。确认虚拟机已启动且有 SSHD？"
    echo "  ssh -p $VM_PORT $VM_USER@$VM_HOST"
    exit 1
}

scp -P "$VM_PORT" "$KERNEL_DIR/ravenfs/ravenfs.ko"   "$VM_USER@$VM_HOST:$VM_DIR/" 2>/dev/null || true
scp -P "$VM_PORT" "$KERNEL_DIR/ravenfs/ravenfs.h"    "$VM_USER@$VM_HOST:$VM_DIR/" 2>/dev/null || true
scp -P "$VM_PORT" "$KERNEL_DIR/user_utils/mkfs.ravenfs" "$VM_USER@$VM_HOST:$VM_DIR/" 2>/dev/null || true

# 如果 test.img 存在也推过去
if [ -f "$KERNEL_DIR/test.img" ]; then
    scp -P "$VM_PORT" "$KERNEL_DIR/test.img" "$VM_USER@$VM_HOST:$VM_DIR/" 2>/dev/null || true
fi

echo "=== 完成 ==="
echo "在 VM 中:"
echo "  cd $VM_DIR"
echo "  sudo insmod ravenfs.ko"
echo "  dmesg | tail"
echo "  sudo ./mkfs.ravenfs -f /dev/nvme1n1   # 或 test.img"
echo "  sudo mount -t ravenfs /dev/nvme1n1 /mnt/test"
