#!/usr/bin/env bash
# =============================================================================
# stackcheck.sh —— WRAITH 截停后栈检查（薄壳）
#
# 用法:
#   Tools/wraith/stackcheck.sh <core|vmcore> [kernel.elf]
#     <core>   虚拟视角 core（vmcore-mkcore.py 产物）；给 .vmcore 会自动先 mkcore
#     [kernel] ELF（默认 ./kernel.elf，须与冻结镜像同构建、带 DWARF、-DKTHREAD_TEST_SCENARIO）
#
# 例:
#   Tools/wraith/stackcheck.sh /mnt/huge_data/sparrowos_debug/traces/wraithtest.core
#   Tools/wraith/stackcheck.sh /mnt/huge_data/sparrowos_debug/traces/wraithtest.vmcore kernel.elf
#
# 实录：末行 [sc] VERDICT=PASS|FAIL。
# =============================================================================
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE="${1:?用法: stackcheck.sh <core|vmcore> [kernel.elf]}"
KERNEL="${2:-kernel.elf}"

if [[ "$CORE" == *.vmcore ]]; then
  tmp="${CORE%.vmcore}.core"
  echo "[stackcheck] mkcore: $CORE -> $tmp"
  python3 "$here/../tcg-trace/vmcore-mkcore.py" "$CORE" "${CORE%.vmcore}.serial" \
      --kernel "$KERNEL" --out "$tmp" >/dev/null || { echo "[stackcheck] mkcore 失败"; exit 2; }
  CORE="$tmp"
fi

exec gdb -q -batch \
  -ex "source $here/wraith-stackcheck.py" \
  -ex "wraith-stackcheck" \
  "$KERNEL" "$CORE"
