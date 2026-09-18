#!/usr/bin/env bash
# =============================================================================
# tcg-trace.sh — SparrowOS TCG(+trace) 单次抓取器：「AI 无状态调试范式」核心工具
#
# 方法论/权威详见 Docs/Debug/TCG_TRACE_ARSENAL.md（同目录 README.md 为薄壳速查）。一句话：用 TCG 复现竞态 panic/死锁，把
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
#     --stop-on RE  串口命中即停的正则（默认 'PANIC|kshell>|#WF#'）
#     --repeat N     连跑 N 次，抓到任一异常样本即停（默认 1）
#     --base DIR     仓库布局根（默认 /home/PS/PS_git/OS_pj_uefi）
#     --dump-always  连正常样本也转储（默认只对 PANIC/HANG/SIZECAP 转储）
#     --dump-fault-only  仅对 guest 首爆冻结（RESULT=FAULT）转储（避开 AP 类噪声 panic）
#     --skip-noise-re RE 串口命中该正则的样本判【噪声】(RESULT=NOISE)：不转储、不算异常、
#                        连跑继续（用于 12× 等高压下顶出的 AP 启动 IPI 超时等假阳性）
#     --dump-vmcore  异常停止时抓【官方 vmcore】（QMP dump-guest-memory, 非 paging,
#                    物理, 只 RAM, 含全 CPU 寄存器）→ <tag>.vmcore（建议首选）
#     --mkcore       在 vmcore 基础上合成【虚拟视角 GDB core】→ <tag>.core
#                    （隐含 --dump-vmcore；单根 kspace_up_half + 附 .map-report.txt）
#     --selfcheck    只校验 QEMU_BIN 是否带 ioport80 魔法断点补丁（不启动 VM）；
#                    通过 ⇒ guest outb(0x80,0xDB) 能让整机暂停、可 dump。
#     -h|--help
#   环境: SMP(默认 6) BASE(同 --base)
#         QEMU_BIN(默认 PATH 里的 qemu-system-x86_64)。⚠️ 跑取证请显式指向打过
#                 patches/qemu-ioport80-magic-bp.patch 的构建产物，并用 --selfcheck 自检。
#
#   停止判据（任一）: 串口命中 --stop-on / 串口 #WF#（fault 冻结）或 #TB#（测试截停）/
#                     QMP 观测到 STOP 事件（guest 冻结）/ 超时 / trace 超帽。
#                     guest 冻结的样本 RESULT=FAULT、REASON=MAGICBP（#TB# 见 Tools/wraith/）。
#   噪声：命中 --skip-noise-re 的样本 RESULT=NOISE（不转储、连跑继续）。
#
# 输出: <outdir>/<tag>.trace  (QEMU -D 日志)
#       <outdir>/<tag>.serial (串口)
#       <outdir>/<tag>.vmcore (官方 vmcore；仅 --dump-vmcore 且命中异常时)
#       <outdir>/<tag>.core   (虚拟视角 core + .map-report.txt；仅 --mkcore)
#       末行: TAG= RESULT= REASON= ELAPSED= TRACE= LINES= SERIAL= RAM= VMCORE= CORE=
#       退出码: 0=正常(KSHELL/OTHER) 10=抓到异常样本(PANIC/HANG/SIZECAP)
# =============================================================================
set -uo pipefail

BASE="${BASE:-/home/PS/PS_git/OS_pj_uefi}"
SMP="${SMP:-6}"
D_CATS='in_asm,int,guest_errors,unimp,cpu_reset,pcall'
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"

OUTDIR=""; TAG="trace"; TIMEOUT=90; CAP_GB=3; STOP_ON='PANIC|kshell>|#WF#'; REPEAT=1
DUMP_ALWAYS=0; DUMP_VMCORE=0; DO_MKCORE=0; DO_SELFCHECK=0; DUMP_ONLY_FAULT=0; SKIP_NOISE_RE=""

usage() { awk 'NR==1{next} /^set /{exit} {print}' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --outdir)  OUTDIR="${2:?}";  shift;;
    --tag)     TAG="${2:?}";     shift;;
    --timeout) TIMEOUT="${2:?}"; shift;;
    --cap-gb)  CAP_GB="${2:?}";  shift;;
    --stop-on) STOP_ON="${2:?}"; shift;;
    --repeat)  REPEAT="${2:?}";  shift;;
    --base)    BASE="${2:?}";    shift;;
    --dump-always) DUMP_ALWAYS=1;;
    --skip-noise-re) SKIP_NOISE_RE="${2:?}"; shift;;
    --dump-fault-only) DUMP_ONLY_FAULT=1;;
    --dump-vmcore) DUMP_VMCORE=1;;
    --mkcore)      DO_MKCORE=1; DUMP_VMCORE=1;;
    --selfcheck)   DO_SELFCHECK=1;;
    -h|--help) usage 0;;
    *) echo "未知参数: $1" >&2; usage 1;;
  esac
  shift
done

VM="$BASE/VMresources"
MT="$BASE/host_tools/mtools/usr/bin"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # 供同目录 qmp-*.py 定位
ESP="$VM/efi_partion.img"
OFF=$((2048 * 512))                       # GPT 分区1起始扇区 2048 → 字节偏移
BOOTLOADER="$BASE/edk2/PlayOSLoaderPkg/BUILD/DEBUG_GCC5/X64/PlayOSLoaderPkg/PlayOSLoaderPkg/DEBUG/PlayOSLoaderPkg.efi"

# ── 魔法断点自检 ──────────────────────────────────────────────
# 512B 探针：outb(0x80,0xDB) 后停机。带补丁的 QEMU 会 vm_stop(DEBUG)。
make_probe() {
  python3 - "$1" <<'PY'
import sys
b = bytearray(512)
b[0:10] = bytes([0xBA,0x80,0x00,0xB0,0xDB,0xEE,0xFA,0xF4,0xEB,0xFC])
b[510], b[511] = 0x55, 0xAA
open(sys.argv[1], "wb").write(b)
PY
}
selfcheck() {
  local probe sock st="NOCONN" s2 i
  probe="$(mktemp /tmp/qemu-magicbp-probe.XXXXXX.img)"
  sock="$(mktemp -u /tmp/qemu-magicbp.XXXXXX.qmp)"
  make_probe "$probe"
  "$QEMU_BIN" -drive "file=$probe,format=raw,index=0,media=disk" \
    -display none -monitor none -no-reboot -m 128 \
    -qmp "unix:$sock,server=on,wait=off" >/dev/null 2>&1 &
  local p=$!
  for i in $(seq 1 30); do
    sleep 0.3
    s2="$(python3 "$SELF_DIR/qmp-status.py" "$sock" 2>/dev/null || true)"
    [ -n "$s2" ] && st="$s2"
    case "$st" in debug|paused) break;; esac
  done
  kill -TERM "$p" 2>/dev/null; wait "$p" 2>/dev/null
  rm -f "$probe" "$sock"
  case "$st" in
    debug|paused) echo "SELFCHECK=PASS  QEMU_BIN=$QEMU_BIN  (魔法断点活着)"; return 0;;
    *)            echo "SELFCHECK=FAIL  QEMU_BIN=$QEMU_BIN  status=${st:-NOCONN}  → 该 QEMU 无 ioport80 魔法断点补丁（见 patches/README.md）" >&2; return 1;;
  esac
}
if [ "$DO_SELFCHECK" = 1 ]; then selfcheck; exit $?; fi

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
  if [ "$DUMP_VMCORE" -gt 0 ]; then rm -f "$qsock"; qmp_arg=(-qmp "unix:$qsock,server=on,wait=off"); fi
  stage_esp
  "$QEMU_BIN" \
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
  local pid=$! s=$SECONDS reason="" sz flag="" wpid=""
  # QMP「STOP 事件」常驻等待器：guest 冻结（魔法断点）时零轮询开销地通知本脚本
  if [ "${#qmp_arg[@]}" -gt 0 ]; then
    flag="$(mktemp -u /tmp/qemu-stop.XXXXXX.flag)"
    python3 "$SELF_DIR/qmp-wait-stop.py" "$qsock" "$flag" $(( TIMEOUT + 20 )) >/dev/null 2>&1 &
    wpid=$!
  fi
  while kill -0 "$pid" 2>/dev/null; do
    if grep -qE '#WF#|#TB#' "$ser" 2>/dev/null; then reason=MAGICBP; break; fi   # guest 冻结记号（#WF#=fault / #TB#=测试截停；独立于 --stop-on）
    grep -qE "$STOP_ON" "$ser" 2>/dev/null && { reason=MATCH; break; }
    if [ -n "$flag" ] && [ -e "$flag" ]; then reason=MAGICBP; break; fi
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
  if   [ "$reason" = MAGICBP ]; then res=FAULT
  elif grep -q 'kshell>' "$ser" 2>/dev/null; then res=KSHELL
  elif grep -q 'PANIC'   "$ser" 2>/dev/null; then res=PANIC
  elif [ "$reason" = TIMEOUT ]; then res=HANG
  elif [ "$reason" = SIZECAP ]; then res=SIZECAP
  fi

  # 噪声过滤：命中 --skip-noise-re 的样本判 NOISE（不转储、不算异常、连跑继续）。
  # 用途：高负载会把"最便宜的时序失败"（如 AP 启动 IPI 超时）顶出来，非目标竞态；
  # 否则狩猎会在第一枚假阳性上就停。默认关闭（SKIP_NOISE_RE 为空）。
  if [ -n "$SKIP_NOISE_RE" ] && grep -qE "$SKIP_NOISE_RE" "$ser" 2>/dev/null; then res=NOISE; fi

  # 转储门控：默认对所有异常结局转储；--dump-fault-only 时只对 guest 冻结(FAULT)转储
  local want_dump=0
  if   [ "$DUMP_ALWAYS" -gt 0 ]; then want_dump=1
  elif [ "$DUMP_ONLY_FAULT" -gt 0 ]; then { [ "$res" = FAULT ] && want_dump=1; }
  elif [ "$res" != KSHELL ] && [ "$res" != OTHER ] && [ "$res" != NOISE ]; then want_dump=1
  fi

  # ⚠️ QEMU QMP 是【单客户端】：常驻的 qmp-wait-stop 必须先退场，否则下方
  #    qmp-dump-vmcore.py 连上却收不到 greeting（KSHELL/HANG/SIZECAP 样本不触发
  #    STOP ⇒ 它不会自行退出，会把唯一的 QMP 槽一直占着）。
  if [ -n "$wpid" ]; then kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null; wpid=""; sleep 0.3; fi

  # ── 官方 vmcore：dump-guest-memory（自带 stop/resume；只 RAM + 全 CPU 寄存器）──
  local vmcore="-" core="-"
  if [ "$DUMP_VMCORE" -gt 0 ]; then
    if [ "$want_dump" = 1 ]; then
      if python3 "$SELF_DIR/qmp-dump-vmcore.py" "$qsock" "$OUTDIR/$1.vmcore" \
                 --timeout "${VMCORE_TIMEOUT:-900}" 2>&1; then
        vmcore="$(du -h "$OUTDIR/$1.vmcore" 2>/dev/null | cut -f1)"
        if [ "$DO_MKCORE" -gt 0 ]; then
          if python3 "$SELF_DIR/vmcore-mkcore.py" "$OUTDIR/$1.vmcore" "$ser" \
                     --kernel "$BASE/kernel/kernel.elf" --out "$OUTDIR/$1.core" \
                     --report "$OUTDIR/$1.map-report.txt" 2>&1 | sed 's/^/  /'; then
            core="$(du -h "$OUTDIR/$1.core" 2>/dev/null | cut -f1)"
          else
            core="MKFAIL"
          fi
        fi
      else
        vmcore="DUMPFAIL"; rm -f "$OUTDIR/$1.vmcore"
      fi
    fi
  fi

  kill -TERM "$pid" 2>/dev/null; sleep 0.3; kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  [ -n "$wpid" ] && kill "$wpid" 2>/dev/null
  rm -f "$qsock" "$flag"

  printf 'TAG=%s RESULT=%s REASON=%s ELAPSED=%ss TRACE=%s LINES=%s SERIAL=%s VMCORE=%s CORE=%s\n' \
    "$tag" "$res" "$reason" "$el" \
    "$(du -h "$tr" 2>/dev/null | cut -f1)" \
    "$(wc -l <"$tr" 2>/dev/null || echo 0)" "$ser" "$vmcore" "$core"
  case "$res" in KSHELL|OTHER|NOISE) return 0;; *) return 10;; esac
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
