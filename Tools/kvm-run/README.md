# Tools/kvm-run —— KVM(硬件加速) 抓取器

> 定位：与 `Tools/tcg-trace/` 平级的 **KVM 专用**单次抓取器。**为什么不并进 tcg-trace**：
> KVM 不能 `-d in_asm` trace；且需固定 `-cpu host,+invtsc`（内核 TSC 门）。

## 与 tcg-trace 的分工

| | tcg-trace.sh | kvm-run.sh |
|---|---|---|
| 加速器 | TCG | **KVM** |
| CPU | `max,+x2apic` | **`host,+invtsc`**（硬编码，勿随意改） |
| trace | **有**（`-D -d in_asm,...`，~25MB/次） | **无**（KVM 不支持） |
| 停机判据 | 串口 `#TB#/#WF#` / `kshell>` / 超时 / trace 超帽 | **QMP STOP**（魔法断点，权威）/ 串口 / 超时 |
| 取证 | vmcore / mkcore | vmcore / mkcore / **`--ring` 打捞** |
| QMP | 仅转储时挂 | **常挂**（停机判据需要） |

## 为什么 `host,+invtsc`

内核 `src/arch/x86_64/core_hardwares/x86_arch/tsc.cpp::tsc_regist()` 在 KVM/bare-metal 下要求
CPUID.80000007H EDX[8]（invariant TSC）与 CPUID.1 ECX[24]（TSC-deadline）**同时**成立，否则
`PANIC: TSC registration failed: system cannot continue`。QEMU 的 `host` 模型默认**不暴露** invtsc，
必须显式 `+invtsc`（TSC-deadline 由 host 满足）。

## 停机与打捞

- 停机：guest `outb(0x80,0xDB)` → QEMU `vm_stop(DEBUG)` → QMP 发 STOP → 本脚本据此判 `RESULT=FAULT/MAGICBP`
  （见 `Docs/Debug/kvm_gdb_debug_notes.md` 方案6）。运行时串口（`bsp_kout`）是 UDP 式、多核会交错撕裂，
  **不作权威判据**。
- 打捞：测试进度/诊断写在 `wraith_test_ring`（debug_tmp_ring_buff）。`--ring` = 转储 + mkcore + `ring-dump`。

## 用法

```bash
cd kernel
Tools/kvm-run/kvm-run.sh --tag t --fwcfg 'name=opt/sparrow/test,string=mode=full' --ring
Tools/kvm-run/kvm-run.sh --selfcheck          # 校验 QEMU_BIN 的 ioport80 补丁
```

依赖同目录/邻目录的 python 助手（实际在 `Tools/tcg-trace/`：`qmp-status.py` / `qmp-wait-stop.py` /
`qmp-dump-vmcore.py` / `vmcore-mkcore.py` / `ring-dump.py`）。`QEMU_BIN` 默认
`/home/PS/PS_git/qemu/build/qemu-system-x86_64`（须带 `patches/qemu-ioport80-magic-bp.patch`）。
