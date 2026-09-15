#!/usr/bin/env bash
# =============================================================================
# tcg-trace.sh — SparrowOS TCG(+trace) 单次抓取器：「AI 无状态调试范式」核心工具
#
# 方法论详见同目录 README.md。一句话：用 TCG 复现竞态 panic/死锁，把
# 「CPU trace（in_asm 反汇编 + 异常/中断 + 寄存器 dump）」一次性落盘；
# 命中 kshell>/PANIC、或超时、或 trace 体积超帽即停 —— 供事后逐条反汇编 +
# 符号化定位（kernel/kernel.elf）。
#
# 为什么是这一套：
#   * TCG（-cpu max）软件模拟，时序与 KVM 不同 —— 是复现竞态/死锁的档位；
#   * -d in_asm,... 「翻译即记、只记唯一 TB」→ 体积可控（正常 ~27MB/次）；
#   * -d cpu（每 TB 寄存器）约 330MB/s，不可用，明确禁用；
#   * 对 -D 文件设体积帽：故障风暴（异常自旋）会写到十几 GB，必须止损。
#
# 用法:
#   ./tcg-trace.sh [选项]
#     --outdir DIR   输出目录（默认 /mnt/huge_data/sparrowos_debug/traces，
#                     不存在时回退 <BASE>/VMresources/traces）
#     --tag NAME     本次样本名（默认 trace）
#     --timeout SEC  单次墙钟上限（默认 90，超时判 HANG 并停）
#     --cap-gb N     trace 体积帽（默认 3 GB，超帽判 SIZECAP 并停）
#     --stop-on RE  串口命中即停的正则（默认 'PANIC|kshell>'）
#     --repeat N     连跑 N 次，抓到 PANIC/HANG/SIZECAP 即停（默认 1）
#     --base DIR     仓库布局根（默认 /home/PS/PS_git/OS_pj_uefi）
#     --dump-mem MB  异常停止时额外抓「最终物理内存镜像」MB 兆字节 → <tag>.ram
#     --mem-base A   内存转储起始物理地址（默认 0；支持 0x… 十六进制）
#     --dump-always  连正常样本也转储（默认只对 PANIC/HANG/SIZECAP 转储）
#     -h|--help
#   环境: SMP(默认 6) BASE(同 --base)
#
# 输出: <outdir>/<tag>.trace  (QEMU -D 日志)
#       <outdir>/<tag>.serial (串口)
#       <outdir>/<tag>.ram    (物理内存镜像；仅 --dump-mem 且命中异常时)
#       末行: TAG= RESULT= REASON= ELAPSED= TRACE= LINES= SERIAL= RAM=
#       退出码: 0=正常(KSHELL/OTHER) 10=抓到异常样本(PANIC/HANG/SIZECAP)
# =============================================================================
set -uo pipefail

BASE="${BASE:-/home/PS/PS_git/OS_pj_uefi}"
SMP="${SMP:-6}"
D_CATS='in_asm,int,guest_errors,unimp,cpu_reset,pcall'

OUTDIR=""; TAG="trace"; TIMEOUT=90; CAP_GB=3; STOP_ON='PANIC|kshell>'; REPEAT=1
DUMP_MEM_MB=0; MEM_BASE=0; DUMP_ALWAYS=0

usage() { sed -n '2,40p' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --outdir)  OUTDIR="${2:?}";  shift;;
    --tag)     TAG="${2:?}";     shift;;
    --timeout) TIMEOUT="${2:?}"; shift;;
    --cap-gb)  CAP_GB="${2:?}";  shift;;
    --stop-on) STOP_ON="${2:?}"; shift;;
    --repeat)  REPEAT="${2:?}";  shift;;
    --base)    BASE="${2:?}";    shift;;
    --dump-mem) DUMP_MEM_MB="${2:?}"; shift;;
    --mem-base) MEM_BASE="${2:?}";    shift;;
    --dump-always) DUMP_ALWAYS=1;;
    -h|--help) usage 0;;
    *) echo "未知参数: $1" >&2; usage 1;;
  esac
  shift
done

VM="$BASE/VMresources"
MT="$BASE/host_tools/mtools/usr/bin"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # 供 qmp-memdump.py 定位
ESP="$VM/efi_partion.img"
OFF=$((2048 * 512))                       # GPT 分区1起始扇区 2048 → 字节偏移
BOOTLOADER="$BASE/edk2/PlayOSLoaderPkg/BUILD/DEBUG_GCC5/X64/PlayOSLoaderPkg/PlayOSLoaderPkg/DEBUG/PlayOSLoaderPkg.efi"

if [ -z "$OUTDIR" ]; then
  STORE="${SPARROW_DEBUG_STORE:-/mnt/huge_data/sparrowos_debug}"
  if [ -d "$(dirname "$STORE")" ]; then OUTDIR="$STORE/traces"
  else OUTDIR="$VM/traces"; fi
fi
mkdir -p "$OUTDIR"
for f in "$MT/mcopy" "$VM/OVMF.fd" "$BASE/kernel/init.elf" "$BASE/kernel/initramfs.img" \
         "$BOOTLOADER" "$ESP" "$VM/arch-root.qcow2"; do
  [ -e "$f" ] || { echo "缺少: $f" >&2; exit 1; }
done

stage_esp() {
  "$MT/mmd"   -i "$ESP@@$OFF" ::/EFI ::/EFI/BOOT 2>/dev/null || true
  "$MT/mcopy" -i "$ESP@@$OFF" -o "$BASE/kernel/initramfs.img" ::/initramfs.img          >/dev/null
  "$MT/mcopy" -i "$ESP@@$OFF" -o "$BASE/kernel/init.elf"       ::/init.elf               >/dev/null
  "$MT/mcopy" -i "$ESP@@$OFF" -o "$BOOTLOADER"                 ::/EFI/BOOT/BOOTX64.efi   >/dev/null
}

run_one() {
  local tag="$1" tr="$OUTDIR/$1.trace" ser="$OUTDIR/$1.serial"
  local cap=$(( CAP_GB * 1024 * 1024 * 1024 ))
  local qsock="$OUTDIR/$1.qmp"
  # 仅转储时才挂 QMP（避免非转储轮被多一个 chardev 扰动 TCG 交织）
  local qmp_arg=()
  if [ "$DUMP_MEM_MB" -gt 0 ]; then rm -f "$qsock"; qmp_arg=(-qmp "unix:$qsock,server=on,wait=off"); fi
  stage_esp
  qemu-system-x86_64 \
    -no-reboot -bios "$VM/OVMF.fd" -smp "$SMP" \
    -drive file="$ESP",format=raw,if=none,id=efi_disk \
    -device nvme,serial=beefdead,drive=efi_disk \
    -machine q35,kernel-irqchip=split \
    -device intel-iommu,intremap=on,caching-mode=on,aw-bits=39,eim=on,device-iotlb=on \
    -device virtio-net-pci,bus=pcie.0,netdev=net0 \
    -device intel-hda,bus=pcie.0 \
    -drive file="$VM/arch-root.qcow2",format=qcow2,if=none,id=nvme_disk \
    -device nvme,serial=deadbeef,drive=nvme_disk \
    -netdev user,id=net0 -m 8192 \
    -cpu "max,+x2apic" -serial stdio -display none -monitor none \
    "${qmp_arg[@]}" \
    -D "$tr" -d "$D_CATS" >"$ser" 2>&1 </dev/null &
  local pid=$! s=$SECONDS reason="" sz
  while kill -0 "$pid" 2>/dev/null; do
    grep -qE "$STOP_ON" "$ser" 2>/dev/null && { reason=MATCH; break; }
    [ $((SECONDS - s)) -ge "$TIMEOUT" ] && { reason=TIMEOUT; break; }
    sz=$(stat -c %s "$tr" 2>/dev/null || echo 0)
    [ "$sz" -ge "$cap" ] && { reason=SIZECAP; break; }
    sleep 0.2
  done
  [ -z "$reason" ] && reason=EXIT
  local el=$((SECONDS - s))
  sleep 1                                   # 命中 PANIC 时给 dump 落盘时间

  # 先分类（在杀 QEMU 之前），再决定是否需要内存转储
  local res=OTHER
  if   grep -q 'kshell>' "$ser" 2>/dev/null; then res=KSHELL
  elif grep -q 'PANIC'   "$ser" 2>/dev/null; then res=PANIC
  elif [ "$reason" = TIMEOUT ]; then res=HANG
  elif [ "$reason" = SIZECAP ]; then res=SIZECAP
  fi

  # ── 最终内存镜像：stop + pmemsave + quit（冻结快照，留下崩溃后的完整 RAM）──
  local ram="-"
  if [ "$DUMP_MEM_MB" -gt 0 ]; then
    if [ "$DUMP_ALWAYS" -gt 0 ] || { [ "$res" != KSHELL ] && [ "$res" != OTHER ]; }; then
      if python3 "$SELF_DIR/qmp-memdump.py" "$qsock" "$OUTDIR/$1.ram" "$MEM_BASE" \
                 $(( DUMP_MEM_MB * 1024 * 1024 )) 2>&1; then
        ram="$(du -h "$OUTDIR/$1.ram" 2>/dev/null | cut -f1)"
      else
        ram="DUMPFAIL"
      fi
    fi
  fi

  kill -TERM "$pid" 2>/dev/null; sleep 0.3; kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  rm -f "$qsock"

  printf 'TAG=%s RESULT=%s REASON=%s ELAPSED=%ss TRACE=%s LINES=%s SERIAL=%s RAM=%s\n' \
    "$tag" "$res" "$reason" "$el" \
    "$(du -h "$tr" 2>/dev/null | cut -f1)" \
    "$(wc -l <"$tr" 2>/dev/null || echo 0)" "$ser" "$ram"
  case "$res" in KSHELL|OTHER) return 0;; *) return 10;; esac
}

rc=0
if [ "$REPEAT" = 1 ]; then
  run_one "$TAG" || { rc=10; echo ">>> 抓到样本: $TAG"; }
else
  for i in $(seq -w 1 "$REPEAT"); do
    if run_one "${TAG}${i}"; then continue; fi
    rc=10; echo ">>> 抓到样本: ${TAG}${i}"; break
  done
fi
exit "$rc"
