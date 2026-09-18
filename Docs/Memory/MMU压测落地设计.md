# MMU（Kspace）压测落地设计 —— 三接口 + invalidate_tlb

> **定位**：**鸠占鹊巢**——`kthread_ymir.cpp` 的 `if_real_init==false` 分支**整段替换**为 MMU 压测逻辑
> （旧调度器/WRAITH 测试场景由 git 兜底，HEAD 即当前版本）。
> **核心是 `invalidate_tlb`**（陈旧 TLB = 直接通往三重错误的生死线）。
> **被测对象**：`memory/out_surfaces.cpp`（三接口 + `broadcast_invalidate_tlb`）·
> `memory/arch/x86_64/KspacMapMgr_pagediretct_operate.cpp`（set/clear + `remote_invalidate_seg`）·
> `memory/arch/x86_64/AddresSpace.cpp`（`invalidate_tlb_of_VM_desc`）。
> **配套**：`Docs/Memory/2026-06-06-TLB-shootdown-v3-IPI-重构.md` · `PCId_tlbshutdown_modle.txt` ·
> `TLB_shootdown_PCID_model_implementation.md` · `memory/FreePagesAllocator.h` · `main_phyaddr_access_window.h`；
> 测试基建复用 `调度器压测落地设计.md`（fw_cfg 选组 / 断言账本 / `sched-matrix.sh` / 停机取证）。

---

## 0. 一句话

三个被测接口：`Kspace_pinterval_alloc_and_map` / `Kspace_phyaddr_direct_map` / `Kspace_phyaddr_direct_unmap`。
- **物理页一律走 FPA 圈地**（`kernel_pinned`），绝不自选物理地址（否则可能打到内核本体），并以**测试水位线**兜底防 OOM。
- 每次映射做**双窗口验证**：主物理窗 `PHYACC_VA(phys)` ↔ 新鲜映射窗。
- **`invalidate_tlb` 的正确性**用「**同一 VA 改投到不同物理页，再读校验**」判定——把「陈旧 TLB」
  从「静默/三重错误」降级为一个**可观测的错误值**。
- **大页路径必须打到 1GB**（不能只测 4KB/2MB）。
- **允许"故意 #PF"用例**：unmap 后解引用 → 命中 #PF 入口的 `FAULT_FREEZE`（`#WF#` + 魔法断点）→ 正是为 dump 而生，不惧。
- 单核是功能核（F）；**跨核 shootdown 是核武器（S）**——SMP≥2 才有意义。

---

## 1. 被测三接口的调用语义（读源码后的合同）

| 接口 | 做什么 | 前置条件 | 返回 |
|---|---|---|---|
| `Kspace_pinterval_alloc_and_map(iv, kurd)` | **自动分配 VA** + 插 VM_DESC + `enable_VMentry`（映射） | `iv.vpn==0`（自动）、`iv.ppn!=0`、`iv.npages!=0` | vbase（0=失败） |
| `Kspace_phyaddr_direct_map(iv)` | 把**调用方指定**的 kernel VA 映射到 `iv.pbase()` | `iv.is_kernel_address()`、`npages!=0` | KURD |
| `Kspace_phyaddr_direct_unmap(iv)` | vm_table remove + `disable_VMentry`（清 PTE、填 pak）→ 出锁 `broadcast_invalidate_tlb(pak)` | kernel VA | KURD |

- 三接口内部：`interrupt_guard` + `kspace_pagetable_modify_lock`（保护「vm_table + 页表」原子性）。
- **物理页由调用方给**：`alloc_and_map` 不会替你分配物理页（这正是"自己 FPA 圈地"的原因）。
- **invalidate 实链**：`broadcast_invalidate_tlb(pak)` = 先 `remote_invalidate_seg(pak)`（本核）+
  对其它核逐个 `returnable_ipi_send(remote_invalidate_seg)`；**50ms deadline 超时即 panic**
  （`TLB_SHOOTDOWN_TIMEOUT`）。`nproc<=1` 时只走本核、不发 IPI。
- 遗留：`KspacePageTable::invalidate_seg()`（走 `shared_inval_kspace_VMentry_info`）**当前无调用者**，
  测试只覆盖 live 路径（`remote_invalidate_seg`）。

---

## 2. 安全包线（怎么不把内核自己打死）

1. **物理**：只用 `FreePagesAllocator::alloc(size, BUDDY_ALLOC_DEFAULT_FLAG, page_state_t::kernel_pinned, kurd)`
   圈地，`FreePagesAllocator::free(base,size)` 归还。**内核映像在启动时已作为 used 排除在 BCB 空闲链之外**
   ⇒ FPA 不会吐出内核本体页。（stack_alloc 同款用法。）
2. **虚拟**：只碰自己 alloc 的 VA。
   - `alloc_available_space()` 以 vm_table（含内核映像/堆/窗口等既有映射）为占位图找洞 ⇒ **不会吐内核映像 VA**。
   - `direct_map` 路径自选 VA：用 `alloc_available_space(size,0)` 取候选后**立即 insert**；用一把测试互斥
     串起「alloc→insert」堵 TOCTOU，避免与并发的 `alloc_and_map` 抢同一 VA。
   - **绝不 unmap/改内核映像区**；只 unmap 本测试插入的 desc 覆盖的 VA。
3. **前置自检（防三重错误）**：每次拿到候选 VA，先断言它**不落在任何既有 VM_DESC 内**
   （`kspace_vm_table->search(va)==null`）。若 vm_table 登记不全导致 alloc 吐到内核映像 VA，立刻停并报证。
4. **检测子默认不触发 #PF**：常规判据全部非故障——
   - 页表真值：`KspacePageTable::v_to_phyaddrtraslation(va,&pa)`；
   - 物理真值：`PHYACC_VA(phys)` 读（主窗口，pbase=0）；
   - TLB 真值：**仅在 VA 仍映射时**解引用。
   ⇒ 陈旧 TLB 表现为**读到错误的值**，而非 #PF。
5. **故意 #PF 用例（允许，终局）**：#PF 入口是 `FAULT_FREEZE`（编译期宏，默认开）→ 串口 `#WF#` →
   `outb(0x80,0xDB)` 魔法断点（QEMU `vm_stop`）→ `cli;hlt`。**这正是 panic_dump 资产，故意踩不惧**。
   但它是**终局**（整机冻结），因此放独立模式、作为该 boot 的最后一步（见 §5 I-PF / §7）。

---

## 3. 双窗口验证法（数据正确性）

对每个映射窗口，三方交叉写读，模式含 seed（phys/va/iter）：

```
1) 经 PHYACC_VA(phys) 写 PAT(seed)  →  经 mapped VA 读  必 == PAT(seed)   // 映射指对物理
2) 经 mapped VA 写 PAT2(seed)       →  经 PHYACC_VA(phys) 读 必 == PAT2     // 写穿透到物理
```

---

## 4. invalidate_tlb 判定核心（三条）

| 代号 | 判据 | 打什么 |
|---|---|---|
| **T-presence** | unmap 后 `v_to_phyaddrtraslation(va)` 必 NOT_PRESENT，且 `kspace_vm_table->search(va)` 命中空 | 页表真被撤 |
| **T-stale（单核）** | map VA→A、读（暖 TLB）→ unmap → map **同一 VA**→B（A≠B 物理）→ 读必 == **B**；读==A ⇒ 陈旧 TLB 未失效 | 本核 invlpg 效力 |
| **T-shootdown（跨核）** | 核 P：map VA→A；核 C：读 VA → P：unmap+remap VA→B → 核 C 再读必 == **B**；C 读==A ⇒ shootdown 漏核 | `broadcast_invalidate_tlb` |
| **T-pf（故意 #PF）** | unmap 后**解引用**该 VA → 必命中 #PF（`#WF#` 冻结）；若**不** #PF ⇒ 页表没撤（真异常） | 硬件层真撤映射 |

> T-stale/T-shootdown 全程 VA 映射到**有效物理页**（A 或 B），解引用永不 #PF，错误以"读到旧内容"显形。
> T-pf 反之：故意踩空，验证"撤得干净"。

---

## 5. 用例清单

### 功能核 F（确定性阶梯，单线程为主）
| 用例 | 内容 | 判据 |
|---|---|---|
| **I-1** | 三接口基本往返：FPA 圈 N 页→填 pattern→`alloc_and_map`→双窗口→`unmap`→T-presence→`free` | 全绿 |
| **I-2** | `direct_map`/`direct_unmap` 基本往返（自选 VA band） | 全绿 |
| **I-3** | **T-stale 单核**（同 VA 改投 A↔B） | 读到期望物理 |
| **I-4** | **页尺寸阶梯 4KB / 2MB / 1GB**（强制覆盖 `_4lv_pte_4KB_` / `_4lv_pde_2MB_` / **`_4lv_pdpte_1GB_`** 的 set/clear + 对应粒度 invlpg）；含非对齐区间的自适应降级（同余拆分） | 各档 set/clear 正确、双窗口正确、大页**确实走 1GB 分支**（探针计数校验） |
| **I-5** | 负例/边界：`npages=0`、非内核 VA、direct_map 到已被占 VA、重复 unmap、unmap 未映射 | 返回**特定 KURD fail，不 panic** |
| **I-6** | 泄漏对账：N 轮 map/unmap 后 FPA `alloc_count==free_count`、vm_table 无测试残留 desc | 账平 |
| **I-PF** | **故意 #PF（终局）**：map→unmap→解引用 → 期望 `#WF#` 冻结 | 命中 #WF#（expected fault）|

### 竞态核 S（核武器，需 SMP≥2）
| 用例 | 内容 | 判据 |
|---|---|---|
| **I-7** | **跨核 shootdown 压**（T-shootdown）：P/C 两核交替 map/remap/读 | mismatch==0（**核心**）|
| **I-8** | **同 VA 重映射风暴**：多核轮番 map/unmap 同一 VA 到交替物理；读者只能见合法 pattern | 无非法值 |
| **I-9** | **map/unmap churn**：N 线程各占 VA band 热循环 map→双窗口→unmap | 无 PTE 撕裂/丢映射 |
| **I-10** | **shootdown 超时抗压**：burner 占满其它核时反复 broadcast | 不命中 `TLB_SHOOTDOWN_TIMEOUT`(50ms) |
| **I-11** | 统计/归属对账：`kspace_pagetable_statistics[pid].invalidate_tlb_count` 合理增长；结束无泄漏 | 无泄漏 |

> `logical_processor_count<=1` 时 `broadcast_invalidate_tlb` 早退、无 IPI ⇒ I-7..I-10 必须在 SMP≥2（建议 6/8）下跑。

---

## 6. 1GB 大页路径（先当有，实验定论）

**口径：直接打 1GB，不预设预留；拿不拿得到，实跑见分晓。**
- I-4 直接用 `align_log2=30` 的 FPA 请求 + `phys%1GB` 同余 VA，尝试走 `_4lv_pdpte_1GB_entries_set`。
- **探针校验**（关键，防"以为测了"）：用 `kspace_pagetable_statistics[pid].pages_set.specific.x86_64.PDPTE_HUGE_set_count`
  增量断言"确实走了 1GB 分支"；若为 0 ⇒ 实际被降级成 2MB/4KB。
- **拿不到时（FPA 给不出 1GB 对齐连续块，或 VA 不同余）**：现场记账（探针标 `huge1g_fallback`）并
  `wraith_freeze("huge1g-unavailable")` 留证 → 再定是否需 boot 早期预留 1GB 对齐物理块。**不预先堵死。**

---

## 7. 组织形态（鸠占鹊巢：整分支替换）

**替换范围**：`kthread_ymir.cpp` `if_real_init==false` 分支内**所有调度器测试逻辑**——
角色枚举（`WROLE_*`）、`wraith_hotloop`、`wraith_worker_entry`、`wraith_root`、bq 测试载体、
slot/canary 调度测试语义——整段换成 MMU 场景。**旧场景由 git 兜底（HEAD `ebd2898` 一带，即 `debug/wraith: 测试分支扩展`）**。

**保留的地基**（通用，非调度专有）：
- `wraith_freeze()` 计划/异常截停闸门（串口 `#TB#` + `outb(0x80,0xDB)`）；
- `wraith_test_ring`（线程上下文日志环，`ring-dump.py` 可捞）；
- UART 标记 / `io_outb` 等裸原语；
- （可选）`g_wraith_slots` 若 MMU 多核握手用得上则留，否则一并换。

**新主体**：
- `mmu_test_main()` 取代 `kthread_test_main()`；入口读 **fw_cfg** 选模式（复用落地设计的选组基建）：
  `-fw_cfg name=opt/sparrow/test,string=mode=full|pf|fonly,tcg,s10,r07`
  - `full`（默认）：跑 F（断言账本 I-1..I-6）+ S（多核 churn I-7..I-11）→ `wraith_freeze("planned")`。
  - `pf`：跑到 I-PF 故意 #PF → 期望 `#WF#`（**终局**，单独 boot）。
  - `fonly`：只跑 F（快速功能轮）。
- **断言账本** `wraith_assert_ledger`（逐条 pass/fail，失败即 `wraith_freeze("assert-fail:I-x")`）承载 F 部分。
- **探针符号** `g_mmu_*`（iters / mismatch / presence_fail / pf_expected / fpa_alloc / fpa_free /
  huge1g_hits / scratch_va / shootdown_count…）供 `ring-dump.py` / `stackcheck.sh` 读。
- 停机取证沿用 `Tools/tcg-trace/tcg-trace.sh` + `Tools/wraith/sched-matrix.sh`。

**与调度器落地设计的关系（已定调）**：不再谈 git 恢复。`调度器待测清单.md` + `调度器压测落地设计.md`
两份文档已把调度器测试的**设计上下文**留住 ⇒ 将来调度器场景**按文档现场重建**，比在旧实现上叠加更划算
（现场鸠占鹊巢 > 屎山叠加）。**本分支归 MMU，不做新旧共存。**

---

## 8. 判据 / 三振出局

- **PASS**：断言账本全绿 + 探针 `mismatch==0` + FPA 对账平 + 无 panic/超时。
- **真异常**（任一）：mismatch / presence_fail / `TLB_SHOOTDOWN_TIMEOUT` / `#WF#`（**非 I-PF 期望的**）/
  PANIC / HANG ⇒ **全停 + `--dump-vmcore --mkcore` + `stackcheck.sh` + `ring-dump.py`**。
- **I-PF 特例**：`mode=pf` 下 `#WF#` 是**期望结局**，执行器须按"expected-fault"记账（用启动标记区分），仍 dump 留档。
- **负对照（敏感度）**：在**故意破坏 invalidate**（临时 patch 去掉 invlpg / 跳过 shootdown /
  不动 `remote_invalidate_seg`）上，**I-3 / I-7 / I-PF 必须变红**——否则"0 命中"无意义。

---

## 9. 水位线（防 OOM）：总 FPA 预算的 75%

- FPA 耗尽时 `alloc` 返回 `INVALID_ALLOC_BASE` + KURD fail（**不 panic**）。
- **水位线 = 现场算出总 FPA 预算 × 75%**（留 25% 给内核与其它子系统），测试自持 outstanding-bytes
  账本，到线即停分配（先 drain 再收）。
- **前置**：当前 `总 FPA 预算`（`g_all_avaliable_mem_accumulate` / BCB 总 span）是**文件级 static、未暴露**，
  `BCB_count/BCBS` 也在 private 段 ⇒ 需**加一个公开 getter**（如 `FreePagesAllocator::get_total_budget_bytes()`）。
- **任何 `alloc` 返回失败 ⇒ `wraith_freeze("fpa-watermark")`**（视作水位线设定失误，非产品 bug，立即留证）。
- 目的：既压出跨核 churn 的显形率，又绝不打到 OOM。

---

## 10. 与既有基建的边界

- `src/tests/`（USER_MODE，g++ 宿主）只覆盖 FPA/BCB/HCB/kpoolmemq 与 **VM_DESC 记账**
  （已有 `kspace_vm_table_test.cpp` 的独立复刻）；三接口的 **TLB/invlpg/IPI 在宿主 USER_MODE 走 mmap，无意义**
  ⇒ **I-3..I-11、I-PF 必须 guest**。
- 可选宿主件：`alloc_available_space()` VA 分配器单测（纯记账，可测）。

---

## 11. 成本分级

| 类 | 用例 | 条件集 |
|---|---|---|
| **F** | I-1..I-6（+ I-PF 单列） | `kvm+tcg × rep10`（无 stress） |
| **S** | I-7..I-11 | `kvm+tcg × {0,5,10,16,22} × rep20`（I-9/I-10 最吃压力） |

矩阵并入总账（替换原调度器分支的测试位）。

---

## 11.5 实测发现（F 冒烟 · 2026-09-18，TCG SMP6）

- **fonly / full 均跑绿**（串口到 `#TB#planned`）：I-1/2/3/4a(4KB)/4b(2MB)/4c(1GB)/5/6 全过。
- **1GB 路径打通**：FPA 能给出 1GB 对齐连续块 → 确实走 `_4lv_pdpte_1GB_entries_set`
  （`PDPTE_HUGE_set_count` 探针命中），双窗口 + unmap 通过。无 `|1G-NO|`/`|1G-SPLIT|`。
- **✅ 交互问题已定位并修复（专项 1，2026-09-18）**：
  - 现象：1GB 用例后紧接的 4KB map/unmap，**首个 unmap 报 FATAL**（event_code=2 DISABLE_VMENTRY，
    reason=0 INVALIDE_PAGES_SIZE）。
  - **根因**：`KspacMapMgr.cpp` `disable_VMentry` 里 `case_4KB_SIZE:` **漏空格** ⇒ 编译器当作
    **goto 标签**（非 case 标签）⇒ 当区间同余级被判为 1GB/2MB（`vm_interval_to_pages_info` 按
    `start` 而非区间形心判同余，1GB/2MB 对齐的小区间会 fallthrough 而 `congruence_level` 不下修）
    且内含 4KB 子项时，switch 落 `default:` → FATAL。4KB-congruent 区间走 `congruence_level_4kb` 分支
    （不 switch）故无恙——解释了“为何只在特定 VA/PA 对齐下翻车”。
  - **修复**：`case_4KB_SIZE:` → `case _4KB_SIZE:`；顺带修 `_4lv_pdpte_1GB_entries_clear` 的
    invlpg 地址 double-count（`vaddr_base + ((pdpt_index+i)<<30)` → `vaddr_base + (i<<30)`；
    该处与 broadcast 冗余，故实际 TLB 刷新无碍，仅潜在隐患）。
  - **验证**：`mode=repro`（1GB→4KB）首 unmap 返 SUCCESS；`mode=full` 恢复原失败序（1GB 在 4KB 前）
    仍跑到 `#TB#planned`。
- 附带确认（repro B）：**未观察到 1GB 陈旧 TLB**（`|RBsame|`=1）——真 TLB 刷新由
  `broadcast_invalidate_tlb` 完成，`_4lv_pdpte_1GB_entries_clear` 内的 invlpg 只是冗余。
- **旁证（调用方隐患）**：`KspacePageTable::v_to_phyaddrtraslation` 对**已清 PTE** 仍返回 SUCCESS
  （叶 present 位未校验）→ **不能用其成功与否判 present**，必须比对译出物理。测试已据此改判据。
  （`__wrapped_pgs_vfree` 曾依赖它取 pbase 再 free——对未映射 VA 有隐患，另记。）

## 11.6 KVM 档（2026-09-18）

- **可跑**。QEMU 侧需 `-accel kvm -cpu host,+invtsc`（已入 `tcg-trace.sh` 的 kvm 默认）：
  - 内核 TSC 门（`tsc.cpp::tsc_regist`）要求 CPUID.80000007H EDX[8]（invariant TSC）——`-cpu max` 是 TCG 专用、
    `host` 默认不带 ⇒ 需显式 **`+invtsc`**（否则 `PANIC: TSC registration failed`）；
    CPUID.1 ECX[24]（TSC-deadline）由 host 满足。
  - KVM 不支持 `-d in_asm`（TCG 专有）⇒ 脚本对 kvm 只留 `-D`，不带 `-d`。
- **结果**：kvm `fonly`/`full` 均绿（环打捞：`total=30 pass=30 fail=0`，`huge_hits=1` 真走 1GB，
  `TB reason=planned`）；kvm `pf` 命中 `#WF#`（期望）。
- **停机判据改为 QMP STOP**：运行时串口（`bsp_kout`）是 UDP 式、会被多核日志交错撕裂
  （实测 `#TB#planned` 被撕成 `#eTB#pnladnn:ed`）⇒ 不可靠。故：
  - 测试打点全部只写 **`wraith_test_ring`**（关中断临界区 + 拿锁写内存），结束落 core 再 `ring-dump` 打捞；
  - 整机停机的**权威判据** = `outb(0x80,0xDB)` 魔法断点 → QEMU STOP（QMP 事件）；
    `tcg-trace.sh` 在 kvm（或转储）时挂 QMP，以 STOP 事件定 `RESULT=FAULT/MAGICBP`。

## 12. 已定 / 待办

**已定**：① 1GB 先当有、实跑定论；② 水位线 = 总 FPA 预算 × 75%；③ 分支归 MMU，调度器将来按文档重建。

**待办（实现前置）**：
1. **总 FPA 预算 getter**（`g_all_avaliable_mem_accumulate` 现为文件 static，需暴露）——水位线用的前提。
2. **I-PF 归档口径**：`mode=pf` 的 `#WF#` 由执行器判为 expected-fault（不触发"全停"）、仍 dump 留档。
3. **负对照**：故意破坏 invalidate 的临时 patch，确保 I-3/I-7/I-PF 能变红。
