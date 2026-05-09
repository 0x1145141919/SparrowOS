#!/usr/bin/env bash
#
# build_bootloader.sh — 构建 PlayOS UEFI bootloader（EDK2 项目）
# 被 CMake 的 add_custom_command 调用。
# 只接受一个参数：EDK2 项目根目录的绝对路径。
#
# 该脚本隐藏 EDK2 的全部内部复杂度（edksetup、工具链等）。
# CMake 只需 "运行这个脚本" 即可。
#
set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 <edk2_root_dir>"
    exit 1
fi

EDK2_DIR="$1"
cd "$EDK2_DIR"

# EDK2 的 edksetup.sh 检查是否被 source（它用 $0 / $@ 判断）。
# 我们自己的 $@ 包含了 EDK2_DIR，会混淆 edksetup.sh 的参数解析。
# 必须在源 edksetup.sh 之前清空 positional parameters。
set --

# 加载 EDK2 编译环境
. edksetup.sh

# 构建 PlayOS bootloader
build
