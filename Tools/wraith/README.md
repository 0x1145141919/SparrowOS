# Tools/wraith —— WRAITH 验收工具（截停后栈检查）

> 定位：**WRAITH 修复（F1–F5）牢固性验收**。配套战报与方案见
> `Docs/Debug/WRAITH/`；通用 TCG 取证武器库见 `Docs/Debug/TCG_TRACE_ARSENAL.md`。
> 本目录只放「测试分支 + 截停后栈检查」的专用工具。

## 组成

| 件 | 角色 |
|---|---|
| `wraith-stackcheck.py` | GDB 内嵌 Python：冻结快照（`.core`/gdbstub）上的双调度金丝雀 + 测试线程栈/金丝雀校验 |
| `stackcheck.sh` | 薄壳：跑 `mkcore`（若给 `.vmcore`）+ 起 gdb + 执行检查 |

## 端到端流程

```bash
cd /home/PS/PS_git/OS_pj_uefi/kernel

# 1) 带测试分支构建（默认关；见 CMakeLists 的 KTHREAD_TEST_SCENARIO）
cmake -DKTHREAD_TEST_SCENARIO=ON . && make kernel.elf initramfs

# 2) 跑到「计划截停」并抓 core（stop 钉在 #TB#，别停在 kshell>）
Tools/tcg-trace/tcg-trace.sh --tag wraithtest --timeout 60 --stop-on '#TB#' \
    --dump-vmcore --mkcore
#   → RESULT=FAULT REASON=MAGICBP（#TB# 独立于 --stop-on 也会被识别）

# 3) 截停后栈检查
Tools/wraith/stackcheck.sh /mnt/huge_data/sparrowos_debug/traces/wraithtest.core kernel.elf
#   → 末行 [sc] VERDICT=PASS|FAIL

# 4) 还原默认构建（release 不含测试分支）
cmake -DKTHREAD_TEST_SCENARIO=OFF . && make kernel.elf initramfs
```

## 检查项

- **INV-A 双调度金丝雀**：任一 task 不得同时登记在两个核的 `g_cpu_running[]` 上
  （= WRAITH 定义式；同 task 双核即违规）。
- **测试线程逐个**（内核以 `-DKTHREAD_TEST_SCENARIO` 构建时）：状态 / 内核栈区间 /
  保存现场(`priv_ctx.iret`) / 栈金丝雀 自洽。
  - `handoff window` / `pre-register window` 为 F4 的**合法单步瞬态**，报为 `[note]`，不计 FAIL。

## 测试分支（在 guest 内）

`kthread_ymir.cpp`（`-DKTHREAD_TEST_SCENARIO` 段）派生一棵**测试线程树**（自相似：线程可自派生）：
- 角色：纯 CPU 竞争(yield) / sleep（被 kicker **跨核唤醒**，打 F4 唤醒窗口）/ 自派生 / 退出；
- **跑完不原子销毁**：`kthread_exit` 后不 `release` ⇒ 僵尸停车区保留其内核栈，供截停后取证；
- 每线程在**浅层帧**持一枚栈金丝雀（`MAGIC^tid`），热循环内每轮自校验，被异核浅写踩坏即 `wraith_freeze`；
- 计划截停：`wraith_freeze("planned")` → 串口 `#TB#` → `outb(0x80,0xDB)` → `cli;hlt`。

> 敏感度验证（关键）：在 **pre-fix**（`git revert -n 67e4a76`）上应能看到 INV-A 违规 /
> 金丝雀命中；post-fix 应 `VERDICT=PASS`。否则「0 命中」无意义。

## 已知坑

- `wraith-stackcheck.py` 必须 **ASCII-only**（gdb 的 Python stdout 常为 ASCII，中文会炸）。
- 工具读的 `.core` 必须**保留恒等窗口别名**（`vmcore-mkcore.py` 默认即保留）。
- `#TB#` 的整机冻结依赖打过 ioport80 补丁的 QEMU；无补丁时 guest 停在 `cli;hlt`（host 仍能 dump）。
