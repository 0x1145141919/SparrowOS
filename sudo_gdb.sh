#!/bin/bash
# 使用 sudo 启动 gdb，并保持当前用户环境
exec sudo -E /usr/bin/gdb "$@"