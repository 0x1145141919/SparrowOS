# Panic Memory Broadcast — 坠机后残留数据回收方案

## 动机

SparrowOS 运行期间，PT (Processor Trace) 将分支数据无中断地写入动态分配的环缓冲。若内核 panic，这些缓存中保留着坠机前最后几百万条分支的执行路径。如何在不依赖固定物理地址预留、不依赖 UEFI Runtime Service、不依赖额外硬件的前提下回收这些数据。

**原则**: 不预先保留任何固定内存块（除一个 16 字节的信标位），全部通过 FPA 动态管理，panic 时用内存自身的 FREE 页面搭建寻址链。

## 关键先决条件

### DRAM 保持

warm reset (0xCF9, bit1=0) 后，DRAM 内容是否保持不变？

**必须测试**: 启动 SparrowOS → 在物理地址 0x500 写已知魔数 → outb(0x06, 0xCF9) warm reset → init.elf 在最早阶段读 0x500 验证值是否存活。

有以下情况需应对：
| 条件 | 结论 |
|------|------|
| warm reset 后 0x500 内容不变 | 方案成立，直接执行 |
| warm reset 后内容被清 | 需换用 cold reset 或通过 UEFI warm reboot 路径（ResetSystem 不走 DRAM 初始化）|
| cold reset/S5 后内容丢失 | 这是预期行为，不影响方案基本盘 |

若 MTL-P 的 warm reset 保 DRAM，则全部方案成立。

## 数据结构

### 1. PANIC_MAILBOX — 单一固定信标 (16 字节)

物理地址 `0x500`（BIOS 数据区，通常无覆盖风险。也可选 `0x9FC00` 或 `0x600`—取决于实测哪个区域不被 BIOS reset 践踏）。

```c
// 位于物理地址 0x500
struct panic_mailbox {
    uint64_t signature;           // "SPRW_PN\0"
    uint32_t chain_head_page;     // 链表首节点所在的 4K 页号
    uint32_t chain_node_count;    // 链表节点总数
};
```

唯一的硬编码位置。其他所有数据均通过链表链接。

### 2. Panic Chain Node — 在 FREE 页面写的链表节点

panic 时从 pages_arr 扫描 FREE 状态的物理页，写入节点结构。每个节点描述一块 PT buffer 并指向下一个节点。

```c
struct panic_chain_node {
    uint64_t signature;           // "CHAIN_NODE" 魔数
    uint32_t next_page;          // 下一个节点的 4K 页号（0=末尾）
    uint32_t cpu_id;             // 这个节点描述的 PT buffer 所属 CPU
    uint64_t pt_buf_phys;        // PT buffer 的物理基址
    uint64_t pt_buf_size;        // PT buffer 分配大小
    uint64_t pt_write_offset;    // Panic 时 PT 写到了 buffer 的哪个偏移
    uint64_t pt_status;          // IA32_RTIT_STATUS 的快照
    uint64_t tsc_at_panic;       // Panic 时刻的 TSC
    uint8_t  reserved[3968];     // 填满整个 4K 页
} __attribute__((packed));
```

**为什么用整页**: 每个链节点占用一个完整 4K 物理页。按链节点数 = CPU 数 + 1 计算，对 8 核消耗 9 个 FREE 页（～36KB），可忽略不计。这样做的好处是：
- 节点间完全独立，无需担心跨页链表断裂
- pages_arr 可以用页状态 `PAGE_STATE_PANIC_CHAIN` 标记这些页，重启后恢复阶段可直接靠扫描所有 `PANIC_CHAIN` 页重建链表

### 3. 分配时标记的页状态

`src/include/memory/all_pages_arr.h` 定义的 `page_state_t` 需要新增两个状态：

```c
enum page_state_t : uint8_t {
    // ... 现有状态 ...

    PAGE_STATE_PT_TRACE   = 0x0A,  // 正常运行时 FPA 分配 PT buffer 的页
    PAGE_STATE_PANIC_CHAIN = 0x0B, // Panic 时写入链表节点的页
};
```

好处：
- `pages_arr` 的页状态在重启后仍保存在 DRAM 中（除非内存被初始化）
- 恢复逻辑可以直接遍历 pages_arr，找到所有 `PANIC_CHAIN` 和 `PT_TRACE` 页
- 不依赖链表指针的完整性

## 工作流

### 阶段 A: 正常运行时

```c
// kinit 第二阶段，free_pages_allocator_init 完成后
void pt_trace_init() {
    // 完全通过 FPA 动态分配，不保留固定地址
    for_each_cpu(cpu) {
        phyaddr_t buf = FreePagesAllocator::alloc(64 * 1024 * 1024,
            buddy_alloc_params{0}, PAGE_STATE_PT_TRACE, kurd);
        // pages_arr 中该区间每页 state=PAGE_STATE_PT_TRACE

        setup_topa_ring(buf, 64MB);
        wrmsr(IA32_RTIT_CTL, TRACEEN | OS | BRANCH_EN | TSC_EN | TOPA);
        // ← PT 开始无中断写 buffer，永远环形覆盖
    }
}
```

目标运行时开销: **零 CPU 介入**。

### 阶段 B: Panic 时 — 内存广播

```c
[[noreturn]] void panic(const char *msg) {
    // 1. 停止所有 CPU 的 PT
    for_each_cpu(cpu) {
        wrmsr_on_cpu(cpu, IA32_RTIT_CTL, 0);
        pt_bufs[cpu].write_offset = rdmsr(IA32_RTIT_OUTPUT_MASK_PTRS) & 0x7F;
        pt_bufs[cpu].status = rdmsr(IA32_RTIT_STATUS);
    }

    // 2. 在 FREE 页上构建链表
    uint16_t node_count = cpu_count + 1; // CPU buffer 节点 + 一个总元数据节点
    uint32_t head_page = scan_free_pages_and_write_chain(pt_bufs, cpu_count);

    // 3. 将 pages_arr 中 PT_TRACE 页改为 PT_SAVED
    //    (可选：标记已回收，防下次 panic 新数据覆盖)
    for_each_cpu(cpu) {
        mark_pages(pt_bufs[cpu].phys, pt_bufs[cpu].size, PAGE_STATE_PT_SAVED);
    }

    // 4. 写 mailbox
    volatile auto* mb = (panic_mailbox*)PA(0x500);
    mb->signature      = 0x53505257505F504E;  // "SPRW_PN\0"
    mb->chain_head_page = head_page;
    mb->chain_node_count = node_count;
    clwb(mb);  // flush mailbox 的 cacheline
    sfence();

    // 5. warm reset
    outb(0x06, 0xCF9);
    __builtin_unreachable();
}
```

#### `scan_free_pages_and_write_chain` 细节

```c
uint32_t scan_free_pages_and_write_chain(
    pt_buf_info* bufs, uint16_t cpu_count)
{
    uint32_t prev_page = 0;
    uint32_t head_page = 0;

    // 先写总元数据节点（CPU0 PT buffer 信息 + panic 上下文切换）
    // 然后写每个 CPU 的节点

    for (int i = 0; i < cpu_count + 1; i++) {
        // 从 pages_arr 找一个 FREE 页
        uint64_t free_pa = find_free_page();
        if (free_pa == 0) break; // 没有 FREE 页了 → 中断链

        auto* node = (panic_chain_node*)PA(free_pa);
        memset(node, 0, 4096);
        node->signature = 0x434841494E5F4E44; // "CHAIN_NOD"
        node->next_page = 0;

        if (i == 0) {
            // 元数据节点
            node->cpu_id = 0xFF;  // 特殊值
            node->pt_buf_phys = bufs[0].phys;
            node->pt_write_offset = bufs[0].write_offset;
            node->tsc_at_panic = rdtsc();
            // 还可以塞更多上下文: 栈回溯, CR3, 最后中断向量...
        } else {
            node->cpu_id = bufs[i-1].cpu;
            node->pt_buf_phys = bufs[i-1].phys;
            node->pt_buf_size = bufs[i-1].size;
            node->pt_write_offset = bufs[i-1].write_offset;
            node->pt_status = bufs[i-1].status;
        }

        // 更新页状态
        pages_arr[free_pa >> 12].state = PAGE_STATE_PANIC_CHAIN;

        if (prev_page)
            ((panic_chain_node*)PA(prev_page << 12))->next_page = free_pa >> 12;
        else
            head_page = free_pa >> 12;

        prev_page = free_pa >> 12;

        clwb_range(free_pa, 4096); // flush 整个节点页
    }

    sfence();
    return head_page;
}
```

### 阶段 C: 重启后回收 — init.elf 阶段

init.elf 是第一个获取物理内存控制权的软件，页表尚未建立，可以直接用物理地址访问。

```c
// init.elf 的 main() 中，在 physical_memory_init() 之后、kernel 加载之前

void check_and_rescue_panic() {
    // 1. 检查 mailbox
    volatile auto* mb = (panic_mailbox*)PA(0x500);
    if (mb->signature != 0x53505257505F504E)
        return;  // 没有历史 panic，正常启动

    console_printf("=== Previous Panic Detected! Rescuing PT Trace... ===\n");

    // 2. 遍历链表
    uint32_t page = mb->chain_head_page;
    for (int i = 0; i < mb->chain_node_count && page; i++) {
        auto* node = (panic_chain_node*)PA(page << 12);
        if (node->signature != 0x434841494E5F4E44)
            break;  // 链表损坏，停止遍历

        // 3. 把 PT buffer 拷贝到 initramfs 的预留区域
        char filename[64];
        if (node->cpu_id == 0xFF) {
            snprintf(filename, 64, "panic_meta.bin");
            // 存元数据
        } else {
            snprintf(filename, 64, "pt_trace_cpu%u.bin", node->cpu_id);
            copy_pt_buffer_to_initramfs(filename,
                node->pt_buf_phys, node->pt_buf_size,
                node->pt_write_offset);
        }

        page = node->next_page;
    }

    // 4. 清 mailbox 签名，防重启循环
    mb->signature = 0;
    clwb(mb);

    // 5. 将 PT_TRACE 和 PANIC_CHAIN 页归还给分配器
    // (init.elf 的 page_allocator 或后续 SparrowOS 的 FPA 接管)
    console_printf("=== PT Trace rescued. Saved to initramfs. ===\n");
}
```

### 阶段 D: SparrowOS 正常启动后

initramfs 中多了 `pt_trace_cpu*.bin` 文件。SparrowOS 的 kshell 或 `/klog` 目录下可以通过以下方式暴露:

```
ksh> ls /kernel/klog/
  pt_trace_cpu0.bin
  pt_trace_cpu1.bin
  pt_trace_cpu2.bin
  panic_meta.bin

ksh> cat /kernel/klog/panic_meta.bin
  Panic at TSC: 0x1A2B3C4D5E6F
  CPUs online: 4
  ...

ksh> trace decode /kernel/klog/pt_trace_cpu0.bin
  (需要 libipt 支持)
```

或通过串口导出后，在主机上用 `ptxed` 解码:

```bash
$ serial_get pt_trace_cpu0.bin
$ ptxed --pt pt_trace_cpu0.bin --elf kernel.elf | head -100
```

## 边界情况

| 情况 | 应对 |
|------|------|
| FreePagesAllocator 自身崩溃 | panic_handler 不再依赖 FPA，直接从 pages_arr 线性扫描找 FREE 页 |
| 无 FREE 页可用 | 退化为只写 mailbox，不建链表。修复者手动搜 pages_arr 所有 PT_TRACE 页 |
| warm reset 清 DRAM | 放弃救援。这是预期行为，无需处理 |
| 链表节点在 warm reset 过程中被 BIOS 踩踏 | pages_arr 的状态位提供冗余恢复路径 |
| 多核 panic，部分核心没来得及关 PT | panic 中断广播到所有核心，统一处理 |

## PT ToPA 单 entry 环缓冲实现概要

为了配合动态分配，PT 使用最简单的 ToPA 配置：

```
ToPA table (单 entry):
┌─────────────────────┐
│ BASE = buffer_phys  │ ← FPA 分配的一块连续物理地址
│ SIZE = 64MB(2^26)   │
│ INT  = 1            │ ← 到末尾时发 PMI（但环缓冲模式下不处理）
│ END  = 1            │ ← 只有一个 entry
└─────────────────────┘
```

写 IA32_RTIT_OUTPUT_MASK_PTRS 时设 WRAP=1，使 PT 在写到末尾后回绕从头继续。这样实现了一个纯硬件的环形缓冲区。

## 后续扩展

- **多 buffer 切换**: ToPA 多 entry 模式可以实现无数据丢失的双缓冲（PT 写 buffer A 时，消费者读 buffer B）。但当前方案中消费者是"永不读取的环缓冲"，panic 才读，双缓冲无意义。
- **解码符号化**: 将 `ptxed` 解码逻辑编译为 SparrowOS 本地命令，直接在 kshell 里看解码后的函数路径。
- **PMI 采样**: 非 panic 场景下，利用 ToPA INT 中断做在线采样（如每写满 1MB 触发一个中断，采集当前 RIP）。
