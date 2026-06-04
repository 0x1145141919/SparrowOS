# nvme_dev_switch.sh — NVMe host/VFIO passthrough mode switcher

## 用途

将物理 NVMe SSD (`0000:01:00.0`, Sandisk WD SN5x0, 2TB) 在宿主机与虚拟机 VFIO 直通之间热切换，**无需重启**。

## 背景

该设备默认被内核参数 `vfio-pci.ids=15b7:5017` 和 `/etc/modprobe.d/vfio.d` 固定由 `vfio-pci` 驱动接管（供 SparrowOS VM 直通用）。

启动默认状态 = VFIO。脚本通过 sysfs 热切换驱动绑定，重启后恢复 VFIO 默认。

## 用法

```bash
# 查看当前状态
sudo ./nvme_dev_switch.sh status

# 切换为宿主主机模式（nvme 驱动，2TB 空间可用）
sudo ./nvme_dev_switch.sh host

# 切换为 VM 直通模式（vfio-pci 驱动）
sudo ./nvme_dev_switch.sh vfio

# 翻转模式
sudo ./nvme_dev_switch.sh toggle
```

## 机制

| 步骤 | host ← 从 VFIO 切出来              | vfio ← 从 nvme 切回去              |
|------|-------------------------------------|-------------------------------------|
| 1    | `modprobe nvme`                     | `modprobe vfio-pci`                 |
| 2    | `unbind` from vfio-pci              | 检查 NVMe 块设备是否被占用/mount    |
| 3    | `bind` to nvme                      | `unbind` from nvme                  |
| 4    | 回退：`driver_override` + rescan    | `bind` to vfio-pci                  |
|      |                                     | 回退：remove + rescan + override    |

## 注意事项

1. **sudo 必须** — 操作 PCI sysfs 节点要求 root
2. **切换前**先确保 QEMU/KVM 已关闭（切换至 VFIO 同理，确保文件系统已 umount）
3. 状态非持久化 — 重启后恢复为 VFIO 默认
4. `driver_override` 每次切换后均清空，避免干扰后续切换
5. dmesg 会记录驱动切换日志；如果块设备未出现，检查 `dmesg | tail -20`
