#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# nvme_dev_switch.sh — NVMe host<->VFIO passthrough mode switcher
#
# 切换 0000:01:00.0 (Sandisk WD SN5x0 NVMe) 的驱动绑定状态：
#   host → nvme 驱动，宿主机可用
#   vfio → vfio-pci 驱动，供 VM 直通
#
# 该设备在开机时被内核参数 vfio-pci.ids=15b7:5017 和
# /etc/modprobe.d/vfio.d 固定由 vfio-pci 接管。
# 本脚本通过 sysfs 热切换驱动绑定，不会改变启动配置。
# 重启后恢复为 VFIO 模式（默认）。
#
# 权限：需要 root，通过 sudo 执行。
#
# Usage:
#   sudo ./nvme_dev_switch.sh status     查看当前状态
#   sudo ./nvme_dev_switch.sh host       切换为宿主机模式 (nvme)
#   sudo ./nvme_dev_switch.sh vfio       切换为直通模式   (vfio-pci)
#   sudo ./nvme_dev_switch.sh toggle     切换至相反模式
#

set -uo pipefail

# ── 配置 ──────────────────────────────────────────────────────────
BDF="0000:01:00.0"
DEV_PATH="/sys/bus/pci/devices/${BDF}"
DRIVER_VFIO="vfio-pci"
DRIVER_NVME="nvme"

ME="$(basename "$0")"

# ── 输出辅助 ──────────────────────────────────────────────────────
# 所有输出统一走 stdout；错误同时走 stderr 以确保 visible
msg()   { echo "  $*"; }
info()  { echo "  [INFO] $*"; }
warn()  { echo "  [WARN] $*"; }
die()   { echo "  [FAIL] $*"; echo "  [FAIL] $*" >&2; exit 1; }
header(){ echo ""; echo "== $* =="; }

# ── 辅助: 通过 BDF 找 NVMe 块设备名 ─────────────────────────────
#   返回 nvme1n1 / nvme0n1 / ""（未找到）
find_nvme_blk() {
    local bdf="$1"
    for blk in /sys/block/nvme*n1; do
        [ -e "$blk" ] || continue
        [ -f "$blk/device/address" ] || continue
        local addr
        addr=$(cat "$blk/device/address" 2>/dev/null || true)
        if [ "$addr" = "$bdf" ]; then
            basename "$blk"
            return 0
        fi
    done
    return 1
}

# ── 前置检查 ──────────────────────────────────────────────────────
check_prereqs() {
    [ -d "$DEV_PATH" ] || die "设备 $BDF 不存在于 /sys/bus/pci/devices/"

    local cls
    cls=$(cat "$DEV_PATH/class" 2>/dev/null || echo "unknown")
    [ "$cls" = "0x010802" ] || warn "设备 class=$cls (预期 0x010802=NVM Express)，确认设备正确"

    if ls /dev/vfio/ 2>/dev/null | grep -q .; then
        local vfio_grp
        vfio_grp=$(readlink "$DEV_PATH/iommu_group" 2>/dev/null || echo "")
        if [ -n "$vfio_grp" ] && [ -e "/dev/vfio/${vfio_grp##*/}" ]; then
            local holder
            holder=$(fuser "/dev/vfio/${vfio_grp##*/}" 2>/dev/null || true)
            [ -n "$holder" ] && warn "设备 IOMMU group ${vfio_grp##*/} 正被进程 $holder 使用 (QEMU?)"
        fi
    fi

    [ "$(id -u)" -eq 0 ] || die "需要 root 权限，请通过 sudo 执行"
}

# ── 获取当前驱动 ──────────────────────────────────────────────────
get_current_driver() {
    local drv_link="$DEV_PATH/driver"
    if [ -L "$drv_link" ]; then
        basename "$(readlink "$drv_link")"
    else
        echo "(none)"
    fi
}

# ── 查询状态 ──────────────────────────────────────────────────────
show_status() {
    local drv
    drv=$(get_current_driver)
    header "NVMe 设备 $BDF 状态"
    echo "  设备          : $(cat "$DEV_PATH/vendor" 2>/dev/null || echo "?"):$(cat "$DEV_PATH/device" 2>/dev/null || echo "?")"
    lspci_nfo=$(lspci -D -s "$BDF" 2>/dev/null | sed 's/^[^ ]* //' || echo "N/A")
    echo "  设备名称      : $lspci_nfo"
    echo "  IOMMU group   : $(readlink "$DEV_PATH/iommu_group" 2>/dev/null | xargs basename || echo "N/A")"
    echo "  当前驱动      : $drv"
    echo "  driver_override: $(cat "$DEV_PATH/driver_override" 2>/dev/null || echo "(null)")"
    echo ""

    # host 模式 → 显示目标 NVMe 块设备
    if [ "$drv" = "$DRIVER_NVME" ]; then
        local nvme_blk
        nvme_blk=$(find_nvme_blk "$BDF")
        if [ -n "$nvme_blk" ]; then
            local nvme_info
            nvme_info=$(lsblk -d -o NAME,SIZE,MODEL "/dev/${nvme_blk}" 2>/dev/null || true)
            if [ -n "$nvme_info" ]; then
                info "块设备 (host 模式):"
                echo "$nvme_info" | tail -n +2 | while IFS= read -r line; do echo "    $line"; done
            fi
        else
            warn "nvme 驱动已绑定，但未发现 $BDF 对应的块设备 — 可能初始化失败"
        fi
    fi

    if [ "$drv" = "$DRIVER_VFIO" ] || [ "$drv" = "(none)" ]; then
        info "启动默认: VFIO (内核参数 vfio-pci.ids=15b7:5017 + modprobe.d/vfio.d)"
    fi

    if [ "$drv" = "$DRIVER_VFIO" ]; then
        local grp
        grp=$(readlink "$DEV_PATH/iommu_group" 2>/dev/null | xargs basename 2>/dev/null || echo "?")
        if [ -e "/dev/vfio/$grp" ]; then
            local hold
            hold=$(fuser "/dev/vfio/$grp" 2>/dev/null || true)
            [ -n "$hold" ] && info "VFIO group /dev/vfio/$grp 正被进程 $hold 使用"
        fi
    fi
}

# ── 切换: VFIO -> host (nvme) ────────────────────────────────────
switch_to_host() {
    local drv
    drv=$(get_current_driver)
    [ "$drv" = "$DRIVER_NVME" ] && { msg "已经是 host 模式 (nvme 驱动)"; return 0; }

    header "切换 $BDF -> host 模式 (nvme)"

    lsmod | grep -q "^${DRIVER_NVME} " || {
        msg "装载 nvme 模块..."
        modprobe "$DRIVER_NVME" || die "装载 nvme 模块失败"
    }

    if [ "$drv" != "(none)" ]; then
        msg "解绑当前驱动 ($drv)..."
        if ! echo "$BDF" > "/sys/bus/pci/drivers/$drv/unbind" 2>/dev/null; then
            warn "解绑 $drv 失败 — 可能是设备正被使用"
            warn "请确保没有 VM (QEMU) 正在运行且持有该 VFIO 设备"
            die "解绑失败，操作终止"
        fi
        sleep 0.5
    fi

    msg "绑定 nvme 驱动..."
    if echo "$BDF" > "/sys/bus/pci/drivers/${DRIVER_NVME}/bind" 2>/dev/null; then
        sleep 1
        local new_drv
        new_drv=$(get_current_driver)
        [ "$new_drv" = "$DRIVER_NVME" ] || die "绑定后驱动为 $new_drv，预期 nvme"
        msg "成功: 设备 $BDF 现已由 nvme 驱动接管"

        local nvme_blk
        nvme_blk=$(find_nvme_blk "$BDF")
        if [ -n "$nvme_blk" ]; then
            local nvme_info
            nvme_info=$(lsblk -d -o NAME,SIZE,MODEL "/dev/${nvme_blk}" 2>/dev/null || true)
            if [ -n "$nvme_info" ]; then
                msg "块设备信息:"
                echo "$nvme_info" | tail -n +2 | while IFS= read -r line; do echo "    $line"; done
            fi
        else
            warn "nvme 驱动绑定成功，但未发现 $BDF 对应的块设备 — 请检查 dmesg"
        fi
    else
        warn "直接绑定失败，尝试 driver_override + rescan 回退方案..."
        echo "" > "$DEV_PATH/driver_override" 2>/dev/null || true
        echo 1 > /sys/bus/pci/rescan 2>/dev/null
        sleep 1
        local new_drv
        new_drv=$(get_current_driver)
        [ "$new_drv" = "$DRIVER_NVME" ] || die "回退方案仍失败，驱动为 $new_drv"
        echo "" > "$DEV_PATH/driver_override" 2>/dev/null || true
        msg "成功 (回退方案): 设备 $BDF 现已由 nvme 驱动接管"
    fi
}

# ── 切换: host (nvme) -> VFIO ────────────────────────────────────
switch_to_vfio() {
    local drv
    drv=$(get_current_driver)
    [ "$drv" = "$DRIVER_VFIO" ] && { msg "已经是 VFIO 模式 (vfio-pci)"; return 0; }

    header "切换 $BDF -> VFIO 直通模式 (vfio-pci)"

    lsmod | grep -q "^${DRIVER_VFIO} " || {
        msg "装载 vfio-pci 模块..."
        modprobe "$DRIVER_VFIO" || die "装载 vfio-pci 模块失败"
    }

    # 只检查这个 BDF 对应的 NVMe 块设备
    local nvme_blk
    nvme_blk=$(find_nvme_blk "$BDF")
    if [ -n "$nvme_blk" ]; then
        local holders
        holders=$(lsof "/dev/${nvme_blk}" 2>/dev/null || true)
        [ -n "$holders" ] && die "NVMe 块设备 /dev/${nvme_blk} 正被使用，请先释放"
        local mounted
        mounted=$(mount 2>/dev/null | grep "/dev/${nvme_blk}" || true)
        [ -n "$mounted" ] && die "NVMe 分区正在挂载，请先 umount:\n$mounted"
        msg "目标设备 $nvme_blk ($BDF) 无挂载/占用，安全切换"
    else
        msg "未找到 $BDF 对应的块设备（VFIO 模式或未初始化），跳过使用检查"
    fi

    if [ "$drv" != "(none)" ] && [ "$drv" != "$DRIVER_VFIO" ]; then
        msg "解绑当前驱动 ($drv)..."
        if ! echo "$BDF" > "/sys/bus/pci/drivers/$drv/unbind" 2>/dev/null; then
            die "解绑 $drv 失败 — 可能是设备正被使用"
        fi
        sleep 0.5
    fi

    msg "绑定 vfio-pci 驱动..."
    if echo "$BDF" > "/sys/bus/pci/drivers/${DRIVER_VFIO}/bind" 2>/dev/null; then
        sleep 0.5
        local new_drv
        new_drv=$(get_current_driver)
        [ "$new_drv" = "$DRIVER_VFIO" ] || die "绑定后驱动为 $new_drv，预期 vfio-pci"
        echo "" > "$DEV_PATH/driver_override" 2>/dev/null || true
        msg "成功: 设备 $BDF 现已由 vfio-pci 驱动接管"
    else
        warn "直接绑定失败，尝试 driver_override 方案..."
        echo "$DRIVER_VFIO" > "$DEV_PATH/driver_override" 2>/dev/null
        sleep 0.5
        if echo "$BDF" > "/sys/bus/pci/drivers/${DRIVER_VFIO}/bind" 2>/dev/null; then
            sleep 0.5
            local new_drv
            new_drv=$(get_current_driver)
            [ "$new_drv" = "$DRIVER_VFIO" ] || die "回退方案仍失败，驱动为 $new_drv"
            echo "" > "$DEV_PATH/driver_override" 2>/dev/null || true
            msg "成功 (回退方案): 设备 $BDF 现已由 vfio-pci 驱动接管"
        else
            warn "尝试终极方案 (remove + rescan)..."
            echo "" > "$DEV_PATH/driver_override" 2>/dev/null || true
            echo 1 > "/sys/bus/pci/devices/${BDF}/remove" 2>/dev/null || \
                die "remove 失败，设备状态异常。建议重启恢复"
            echo "$DRIVER_VFIO" > "/sys/bus/pci/devices/${BDF}/driver_override" 2>/dev/null || true
            echo 1 > /sys/bus/pci/rescan 2>/dev/null
            sleep 1
            local new_drv
            new_drv=$(get_current_driver)
            [ "$new_drv" = "$DRIVER_VFIO" ] || \
                die "终极方案失败，当前驱动为 $new_drv。请重新运行或重启"
            echo "" > "$DEV_PATH/driver_override" 2>/dev/null || true
            msg "成功 (终极方案): 设备 $BDF 现已由 vfio-pci 驱动接管"
        fi
    fi
}

# ── 主流程 ────────────────────────────────────────────────────────
main() {
    local cmd="${1:-status}"

    echo "=== $ME: NVMe device $BDF switch ==="

    case "$cmd" in
        status)     ;;
        host|vfio|toggle) echo "  (可能需要 root 权限)" ;;
        *)
            echo "Usage: $ME {status|host|vfio|toggle}"
            echo "  status   查看当前绑定状态和设备信息"
            echo "  host     切换为宿主机模式 (绑定 nvme 驱动)"
            echo "  vfio     切换为直通模式   (绑定 vfio-pci)"
            echo "  toggle   切换至相反模式"
            echo ""
            echo "  当前绑定: $(get_current_driver 2>/dev/null || echo '?')"
            exit 1
            ;;
    esac

    check_prereqs

    case "$cmd" in
        status) show_status ;;
        host)   switch_to_host ;;
        vfio)   switch_to_vfio ;;
        toggle)
            local drv
            drv=$(get_current_driver)
            if [ "$drv" = "$DRIVER_NVME" ]; then
                switch_to_vfio
            elif [ "$drv" = "$DRIVER_VFIO" ]; then
                switch_to_host
            elif [ "$drv" = "(none)" ]; then
                msg "设备无驱动绑定，默认切换至 host (nvme) 模式"
                switch_to_host
            else
                die "未知驱动状态: $drv，请手动指定 host 或 vfio 模式"
            fi
            ;;
    esac
}

main "$@"
