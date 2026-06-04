# Runtime Environment Detection

日期: 2026-06-05
涉及文件: kinit.cpp / cpuid_intel.h

---

## 动机

SparrowOS 各功能（IPI 等待策略、TSC-deadline timer、UMONITOR/UMWAIT 等）的行为依赖于
底层执行环境的特性。同一段代码在 KVM、TCG、裸机下的语义差异可能很大，甚至完全不可用
（如 TCG 下 UMONITOR/UMWAIT 不存在）。

需要一个统一的全局机制在 early boot 探测环境，各功能模块按 `g_env` 分流。

---

## 三态枚举

```cpp
enum runtime_env : uint8_t {
    ENV_BARE_METAL = 0,   // 物理机
    ENV_KVM        = 1,   // QEMU + KVM 加速
    ENV_TCG        = 2,   // QEMU TCG 模拟（无加速）
};
```

存储在全局变量 `g_env` 中，在 `very_early_init()` 第一行初始化。

---

## 探测算法

**唯一判据：CPUID leaf 0x40000000（hypervisor 签名）。**

```
CPUID(0x40000000)
    ├─ EAX=0x40000001  EBX=0x4B4D564B  ECX=0x564B4D56  EDX=0x0000004D → KVM
    │   ("KVMKVMKVM\0\0\0")
    ├─ EAX=0x40000001  EBX=0x54474354  ECX=0x43544743  EDX=0x47435447 → TCG
    │   ("TCGTCGTCGTCG")
    └─ 其他任何组合 → BARE_METAL
```

**原则：**
- 不拿能力探查（WAITPKG、MONITOR 等）当环境判据
- WAITPKG、TSC-deadline 等走各自独立的 feature flag 探查路径
- 四个寄存器全量匹配，避免 leaf aliasing 误判

**实现：**

```c
static runtime_env probe_env(void)
{
    cpuid_tmp c(0x40000000, 0);

    /* KVM: "KVMKVMKVM\0\0\0" */
    if (c.eax == 0x40000001 &&
        c.ebx == 0x4B4D564B &&
        c.ecx == 0x564B4D56 &&
        c.edx == 0x0000004D)
        return ENV_KVM;

    /* TCG: "TCGTCGTCGTCG" */
    if (c.eax == 0x40000001 &&
        c.ebx == 0x54474354 &&
        c.ecx == 0x43544743 &&
        c.edx == 0x47435447)
        return ENV_TCG;

    return ENV_BARE_METAL;
}
```

---

## 各环境关键差异

| 特性 | BARE_METAL | KVM | TCG |
|------|-----------|-----|-----|
| UMONITOR/UMWAIT | ✅ 硬件原生 | ✅ 透传至物理 CPU | ❌ #UD |
| MONITOR/MWAIT | ✅ CPL=0 | ✅ 透传或 NOP（取决于 `-overcommit mwait=on`） | ❌ stub NOP |
| TSC-deadline timer | ✅ 硬件原生 | ✅ 透传 | ❌ 写入静默丢弃 |
| cmpxchg16b | ✅ 硬件 | ✅ 透传 | ✅ TCG 模拟 |
| 真实并发 | ✅ 物理核并行 | ✅ vCPU 并行（取决于 host 分配） | ❌ 单线程时分复用 |
| Interrupt 语义 | 真实 | 真实 | 模拟 |
| 多核同步 | 真实 cache 一致性 | 真实 cache 一致性 | 一致性模型不完全 |

---

## 影响范围

当前受 `g_env` 影响的功能模块：

| 模块 | BARE_METAL / KVM | TCG |
|------|-----------------|-----|
| IPI v3 sender 等待 | UMONITOR/UMWAIT 休眠 | `_mm_pause()` spin |
| TSC-deadline timer | 启用（硬件支持时） | 禁用（写 MSR 无效） |
| UMWAIT TSC deadline 兜底 | 有效 | 不执行此路径 |

**新增功能时**，如果行为在 TCG/KVM/裸机下有差异，必须考虑添加 `g_env` 分流。
