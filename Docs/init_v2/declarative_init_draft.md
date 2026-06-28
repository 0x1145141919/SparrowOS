# 声明式 init.elf 草案（2026-06-28）

> **起源：** Phase 3b 中 conjunc_GSs 和 hdstacks 的 `pages_set` 遗漏
> （commit bdcfafe 修复）。根本原因不是逻辑复杂，而是重复模板代码
> 导致人眼疲劳——类似的 `probe_keep + map + fill` 模式散布在 ~50 行
> 代码中，参数各有细微差异，肉眼扫过容易漏行。
>
> **困境：** 提议的声明式框架能根除这类遗漏，但 init.elf 设计冻结度
> 已高（~80%），为两处遗漏引入一个框架又显得过度工程。本文档记录
> 这个矛盾，作为未来是否跨过这条线的决策参考。

---

## 1. 当前痛点：模板代码的噪声密度

Phase 3b 的典型工作单元：

```cpp
// conjunc_GSs — 10 行
{
    uint64_t total_bytes = header->logical_processor_count * GS_COMPLEX_STRIDE;
    uint64_t npg = total_bytes >> 12;
    phyaddr_t pbase = page_allocator::available_meminterval_probe_keep(npg, 12);
    // 此处可能漏 pages_set ← 真发生了
    vaddr_t vbase = va_alloc_up(total_bytes, 12);
    ksetmem_8((void*)(uint64_t)pbase, 0, total_bytes);
    iv.arch_info.conjunc_GSs = { ... };
    kmmu->map({vbase, pbase, total_bytes}, KSPACE_RW_ACCESS);
}
```

这是极简的 C++——如果引入抽象，噪音可以压到 ~2 行。但每个工作单元都做了一次这样的展开，噪信比（boilerplate / actual decision）约为 7:1。

**错误由噪讯引入**，而非由决策引入。`pages_set` 是决策（"这些页要持久化"），但在展开中被淹没在映射/对齐/清零/VA 分配的机械代码中。

---

## 2. 声明式提案：resource_table

### 核心思想

将 Phase 3b 的工作从"指令序列"改为"资源配置表"。init.elf 遍历表，对每条资源自动执行：

```
probe_keep → pages_set(kernel_persisit) → map → 返回 vm_interval
```

### 表定义

```cpp
// 每条 = 一个要持久分配并映射的物理区间
struct init_resource_entry {
    const char*     name;           // 调试/日志用
    uint64_t        npages;         // 页数
    uint8_t         align_log2;     // 对齐（通常 12）
    uint64_t        access;         // KSPACE_RW_ACCESS / KSPACE_RX_ACCESS
    bool            need_clear;     // 是否需要 memset 清零
    bool            need_zero_va;   // 提前知道 VA？
    vaddr_t         fixed_va;       // 固定 VA（need_zero_va=true 时有效）
    vm_interval*    out;            // 结果写入位置
};
```

### Phase 3b 变为

```cpp
static const init_resource_entry R3B_TABLE[] = {
    // 物理资源
    { "kernel.elf",        kelf_pages, 12, KSPACE_RW_ACCESS, false, false, 0, &kl... },
    // 0x100 段：来自 ELF 遍历，动态追加
    // ...
    // 固定资源
    { "conjunc_GSs",       proc * stride >> 12, 12, KSPACE_RW_ACCESS, true,  false, 0, &iv.conjunc_GSs },
    { "hdstacks",          total >> 12,          12, KSPACE_RW_ACCESS, false, false, 0, &iv.hdstacks_* },
    { "IOAPIC MMIO",       ioapic_pages,         12, KSPACE_RW_ACCESS, false, true,  IOAPIC_VA, &ioapic },
    // ...
};

void phase_3b_alloc_all(kernel_mmu* kmmu, const init_resource_entry table[], int count) {
    for (int i = 0; i < count; i++) {
        auto* e = &table[i];
        phyaddr_t pa = page_allocator::available_meminterval_probe_keep(e->npages, e->align_log2);
        page_allocator::pages_set({pa, e->npages << 12}, kernel_persisit);  // ← 框架保证
        vaddr_t va = e->fixed_va ? e->fixed_va : va_alloc_up(e->npages << 12, e->align_log2);
        kmmu->map({pa, va, e->npages << 12}, e->access);
        if (e->need_clear) ksetmem_8((void*)(uint64_t)pa, 0, e->npages << 12);
        *e->out = vm_interval(va >> 12, pa >> 12, e->npages, e->access);
        bsp_kout << "[R3B] " << e->name << ": pa=" << pa << " va=" << va << kendl;
    }
}
```

### 能防住的 bug 种类

| bug | 指令序列 | 声明式表 |
|-----|---------|---------|
| `pages_set` 漏了 | ❌ 发生了 | ✅ 框架保证 |
| `probe_keep` 页数算错 | ❌ | ✅ 命中有无 |
| 对齐参数不一致 | ❌ | ✅ 集中控制 |
| 忘了映射某段 | ❌ | ✅ 表即映射 |
| 映射后忘了初始化 GDT 指针 | ❌ 另一类错误 | ⚠️ 不行，这是 Phase 4.5 的事 |

---

## 3. 为什么跨不过去

### 阻碍 A：动态表 vs 静态表

不是所有 Phase 3b 的资源都在编译期已知。kernel.elf 的 0x100 段是 ELF 解析时动态发现的，需要在循环中追加到表里——破坏了"纯声明式"的优雅。

### 阻碍 B：Phase 3b 只占 init.elf 的 ~15%

```
Phase 1 (堆 + 控制台)       → 不需要声明式
Phase 2 (basic_allocator)   → 不需要
Phase 2.5 (initramfs 搬迁)  → 不需要
Phase 3a (解析 ELF)         → 需要定制逻辑，不适合表驱动
Phase 3b (分配+映射)        → 声明式的最佳候选人 (~15%)
Phase 4 (信息包)            → 不需要
Phase 4.5 (自裁+CR3+GDT)   → 需要程序化逻辑
```

一个框架只为 15% 的代码服务。剩下的 85% 仍然是命令式 C++，对整体可维护性的提升有限。

### 阻碍 C：冻结度已高  → 新 bug 的预期收益低

上次加字段是 v2.1 协议（自省精简、独立段加载、hdstacks 独立），之后 init_init.cpp 的变更频率已经显著下降。`pages_set` 漏了是**存量代码的一次性疏漏**，不是**增量开发中的重复模式**。框架预防不了未来的独有 bug。

---

## 4. 折中方案：最小 helper，不引入框架

并非全有或全无。一个 15 行的 helper 函数就能消除 `pages_set` 漏掉的路径：

```cpp
// 最小分配抽象：probe_keep + pages_set + map 打包
vm_interval alloc_and_map(
    kernel_mmu* kmmu, uint64_t npages, uint32_t align_log2,
    vaddr_t vbase, uint64_t access, bool need_clear)
{
    phyaddr_t pbase = page_allocator::available_meminterval_probe_keep(npages, align_log2);
    page_allocator::pages_set({pbase, npages << 12}, page_state_t::kernel_persisit);
    kmmu->map({pbase, vbase, npages << 12}, access);
    if (need_clear) ksetmem_8((void*)(uint64_t)pbase, 0, npages << 12);
    return { .vpn = vbase >> 12, .ppn = pbase >> 12, .npages = npages, .access = access };
}
```

Phase 3b 从：

```cpp
phyaddr_t pbase = page_allocator::available_meminterval_probe_keep(npg, 12);
// pages_set 容易忘
vaddr_t vbase = va_alloc_up(total_bytes, 12);
ksetmem_8((void*)(uint64_t)pbase, 0, total_bytes);
iv.arch_info.conjunc_GSs = { .vpn = vbase >> 12, .ppn = pbase >> 12, .npages = npg, .access =  KSPACE_RW_ACCESS };
kmmu->map({vbase, pbase, total_bytes}, KSPACE_RW_ACCESS);
```

变成：

```cpp
iv.arch_info.conjunc_GSs = alloc_and_map(kmmu, npg, 12, va_alloc_up(total_bytes, 12), KSPACE_RW_ACCESS, true);
```

一行。`pages_set`、`map`、`interval` 构造全部打包。手写也不会漏了。

这个 helper 没有框架的负担——不引入配置表、不改变执行模型、不要求迁移所有现有代码。纯粹是把重复的机械模式压缩掉。要不要落地这个折中方案？

---

## 5. 决策树

```
init.elf 还会加新字段吗？
├── 不会（冻结了）→ 不引入任何东西，这次 pages_set 补上就是终点
└── 会 → 还会改 Phase 3b 吗？
    ├── 不会 → 同上
    └── 会 → 这个 15 行的 helper 值得加，防止第二次 pages_set 漏掉
          └── 迁移所有现有调用 → 要不要做？（10 分钟替换，保未来）
```

_本文档不做出决策，只记录矛盾。_
