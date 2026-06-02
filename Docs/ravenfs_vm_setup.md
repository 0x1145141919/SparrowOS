# RavenFS VM 测试环境搭建

## 一句话流程

```
Host: make → scp → QEMU (已改好 SSH 转发) → Guest: insmod + mount
```

## Step 1: 准备 Guest Linux 虚拟机

你的 arch-root.qcow2（32G）需要内装 Arch Linux + SSH。现有镜像可能已有系统，
但不确定内容。重建方法：

```bash
# 安装 arch-install-scripts（如果还没有）
sudo pacman -S arch-install-scripts

# 创建一个临时挂载点
mkdir -p /tmp/vmroot

# 挂载 qcow2
sudo modprobe nbd max_part=8
sudo qemu-nbd --connect=/dev/nbd0 arch-root.qcow2
sudo mount /dev/nbd0p1 /tmp/vmroot   # 或 p2，视分区情况而定
```

如果 qcow2 是裸文件系统（无分区表）：

```bash
sudo modprobe nbd
sudo qemu-nbd --connect=/dev/nbd0 arch-root.qcow2
# 查看有什么分区
sudo fdisk -l /dev/nbd0
sudo mount /dev/nbd0p1 /tmp/vmroot
```

检查已有内容：

```bash
ls /tmp/vmroot/
cat /tmp/vmroot/etc/os-release   # 看是不是 Arch
```

如果**已经**是 Arch 系统，跳过 Step 2，直接进 Step 3 配置。

如果要**完全重建**：

```bash
# 格式化 qcow2
sudo mkfs.ext4 arch-root.qcow2
sudo mount arch-root.qcow2 /tmp/vmroot

# pacstrap 最小系统
sudo pacstrap /tmp/vmroot base linux linux-firmware openssh vim sudo gcc make

# 生成 fstab
sudo genfstab -U /tmp/vmroot | sudo tee /tmp/vmroot/etc/fstab

# 进入 chroot 配置
sudo arch-chroot /tmp/vmroot

# 在 chroot 里：
echo "raven-test" > /etc/hostname
passwd root              # 设 root 密码
useradd -m ps
passwd ps                # 设 ps 密码
echo "ps ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/ps
systemctl enable sshd
exit

# 清理
sudo umount /tmp/vmroot
sudo qemu-nbd --disconnect /dev/nbd0
```

## Step 2: 启动虚拟机

```bash
# 改好的 VMtest.sh 已经含 SSH 转发
cd ~/PS_git/OS_pj_uefi/kernel
sudo ./VMtest.sh Release
# QEMU 启动后 SSH 端口转发即生效
```

## Step 3: 确认 SSH 通

```bash
# 另一个终端
ssh -p 2222 ps@localhost
# 第一次可能要确认 ~/.ssh/known_hosts 冲突
ssh-keygen -R "[localhost]:2222"
```

## Step 4: 编译 + 推送

```bash
cd ~/PS_git/OS_pj_uefi/kernel/user_utils
./sync_to_vm.sh all
```

## Step 5: 在 Guest 测试

```bash
# 在虚拟机里：
cd ~/ravenfs_test
sudo insmod ravenfs.ko
dmesg | tail -5
# → 应该看到 "ravenfs: module loaded"

# 创建测试镜像（不碰真盘）
dd if=/dev/zero of=test.img bs=1M count=256
./mkfs.ravenfs test.img
sudo mount -t ravenfs -o loop test.img /mnt/test
ls /mnt/test
echo "hello world" > /mnt/test/hello.txt
cat /mnt/test/hello.txt
sudo umount /mnt/test
```

## Step 6: 真盘测试（01:00.0）

```bash
# 1. Host 上切回 VFIO
sudo ./nvme_dev_switch.sh vfio

# 2. 启动 VM，vfio-pci 直通 01:00.0
sudo ./VMtest.sh Release

# 3. 在 VM 里：mkfs 直接在真盘上
sudo ./mkfs.ravenfs /dev/nvme1n1   # ← 真实的 2TB NVMe
sudo mount -t ravenfs /dev/nvme1n1 /mnt/test
# 随便读写，重启后数据保留

# 4. 切回 Host 模式
#    关 VM → Host 上切回 host 模式
sudo ./nvme_dev_switch.sh host
#    然后 host 上也能 mount 同一块盘
sudo mount -t ravenfs /dev/nvme1n1 /mnt/raven
```

## 注意

- `mkfs.ravenfs` 还不完整（只写了 superblock + bgdt，B+tree 初始化未完成）
- 现在跑 `insmod ravenfs.ko` 后 mount 会识别但 readdir 还不是真的遍历
- 等你实现完 B+tree 和 inode 操作后，mkfs 也要同步更新
