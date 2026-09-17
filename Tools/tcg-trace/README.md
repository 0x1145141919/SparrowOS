# Tools/tcg-trace —— TCG+trace 取证工具（薄壳速查）

> ⚠️ **权威文档在 `kernel/Docs/Debug/TCG_TRACE_ARSENAL.md`**（工具矩阵 / 全部参数 /
> 输出布局 / 离线流程 / 已知坑）。**本文件只是速查，不复述细节**；两者冲突以 ARSENAL 为准，
> 改工具后请**同一回合**更新 ARSENAL。

## 30 秒上手

```bash
cd /home/PS/PS_git/OS_pj_uefi/kernel

# 取证前：确认 QEMU 带 ioport80 魔法断点补丁（+#PF/#GP 钩子已开启）
QEMU_BIN=/path/to/patched/qemu-system-x86_64 Tools/tcg-trace/tcg-trace.sh --selfcheck

# 连跑到抓到异常样本，出官方 vmcore
Tools/tcg-trace/tcg-trace.sh --tag w --repeat 40 --timeout 40 --dump-vmcore

# 挑出有价值的样本后再做高质处理：
Tools/tcg-trace/vmcore-mkcore.py w01.vmcore w01.serial --kernel kernel.elf \
    --out w01.core --report w01.map-report.txt
Tools/tcg-trace/trace-sym.py w01.trace --start kernel --only-domain kernel --src \
    --out w01.kernel.src.log
gdb kernel.elf w01.core
```

结论解析：末行 `grep '^TAG='`（**别用 `tail -1`**）；退出码 `10` = 抓到异常样本。
样本落 `$SPARROW_DEBUG_STORE/traces`（默认 `/mnt/huge_data/sparrowos_debug/traces`）。

## 工具清单（细节见 ARSENAL §3）

`tcg-trace.sh` · `qmp-dump-vmcore.py` · `vmcore-mkcore.py` · `trace-sym.py` ·
`qmp-wait-stop.py` · `qmp-status.py` · `patches/`（QEMU 魔法断点补丁 + 部署纪律）。

> 老式 `.ram`/pmemsave 路径（`qmp-memdump.py`、`ram-read.py`、`ram-mkcore.py`）已删除。
