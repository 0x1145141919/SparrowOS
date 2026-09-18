#!/usr/bin/env bash
# =============================================================================
# kvm-run.sh — SparrowOS KVM(硬件加速) 单次抓取器
#
# 为什么单开一个（不并进 tcg-trace.sh）：
#   · KVM **不能 trace**（-d in_asm 是 TCG 专有）⇒ 不做 -D/-d，只靠【内存转储】；
#   · KVM 需要固定 CPU 型号  `-accel kvm -cpu host,+invtsc`：
#       内核 tsc.cpp::tsc_regist() 要求 CPUID.80000007H EDX[8]（invariant TSC），
#       `-cpu max` 是 TCG 专用；host 默认不带 ⇒ 必须显式 +invtsc，
#       否则 `PANIC: TSC registration failed`。（TSC-deadline CPUID.1 ECX[24] 由 host 满足。）
#   · KVM 下运行时串口（bsp_kout）是 UDP 式、会被多核日志交错撕裂 ⇒ 不作停机判据；
#       停机以 outb(0x80,0xDB) 魔法断点 → QMP STOP 事件为权威
#       （见 Docs/Debug/kvm_gdb_debug_notes.md 方案6：端口强制断点）。
#   · 测试自身的进度/诊断写在 wraith_test_ring（debug_tmp_ring_buff），
#       结束落 core 后用 ring-dump.py 打捞（--ring 便捷）。
#
# 用法:
#   ./kvm-run.sh [选项]
#     --tag NAME      样本名（默认 kvmtest）
#     --timeout SEC   墙钟上限（默认 60，超时判 HANG）
#     --smp N         核数（默认 6）
#     --fwcfg STR     追加 -fw_cfg（可重复）；如 'name=opt/sparrow/test,string=mode=full'
#     --cpu SPEC      CPU 型号（默认 'host,+invtsc'，勿轻易改）
#     --outdir DIR    输出目录（默认 $SPARROW_DEBUG_STORE/traces，回退 <BASE>/VMresources/traces）
#     --base DIR      仓库布局根（默认 /home/PS/PS_git/OS_pj_uefi）
#     --stop-on RE    串口命中即停正则（默认 '#TB#|#WF#|PANIC'）
#     --repeat N      连跑 N 次，命中任一样本即停（默认 1）
#     --skip-noise-re RE  命中该正则判【噪声】：不转储、不算异常、连跑继续
#     --dump-vmcore   异常停止时抓官方 vmcore（QMP dump-guest-memory）→ <tag>.vmcore
#     --mkcore        由 vmcore 合成虚拟视角 GDB core → <tag>.core（隐含 --dump-vmcore）
#     --ring          异常停止时（隐含 --mkcore）再 ring-dump 打捞 wraith_test_ring 并打印
#     --selfcheck     只校验 QEMU_BIN 是否带 ioport80 魔法断点补丁（不启动 VM）
#     -h|--help
#   环境: QEMU_BIN(默认 /home/PS/PS_git/qemu/build/qemu-system-x86_64，须带 ioport80 补丁)
#         SPARROW_DEBUG_STORE(默认 /mnt/huge_data/sparrowos_debug)
#         RING_SYM(默认 wraith_test_ring)
#
# 停止判据（任一）: QMP STOP 事件（魔法断点，权威）/ 串口命中 --stop-on / 超时。
# 输出: <outdir>/<tag>.serial ; 异常时 <tag>.vmcore / <tag>.core
#       末行: TAG= RESULT= REASON= ELAPSED= SMP= SERIAL= VMCORE= CORE=
#       退出码: 0=正常(KSHELL/OTHER) 10=抓到异常样本
# =============================================================================
set -uo pipefail

BASE="${BASE:-/home/PS/PS_git/OS_pj_uefi}"
QEMU_BIN="${QEMU_BIN:-/home/PS/PS_git/qemu/build/qemu-system-x86_64}"
SMP="${SMP:-6}"
CPU="${CPU:-host,+invtsc}"

OUTDIR=""; TAG="kvmtest"; TIMEOUT=60; STOP_ON='#TB#|#WF#|PANIC'; REPEAT=1
DUMP_VMCORE=0; DO_MKCORE=0; DO_RING=0; DO_SELFCHECK=0; SKIP_NOISE_RE=""
FWCFG_ARG=()
ACCEL="kvm"
RING_SYM="${RING_SYM:-wraith_test_ring}"

usage() { awk 'NR==1{next} /^set /{exit} {print}' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --outdir) OUTDIR="${2:?}"; shift;;
    --tag) TAG="${2:?}"; shift;;
    --timeout) TIMEOUT="${2:?}"; shift;;
    --smp) SMP="${2:?}"; shift;;
    --cpu) CPU="${2:?}"; shift;;
    --fwcfg) FWCFG_ARG+=(-fw_cfg "${2:?}"); shift;;
    --base) BASE="${2:?}"; shift;;
    --stop-on) STOP_ON="${2:?}"; shift;;
    --repeat) REPEAT="${2:?}"; shift;;
    --skip-noise-re) SKIP_NOISE_RE="${2:?}"; shift;;
    --dump-vmcore) DUMP_VMCORE=1;;
    --mkcore) DO_MKCORE=1; DUMP_VMCORE=1;;
    --ring) DO_RING=1; DO_MKCORE=1; DUMP_VMCORE=1;;
    --selfcheck) DO_SELFCHECK=1;;
    -h|--help) usage 0;;
    *) echo "未知参数: $1" >&2; usage 1;;
  esac
  shift
done

VM="$BASE/VMresources"
MT="$BASE/host_tools/mtools/usr/bin"
TCGDIR="$BASE/kernel/Tools/tcg-trace"      # 复用 QMP/转储/打捞 python 助手
ESP="$VM/efi_partion.img"
OFF=$((2048 * 512))
BOOTLOADER="$BASE/edk2/PlayOSLoaderPkg/BUILD/DEBUG_GCC5/X64/PlayOSLoaderPkg/PlayOSLoaderPkg/DEBUG/PlayOSLoaderPkg.efi"

# ── 魔法断点自检（KVM）───────────────────────────────────────
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
  local probe sock st="NOCONN" s2
  probe="$(mktemp /tmp/qemu-magicbp-probe.XXXXXX.img)"
  sock="$(mktemp -u /tmp/qemu-magicbp.XXXXXX.qmp)"
  make_probe "$probe"
  "$QEMU_BIN" -accel kvm -cpu "$CPU" \
    -drive "file=$probe,format=raw,index=0,media=disk" \
    -display none -monitor none -no-reboot -m 128 \
    -qmp "unix:$sock,server=on,wait=off" >/dev/null 2>&1 &
  local p=$!
  for i in $(seq 1 30); do
    sleep 0.3
    s2="$(python3 "$TCGDIR/qmp-status.py" "$sock" 2>/dev/null || true)"
    [ -n "$s2" ] && st="$s2"
    case "$st" in debug|paused) break;; esac
  done
  kill -TERM "$p" 2>/dev/null; wait "$p" 2>/dev/null
  rm -f "$probe" "$sock"
  case "$st" in
    debug|paused) echo "SELFCHECK=PASS  QEMU_BIN=$QEMU_BIN  accel=kvm  (魔法断点活着)"; return 0;;
    *) echo "SELFCHECK=FAIL  QEMU_BIN=$QEMU_BIN  status=${st:-NOCONN}  → 无 ioport80 补丁或 KVM 不可用" >&2; return 1;;
  esac
}
if [ "$DO_SELFCHECK" = 1 ]; then selfcheck; exit $?; fi

if [ -z "$OUTDIR" ]; then
  STORE="${SPARROW_DEBUG_STORE:-/mnt/huge_data/sparrowos_debug}"
  if [ -d "$(dirname "$STORE")" ]; then OUTDIR="$STORE/traces"; else OUTDIR="$VM/traces"; fi
fi
mkdir -p "$OUTDIR"
for f in "$MT/mcopy" "$VM/OVMF.fd" "$BASE/kernel/init.elf" "$BASE/kernel/initramfs.img" \
         "$BOOTLOADER" "$ESP" "$VM/arch-root.qcow2"; do
  [ -e "$f" ] || { echo "缺少: $f" >&2; exit 1; }
done

stage_esp() {
  "$MT/mmd"   -i "$ESP@@$OFF" ::/EFI ::/EFI/BOOT 2>/dev/null || true
  "$MT/mcopy" -i "$ESP@@$OFF" -o "$BASE/kernel/initramfs.img" ::/initramfs.img        >/dev/null
  "$MT/mcopy" -i "$ESP@@$OFF" -o "$BASE/kernel/init.elf"       ::/init.elf             >/dev/null
  "$MT/mcopy" -i "$ESP@@$OFF" -o "$BOOTLOADER"                 ::/EFI/BOOT/BOOTX64.efi >/dev/null
}

run_one() {
  local tag="$1" ser="$OUTDIR/$1.serial"
  local qsock="$OUTDIR/$1.qmp"
  rm -f "$qsock"; local qmp_arg=(-qmp "unix:$qsock,server=on,wait=off")
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
    -accel "$ACCEL" -cpu "$CPU" -serial stdio -display none -monitor none \
    "${FWCFG_ARG[@]}" \
    "${qmp_arg[@]}" \
    >"$ser" 2>&1 </dev/null &
  local pid=$! s=$SECONDS reason="" flag="" wpid=""
  flag="$(mktemp -u /tmp/qemu-stop.XXXXXX.flag)"
  python3 "$TCGDIR/qmp-wait-stop.py" "$qsock" "$flag" $(( TIMEOUT + 20 )) >/dev/null 2>&1 &
  wpid=$!
  while kill -0 "$pid" 2>/dev/null; do
    if [ -n "$flag" ] && [ -e "$flag" ]; then reason=MAGICBP; break; fi
    grep -qE "$STOP_ON" "$ser" 2>/dev/null && { reason=MATCH; break; }
    [ $((SECONDS - s)) -ge "$TIMEOUT" ] && { reason=TIMEOUT; break; }
    sleep 0.2
  done
  [ -z "$reason" ] && reason=EXIT
  local el=$((SECONDS - s))
  sleep 0.5

  # 分类
  local res=OTHER
  if   [ "$reason" = MAGICBP ]; then res=FAULT
  elif grep -q 'kshell>' "$ser" 2>/dev/null; then res=KSHELL
  elif grep -q 'PANIC'   "$ser" 2>/dev/null; then res=PANIC
  elif [ "$reason" = TIMEOUT ]; then res=HANG
  fi
  if [ -n "$SKIP_NOISE_RE" ] && grep -qE "$SKIP_NOISE_RE" "$ser" 2>/dev/null; then res=NOISE; fi

  local want_dump=0
  if [ "$res" != KSHELL ] && [ "$res" != OTHER ] && [ "$res" != NOISE ]; then want_dump=1; fi

  # QMP 单客户端：常驻等待器先退场
  if [ -n "$wpid" ]; then kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null; wpid=""; sleep 0.3; fi

  local vmcore="-" core="-"
  if [ "$DUMP_VMCORE" -gt 0 ] && [ "$want_dump" = 1 ]; then
    if python3 "$TCGDIR/qmp-dump-vmcore.py" "$qsock" "$OUTDIR/$1.vmcore" \
               --timeout "${VMCORE_TIMEOUT:-900}" 2>&1; then
      vmcore="$(du -h "$OUTDIR/$1.vmcore" 2>/dev/null | cut -f1)"
      if [ "$DO_MKCORE" -gt 0 ]; then
        if python3 "$TCGDIR/vmcore-mkcore.py" "$OUTDIR/$1.vmcore" "$ser" \
                   --kernel "$BASE/kernel/kernel.elf" --out "$OUTDIR/$1.core" \
                   --report "$OUTDIR/$1.map-report.txt" 2>&1 | sed 's/^/  /'; then
          core="$(du -h "$OUTDIR/$1.core" 2>/dev/null | cut -f1)"
          if [ "$DO_RING" -gt 0 ] && [ -e "$OUTDIR/$1.core" ]; then
            echo "  [ring] --- $RING_SYM ---"
            python3 "$TCGDIR/ring-dump.py" "$OUTDIR/$1.core" --kernel "$BASE/kernel/kernel.elf" \
                    --symbol "$RING_SYM" --max-bytes "${RING_MAX_BYTES:-8192}" 2>&1 | sed 's/^/  [ring] /'
          fi
        else
          core="MKFAIL"
        fi
      fi
    else
      vmcore="DUMPFAIL"; rm -f "$OUTDIR/$1.vmcore"
    fi
  fi

  kill -TERM "$pid" 2>/dev/null; sleep 0.3; kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  [ -n "$wpid" ] && kill "$wpid" 2>/dev/null
  rm -f "$qsock" "$flag"

  printf 'TAG=%s RESULT=%s REASON=%s ELAPSED=%ss SMP=%s SERIAL=%s VMCORE=%s CORE=%s\n' \
    "$tag" "$res" "$reason" "$el" "$SMP" "$ser" "$vmcore" "$core"
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
