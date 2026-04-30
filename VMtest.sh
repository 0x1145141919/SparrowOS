#!/bin/bash

# 配置基础路径
BASE_DIR="/home/PS/PS_git/OS_pj_uefi"
VM_RESOURCES="${BASE_DIR}/VMresources"

# 配置路径变量
ESP_DIR="${VM_RESOURCES}/tmp-efi"
OVMF_PATH="${VM_RESOURCES}/OVMF.fd"
BOOTLOADER_SRC="${BASE_DIR}/edk2/PlayOSLoaderPkg/BUILD/DEBUG_GCC5/X64/PlayOSLoaderPkg.efi"
KERNEL_SRC="${BASE_DIR}/kernel_agent_kshell_branch/kernel.elf"
KSYM_SRC="${BASE_DIR}/kernel_agent_kshell_branch/ksymbols.bin"
INIT_SRC="${BASE_DIR}/kernel_agent_kshell_branch/init.elf"
ROOT_PARTION="${VM_RESOURCES}/arch-root.qcow2"
EFI_PARTION="${VM_RESOURCES}/efi_partion.img"

# QEMU 配置
MECHAIN_CONFIG="-bios $OVMF_PATH -smp 6  -drive file=$EFI_PARTION,format=raw,if=none,id=efi_disk -device nvme,serial=beefdead,drive=efi_disk -machine q35,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on,aw-bits=39,eim=on,device-iotlb=on -cpu max,+x2apic -device virtio-net-pci,bus=pcie.0,netdev=net0 -device intel-hda,bus=pcie.0 -drive file=$ROOT_PARTION,format=qcow2,if=none,id=nvme_disk -device nvme,serial=deadbeef,drive=nvme_disk -netdev user,id=net0 -m 8192 -device vfio-pci,host=01:00.0"

# 设置错误处理
set -euo pipefail

# 错误处理函数
handle_error() {
    echo "错误发生在第 $1 行，退出状态码 $2"
    echo "清理可能存在的挂载点..."
    [ -d "$ESP_DIR" ] && sudo umount "$ESP_DIR" 2>/dev/null || true
    sudo losetup -d /dev/loop5 2>/dev/null || true
    exit 1
}

trap 'handle_error ${LINENO} $?' ERR

# 检查参数
if [ $# -ne 1 ]; then
    echo "用法: $0 <Debug|Release>"
    exit 1
fi

# 验证必要文件存在
check_file_exists() {
    if [ ! -f "$1" ]; then
        echo "错误: 文件 $1 不存在"
        exit 1
    fi
}

check_file_exists "$OVMF_PATH"
check_file_exists "$BOOTLOADER_SRC"
check_file_exists "$KERNEL_SRC"
check_file_exists "$ROOT_PARTION"
check_file_exists "$EFI_PARTION"
check_file_exists "$KSYM_SRC"
check_file_exists "$INIT_SRC"

# 准备ESP目录
echo "设置循环设备..."
sudo losetup /dev/loop5 -P "$EFI_PARTION" || {
    echo "错误: 无法设置循环设备"
    exit 1
}

echo "挂载EFI分区..."
sudo mount /dev/loop5p1 "$ESP_DIR" || {
    echo "错误: 无法挂载分区"
    sudo losetup -d /dev/loop5
    exit 1
}

echo "复制内核和引导加载程序..."
sudo cp "$KERNEL_SRC" "$ESP_DIR/kernel.elf" || {
    echo "错误: 无法复制内核文件"
    sudo umount "$ESP_DIR"
    sudo losetup -d /dev/loop5
    exit 1
}

sudo cp "$BOOTLOADER_SRC" "$ESP_DIR/PlayOSloader.efi" || {
    echo "错误: 无法复制引导加载程序"
    sudo umount "$ESP_DIR"
    sudo losetup -d /dev/loop5
    exit 1
}

sudo cp "$KSYM_SRC" "$ESP_DIR/ksymbols.bin"||{	
    echo "错误: 无法复制内核符号文件"
    sudo umount "$ESP_DIR"
    sudo losetup -d /dev/loop5
    exit 1
}
sudo cp "$INIT_SRC" "$ESP_DIR/init.elf"||{	
    echo "错误: 无法复制内核初始化文件"
    sudo umount "$ESP_DIR"
    sudo losetup -d /dev/loop5
    exit 1
}
echo "卸载分区并清理循环设备..."
sudo umount "$ESP_DIR" || {
    echo "警告: 无法正常卸载分区，尝试强制卸载"
    sudo umount -f "$ESP_DIR"
}

sudo losetup -d /dev/loop5 || {
    echo "警告: 无法正常删除循环设备"
}

# 启动QEMU
echo "启动QEMU..."
if [ "$1" = "Release" ]; then
    sudo qemu-system-x86_64 -no-reboot $MECHAIN_CONFIG -enable-kvm -serial stdio
else
    sudo qemu-system-x86_64 -no-reboot $MECHAIN_CONFIG  -s -S -serial stdio -D fault_log.txt -d cpu_reset,int,guest_errors
fi 
