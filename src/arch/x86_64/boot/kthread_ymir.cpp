// ════════════════════════════════════════════════════════════════
// kthread_ymir.cpp — 内核线程初始化（始祖线程 YMIR 及其派生线程）
//
// 从 kinit.cpp 拆出（2026-09-18）：kinit 只留「内存成熟 → 跳入线程运行时」的
// 引导胶水；凡由 kthread_ymir 派生/管理的线程初始化集中于此。
// 接口与符号声明见 src/include/boot/kthread_ymir.h。
//
// 【测试分支（if_real_init==false）】：
//   2026-09-18 起由 MMU（Kspace 三接口 + invalidate_tlb）压测占用（鸠占鹊巢）。
//   旧调度器/WRAITH 测试逻辑已移除，git 兜底；设计见 Docs/Memory/MMU压测落地设计.md。
//
// 打点纪律：运行时串口（bsp_kout）是 UDP 式的、会被多核交错撕裂——**不可靠**。
//   故本测试的所有进度/诊断只写 wraith_test_ring（关中断临界区 + 拿锁写内存），
//   结束时落 core → ring-dump 打捞。串口只留 wraith_freeze 的 #TB# 兜底记号；
//   整机停机的**权威判据**是 outb(0x80,0xDB) 魔法断点 → QEMU STOP（QMP 事件）。
// ════════════════════════════════════════════════════════════════
#include "boot/kthread_ymir.h"

#include "abi/os_error_definitions.h"
#include "util/kout.h"
#include "util/arch/x86-64/cpuid_intel.h"                    // fast_get_processor_id
#include "util/kshell.h"                                    // kshell_framework_t
#include "arch/x86_64/core_hardwares/i8042.h"               // i8042_char_subscriber_init
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"   // nvme_parallel_init_all
#include "Scheduler/kthread_abi.h"                          // creat_kthread / kthread_creating_package / kthread_sleep

#ifdef KTHREAD_TEST_SCENARIO
#include "util/wraith_probe.h"                              // wraith::rsp_now / gs_now / now_running_task
#include "util/debug_tmp_ring_buff.h"                       // debug_tmp_ring_buff（测试专用环）
#include "kcirclebufflogMgr.h"                              // DmesgRingBuffer_soul
#include "memory/AddresSpace.h"                             // Kspace_pinterval_alloc_and_map / KspacePageTable / kspace_vm_table
#include "memory/memory_base.h"                             // vm_interval / VM_DESC / pgaccess / page_state_t / buddy_alloc_params
#include "memory/FreePagesAllocator.h"                      // FreePagesAllocator::alloc/free/get_total_budget_bytes
#include "memory/main_phyaddr_access_window.h"              // PHYACC_VA（物理主窗口）
#include "arch/x86_64/mem_init.h"                            // logical_processor_count
#include "util/lock.h"                                      // spinlock_interrupt_about_guard
#endif

// ── 运行模式：true = 业务初始化 / false = 测试初始化（当前为 MMU 压测） ──
// 声明在 kthread_ymir.h（供外部置位）；此处给初值。
#ifdef KTHREAD_TEST_SCENARIO
bool if_real_init = false;      // 测试构建默认走测试初始化
#else
bool if_real_init = true;       // 正常构建恒业务初始化
#endif
// 是否派生 BQ 超时扫描线程（“兜底计时器”）；两档都可开关。
bool if_bq_sweeper = true;
// ────────────────────────────────────────────────────────────────
// 测试 / 压测线程
// ────────────────────────────────────────────────────────────────

// Collatz 序列：调度器压力 / 并行性测试载体；返回其运行所在 CPU id。
void* Collatz_kthread(void* init_value){
    uint64_t value = (uint64_t)init_value;
    uint64_t loop_count = 0;
    while (true) {
        if (value & 1) {
            value = value * 3 + 1;
        } else {
            value >>= 1;
        }
        loop_count++;
        if (value == 1) {
            return (void*)fast_get_processor_id();
        }
    }
}

constexpr uint8_t test_kthread_count = 100;
uint64_t test_kthreads[test_kthread_count];

// ────────────────────────────────────────────────────────────────
// 始祖线程
// ────────────────────────────────────────────────────────────────

// 派生 BQ 超时扫描线程（两档共用）。
static void spawn_bq_sweeper() {
    kthread_creating_package pkg = {};
    pkg.func_raw   = (uint64_t)bq_timeout_sweeper;
    pkg.args[0]    = (uint64_t)nullptr;
    pkg.launch_pid = 0;
    KURD_t kurd2{};
    uint64_t tid = creat_kthread(&pkg, &kurd2);
    if (error_kurd(kurd2)) {
        bsp_kout << "[BQ] sweeper thread spawn failed" << kendl;
    } else {
        bsp_kout << "[BQ] sweeper thread tid=" << tid << kendl;
    }
}

void*kthread_ymir(void*null){//所有内核线程的始祖之"尤米尔线程"（出自进击的巨人）
    (void)null;
    KURD_t kurd = KURD_t();

    if (if_real_init) {
        // ── 业务初始化 ──
        if (if_bq_sweeper) spawn_bq_sweeper();
        i8042_char_subscriber_init();
        //pcie_text_praser();
        // 并行初始化所有 NVMe 控制器（每控制器一线程，共享 u64 汇报画板，≤5s 轮询提前退出）
        nvme_parallel_init_all();
        //text_input_subscriber_init();

        // 初始化 kshell 框架
        kurd = kshell_framework_t::Init();
        if (error_kurd(kurd)) {
            bsp_kout << "[KSHELL] Failed to initialize framework!" << kendl;
        } else {
            bsp_kout << "[KSHELL] Framework initialized, ready for commands" << kendl;
        }
    }
#ifdef KTHREAD_TEST_SCENARIO
    else {
        // ── 测试初始化：MMU 压测（鸠占鹊巢）──
        // 业务/调度噪声对本测试无关，默认不起（如需可自行加）。
        mmu_test_main();
    }
#endif

    while (true)
    {
        kthread_sleep(1000000);
    }

    return nullptr;
}

// ════════════════════════════════════════════════════════════════
// 测试分支实现 · MMU 压测（仅 -DKTHREAD_TEST_SCENARIO）
// 设计：Docs/Memory/MMU压测落地设计.md
//   · 三接口：Kspace_pinterval_alloc_and_map / Kspace_phyaddr_direct_map / Kspace_phyaddr_direct_unmap
//   · 物理页一律走 FPA 圈地（kernel_pinned），测试水位线 = 总 FPA 预算 × 75%
//   · 双窗口验证：PHYACC_VA(phys) ↔ 新鲜映射窗
//   · invalidate_tlb：同 VA 改投不同物理页再读（陈旧 TLB → 可观测错值）
// 观测：g_mmu_* 探针 + g_mmu_ledger 断言账本（tool 直接按符号读）+ wraith_test_ring 文本。
// ════════════════════════════════════════════════════════════════
#ifdef KTHREAD_TEST_SCENARIO

// ── 测试专用日志环（ring-dump.py --symbol wraith_test_ring）────
debug_tmp_ring_buff* wraith_test_ring = nullptr;
alignas(debug_tmp_ring_buff) static uint8_t g_wraith_ring_obj[sizeof(debug_tmp_ring_buff)];
static DmesgRingBuffer_soul g_wraith_ring_soul;

static void wraith_test_ring_init() {
    constexpr uint64_t BYTES = 1ull << 20;   // 1 MiB
    KURD_t k = KURD_t();
    auto pb = FreePagesAllocator::alloc(BYTES, BUDDY_ALLOC_DEFAULT_FLAG,
                                        page_state_t::kernel_pinned, k);
    if (pb == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(k)) return;
    g_wraith_ring_soul = { (void*)PHYACC_VA(pb), BYTES, 0, 0 };
    wraith_test_ring = new (g_wraith_ring_obj) debug_tmp_ring_buff(&g_wraith_ring_soul);
}

// 打点：关中断临界区内拿锁写环（内存），结束落 core 后打捞。【不写串口】
#define WT_LOG(...)                                                        \
    do {                                                                   \
        if (wraith_test_ring) {                                            \
            spinlock_interrupt_about_guard __wt_g(wraith_test_ring->lock); \
            wraith_test_ring->print(__VA_ARGS__);                          \
        }                                                                  \
    } while (0)

// ── 断言账本（tool 按符号读；失败即截停）──────────────────────
struct mmu_ledger_t {
    volatile uint64_t n_total;
    volatile uint64_t n_pass;
    volatile uint64_t n_fail;
    volatile uint64_t first_fail;   // 失败用例编码（见 case code）
};
mmu_ledger_t g_mmu_ledger = {};

// ── 探针（tool 按符号读）──────────────────────────────────────
volatile uint64_t g_mmu_mode            = 0;   // 0=full 1=fonly 2=pf 4=repro
volatile uint64_t g_mmu_iters           = 0;   // 热循环轮数
volatile uint64_t g_mmu_mismatch        = 0;   // 陈旧 TLB/错值命中数
volatile uint64_t g_mmu_presence_fail   = 0;   // unmap 后仍 present 计数
volatile uint64_t g_mmu_pf_expected     = 0;   // 期望 #PF 命中标志（I-PF）
volatile uint64_t g_mmu_fpa_alloc       = 0;   // 本测试 FPA 圈地次数
volatile uint64_t g_mmu_fpa_free        = 0;   // 本测试 FPA 归还次数
volatile uint64_t g_mmu_outstanding_bytes = 0;
volatile uint64_t g_mmu_budget_bytes    = 0;   // 水位线
volatile uint64_t g_mmu_huge1g_hits     = 0;   // 1GB 分支命中（PDPTE_HUGE 增量）
volatile uint64_t g_mmu_huge1g_fallback = 0;   // 1GB 拿不到（降级）
volatile uint64_t g_mmu_shootdown_count = 0;   // 触发 shootdown 次数（S 段用）
volatile uint64_t g_mmu_last_1g_va = 0;        // I-4c 最近一次 1GB 映射 VA
volatile uint64_t g_mmu_last_1g_pa = 0;        // I-4c 最近一次 1GB 物理基

namespace {

// ── 裸端口 I/O（仅 wraith_freeze 的 #TB# 兜底记号用；权威停机靠魔法断点/QMP）──
inline void io_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
inline uint8_t io_inb(uint16_t port) {
    uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
inline void io_outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}
void uart_marker(const char* s) {              // 有界 THRE 轮询后写标记（best-effort）
    for (const char* p = s; *p; ++p) {
        uint32_t spins = 200000;
        while (!(io_inb(0x3FD) & 0x20) && --spins) {}
        io_outb(0x3F8, (uint8_t)*p);
    }
}

// ── fw_cfg 选模式（QEMU opt/sparrow/test；缺失则默认）──
// 接口：selector=0x510(16bit)，data=0x511(8bit)；多字节字段大端。
static uint32_t be32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | io_inb(0x511);
    return v;
}
static bool name_eq56(const char* nm, const char* want) {
    for (int i = 0; i < 56; ++i) {
        char a = nm[i], b = want[i];
        if (a != b) return false;
        if (b == '\0') return true;
    }
    return true;
}
static bool fwcfg_find(const char* want, uint16_t* out_sel, uint32_t* out_size) {
    io_outw(0x510, 0x0000);                        // signature
    char sig[4];
    for (int i = 0; i < 4; ++i) sig[i] = (char)io_inb(0x511);
    if (!(sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U')) return false;
    io_outw(0x510, 0x0019);                        // file directory
    uint32_t count = be32();
    for (uint32_t i = 0; i < count && i < 512; ++i) {
        uint32_t size = be32();
        uint8_t sel_hi = io_inb(0x511);            // 顺序读，避免 | 操作数求值顺序未定义
        uint8_t sel_lo = io_inb(0x511);
        uint16_t sel  = (uint16_t)((sel_hi << 8) | sel_lo);
        (void)io_inb(0x511); (void)io_inb(0x511);  // reserved
        char nm[56];
        for (int j = 0; j < 56; ++j) nm[j] = (char)io_inb(0x511);
        if (name_eq56(nm, want)) { *out_sel = sel; *out_size = size; return true; }
    }
    return false;
}
static void fwcfg_read_first(uint16_t sel, char* buf, uint32_t cap) {
    io_outw(0x510, sel);
    for (uint32_t i = 0; i < cap; ++i) buf[i] = (char)io_inb(0x511);
    buf[cap - 1] = '\0';
}
static bool str_contains(const char* hay, const char* needle) {
    for (const char* h = hay; *h; ++h) {
        const char* a = h; const char* b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
static void fwcfg_select_mode() {
    char buf[128];
    uint16_t sel = 0; uint32_t size = 0;
    if (!fwcfg_find("opt/sparrow/test", &sel, &size)) { g_mmu_mode = 0; return; }
    uint32_t cap = (size < sizeof(buf) - 1) ? size + 1 : (uint32_t)sizeof(buf);
    fwcfg_read_first(sel, buf, cap);
    if (str_contains(buf, "mode=pf"))            g_mmu_mode = 2;
    else if (str_contains(buf, "mode=repro"))    g_mmu_mode = 4;
    else if (str_contains(buf, "fonly"))         g_mmu_mode = 1;
    else                                         g_mmu_mode = 0;
}

// ── 断言原语 ──────────────────────────────────────────────
// 失败即截停：雷环留证（含 code）→ 串口 #TB#（best-effort）→ 魔法断点。
static void mmu_assert_fail(uint64_t code) {
    g_mmu_ledger.n_fail++;
    if (!g_mmu_ledger.first_fail) g_mmu_ledger.first_fail = code;
    WT_LOG("MMU FAIL code=%llx total=%llu\n",
           (unsigned long long)code, (unsigned long long)g_mmu_ledger.n_total);
    wraith_freeze("mmu-assert-fail");
}
#define MMU_ASSERT(cond, code)                    \
    do {                                          \
        g_mmu_ledger.n_total++;                   \
        if (cond) g_mmu_ledger.n_pass++;          \
        else mmu_assert_fail(code);               \
    } while (0)

// ── FPA 圈地（水位线保护）─────────────────────────────────
static phyaddr_t mmu_fpa_alloc(uint64_t bytes, uint8_t align_log2) {
    if (g_mmu_outstanding_bytes + bytes > g_mmu_budget_bytes) {
        WT_LOG("MMU watermark hit need=%llu outstanding=%llu budget=%llu\n",
               (unsigned long long)bytes,
               (unsigned long long)g_mmu_outstanding_bytes,
               (unsigned long long)g_mmu_budget_bytes);
        wraith_freeze("fpa-watermark");
    }
    buddy_alloc_params p = BUDDY_ALLOC_DEFAULT_FLAG;
    p.align_log2 = align_log2;
    KURD_t k;
    phyaddr_t pa = FreePagesAllocator::alloc(bytes, p, page_state_t::kernel_pinned, k);
    if (pa == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(k)) {
        WT_LOG("MMU FPA alloc FAIL bytes=%llu align=%u\n",
               (unsigned long long)bytes, (unsigned)align_log2);
        wraith_freeze("fpa-watermark");
    }
    g_mmu_outstanding_bytes += bytes;
    g_mmu_fpa_alloc++;
    return pa;
}
static void mmu_fpa_free(phyaddr_t pa, uint64_t bytes) {
    (void)FreePagesAllocator::free(pa, bytes);
    g_mmu_outstanding_bytes -= bytes;
    g_mmu_fpa_free++;
}

// ── 采样点（大页只抽样，避免 1GB 逐字遍历）────────────────
static constexpr int MMU_SAMPLE_N = 12;
static uint64_t mmu_sample_idx(uint64_t words, int i) {
    if (i <= 0) return 0;
    if (i >= MMU_SAMPLE_N - 1) return words - 1;
    return (words - 1) * (uint64_t)i / (MMU_SAMPLE_N - 1);
}
static uint64_t mmu_pat(uint64_t seed, int i) {
    return 0x9E3779B97F4A7C15ull * (seed + 1) ^ ((seed + (uint64_t)i) << 17);
}
// 经物理主窗口写采样点
static void mmu_fill_phys(phyaddr_t pa, uint64_t npages, uint64_t seed) {
    volatile uint64_t* p = (volatile uint64_t*)PHYACC_VA(pa);
    uint64_t words = npages * 512;
    for (int i = 0; i < MMU_SAMPLE_N; ++i) p[mmu_sample_idx(words, i)] = mmu_pat(seed, i);
}
// 经映射窗验证采样点（== 物理窗写入值）
static bool mmu_verify_va(vaddr_t va, uint64_t npages, uint64_t seed) {
    volatile uint64_t* p = (volatile uint64_t*)va;
    uint64_t words = npages * 512;
    for (int i = 0; i < MMU_SAMPLE_N; ++i)
        if (p[mmu_sample_idx(words, i)] != mmu_pat(seed, i)) return false;
    return true;
}
// 经映射窗写采样点（另一 seed）→ 物理窗读回
static void mmu_fill_va(vaddr_t va, uint64_t npages, uint64_t seed) {
    volatile uint64_t* p = (volatile uint64_t*)va;
    uint64_t words = npages * 512;
    for (int i = 0; i < MMU_SAMPLE_N; ++i) p[mmu_sample_idx(words, i)] = mmu_pat(seed, i);
}
static bool mmu_verify_phys(phyaddr_t pa, uint64_t npages, uint64_t seed) {
    volatile uint64_t* p = (volatile uint64_t*)PHYACC_VA(pa);
    uint64_t words = npages * 512;
    for (int i = 0; i < MMU_SAMPLE_N; ++i)
        if (p[mmu_sample_idx(words, i)] != mmu_pat(seed, i)) return false;
    return true;
}

// 翻译是否命中“指定物理”（非故障判据）。
// 注意：v_to_phyaddrtraslation 对“已清 PTE”也返回 SUCCESS（叶 present 位未校验）
// ⇒ 不能用其成功与否判 present，必须比对译出的物理地址。
static bool mmu_mapped_to(vaddr_t va, phyaddr_t expect) {
    phyaddr_t out = 0;
    KURD_t k = KspacePageTable::v_to_phyaddrtraslation(va, out);
    return !error_kurd(k) && out == expect;
}
static bool mmu_vt_free(vaddr_t va) {
    return kspace_vm_table->search(va) == nullptr;
}

// 映射一个物理区间（自动 VA）
static vaddr_t mmu_map_auto(phyaddr_t pa, uint64_t npages, KURD_t* kout) {
    vm_interval iv = { .vpn = 0, .ppn = pa >> 12, .npages = npages, .access = KspacePageTable::PG_RW };
    return Kspace_pinterval_alloc_and_map(iv, kout);
}
// 在指定 VA 映射（固定）
static KURD_t mmu_map_at(vaddr_t va, phyaddr_t pa, uint64_t npages) {
    vm_interval iv = { .vpn = va >> 12, .ppn = pa >> 12, .npages = npages, .access = KspacePageTable::PG_RW };
    return Kspace_phyaddr_direct_map(iv);
}
static KURD_t mmu_unmap(vaddr_t va, phyaddr_t pa, uint64_t npages) {
    vm_interval iv = { .vpn = va >> 12, .ppn = pa >> 12, .npages = npages, .access = KspacePageTable::PG_RW };
    return Kspace_phyaddr_direct_unmap(iv);
}
static vaddr_t mmu_scratch_va(uint64_t bytes) {
    return kspace_vm_table->alloc_available_space(bytes, 0);
}

// 案例编码（落账本，供 tool 识别）
enum : uint64_t {
    C_I1_VA0      = 0x101, C_I1_DUAL = 0x102, C_I1_UNMAP = 0x103, C_I1_PRES = 0x104,
    C_I2_MAP      = 0x201, C_I2_DUAL = 0x202, C_I2_UNMAP = 0x203,
    C_I3_STALE    = 0x301,
    C_I4_2M       = 0x401, C_I4_2M_MAP = 0x402, C_I4_2M_DUAL = 0x403,
    C_I4_1G_MAP   = 0x411, C_I4_1G_DUAL = 0x412,
    C_I5_BADIV    = 0x501, C_I5_NONK    = 0x502, C_I5_DBL   = 0x503,
    C_I6_LEAK     = 0x601,
    C_IPF_HIT     = 0x701,
};

// ── I-1：三接口基本往返 ────────────────────────────────────
static void mmu_case_i1() {
    WT_LOG("|I1|\n");
    constexpr uint64_t PG = 1;
    phyaddr_t pa = mmu_fpa_alloc(PG << 12, 12);
    mmu_fill_phys(pa, PG, 0xA1);
    KURD_t k;
    vaddr_t va = mmu_map_auto(pa, PG, &k);
    MMU_ASSERT(va != 0 && !error_kurd(k), C_I1_VA0);
    if (va) {
        MMU_ASSERT(mmu_verify_va(va, PG, 0xA1), C_I1_DUAL);   // 物理窗写→映射窗读
        mmu_fill_va(va, PG, 0xB1);
        MMU_ASSERT(mmu_verify_phys(pa, PG, 0xB1), C_I1_DUAL); // 映射窗写→物理窗读
        KURD_t u = mmu_unmap(va, pa, PG);
        MMU_ASSERT(!error_kurd(u), C_I1_UNMAP);
        bool pres = mmu_mapped_to(va, pa);   // 仍指向原物理 ⇒ 未真正撤销
        bool vf   = mmu_vt_free(va);
        if (pres) { WT_LOG("I1 |P!| va=%llx\n", (unsigned long long)va); g_mmu_presence_fail++; }
        if (!vf)  WT_LOG("I1 |V!| va=%llx\n", (unsigned long long)va);
        MMU_ASSERT(!pres && vf, C_I1_PRES);
    }
    mmu_fpa_free(pa, PG << 12);
    WT_LOG("I1 done va=%llx\n", (unsigned long long)va);
}

// ── I-2：direct_map / direct_unmap 固定 VA 往返 ─────────────
static void mmu_case_i2() {
    WT_LOG("|I2|\n");
    constexpr uint64_t PG = 1;
    phyaddr_t pa = mmu_fpa_alloc(PG << 12, 12);
    mmu_fill_phys(pa, PG, 0xA2);
    vaddr_t va = mmu_scratch_va(PG << 12);
    MMU_ASSERT(va != 0, C_I2_MAP);
    if (va) {
        KURD_t m = mmu_map_at(va, pa, PG);
        MMU_ASSERT(!error_kurd(m), C_I2_MAP);
        MMU_ASSERT(mmu_verify_va(va, PG, 0xA2), C_I2_DUAL);
        mmu_fill_va(va, PG, 0xB2);
        MMU_ASSERT(mmu_verify_phys(pa, PG, 0xB2), C_I2_DUAL);
        KURD_t u = mmu_unmap(va, pa, PG);
        MMU_ASSERT(!error_kurd(u), C_I2_UNMAP);
        MMU_ASSERT(!mmu_mapped_to(va, pa) && mmu_vt_free(va), C_I2_UNMAP);
    }
    mmu_fpa_free(pa, PG << 12);
    WT_LOG("I2 done va=%llx\n", (unsigned long long)va);
}

// ── I-3：T-stale（同 VA 改投不同物理页，本核 invlpg 效力）────
static void mmu_case_i3() {
    WT_LOG("|I3|\n");
    constexpr uint64_t PG = 1;
    phyaddr_t paA = mmu_fpa_alloc(PG << 12, 12);
    phyaddr_t paB = mmu_fpa_alloc(PG << 12, 12);
    mmu_fill_phys(paA, PG, 0xAA);
    mmu_fill_phys(paB, PG, 0xBB);
    KURD_t k;
    vaddr_t va = mmu_map_auto(paA, PG, &k);
    MMU_ASSERT(va != 0 && !error_kurd(k), C_I3_STALE);
    if (va) {
        MMU_ASSERT(mmu_verify_va(va, PG, 0xAA), C_I3_STALE);  // 暖 TLB
        KURD_t u = mmu_unmap(va, paA, PG);                    // shootdown(self)
        MMU_ASSERT(!error_kurd(u), C_I3_STALE);
        KURD_t m = mmu_map_at(va, paB, PG);                   // 同 VA → B
        MMU_ASSERT(!error_kurd(m), C_I3_STALE);
        bool ok = mmu_verify_va(va, PG, 0xBB);                // 陈旧则读到 0xAA
        if (!ok) g_mmu_mismatch++;
        MMU_ASSERT(ok, C_I3_STALE);
        (void)mmu_unmap(va, paB, PG);
    }
    mmu_fpa_free(paA, PG << 12);
    mmu_fpa_free(paB, PG << 12);
    WT_LOG("I3 done va=%llx mismatch=%llu\n",
           (unsigned long long)va, (unsigned long long)g_mmu_mismatch);
}

// ── I-4：页尺寸阶梯（4KB/2MB；1GB 单列）────────────────────
static void mmu_case_i4(const char* title, uint64_t npages, uint8_t align_log2, uint64_t seed) {
    WT_LOG("|%s|\n", title);
    uint64_t bytes = npages << 12;
    phyaddr_t pa = mmu_fpa_alloc(bytes, align_log2);
    mmu_fill_phys(pa, npages, seed);
    KURD_t k;
    vaddr_t va = mmu_map_auto(pa, npages, &k);
    MMU_ASSERT(va != 0 && !error_kurd(k), C_I4_2M_MAP);
    if (va) {
        MMU_ASSERT(mmu_verify_va(va, npages, seed), C_I4_2M_DUAL);
        KURD_t u = mmu_unmap(va, pa, npages);
        MMU_ASSERT(!error_kurd(u), C_I4_2M_MAP);
    }
    mmu_fpa_free(pa, bytes);
    WT_LOG("%s done va=%llx npages=%llu\n",
           title, (unsigned long long)va, (unsigned long long)npages);
}

// ── I-4c：1GB 大页（先当有；拿不到记 fallback，不冻结）────────
static uint64_t mmu_huge_hits() {
    if (!kspace_pagetable_statistics) return 0;
    uint32_t pid = fast_get_processor_id();
    if (logical_processor_count && pid >= logical_processor_count)
        pid = pid % logical_processor_count;
    return kspace_pagetable_statistics[pid].pages_set.specific.x86_64.PDPTE_HUGE_set_count;
}
static void mmu_case_i4c_1gb() {
    WT_LOG("|I-4c/1GB|\n");
    constexpr uint64_t NPG   = 1ull << 18;      // 262144 页 = 1GB
    constexpr uint64_t BYTES = 1ull << 30;
    buddy_alloc_params p = BUDDY_ALLOC_DEFAULT_FLAG;
    p.align_log2 = 30;                          // 1GB 对齐
    KURD_t k;
    phyaddr_t pa = FreePagesAllocator::alloc(BYTES, p, page_state_t::kernel_pinned, k);
    if (pa == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(k)) {
        g_mmu_huge1g_fallback = 1;
        WT_LOG("I-4c: FPA 1GB unavailable\n");   // 现场记账，实跑定论
        return;
    }
    g_mmu_outstanding_bytes += BYTES; g_mmu_fpa_alloc++;
    g_mmu_last_1g_pa = (uint64_t)pa;
    mmu_fill_phys(pa, NPG, 0x47);
    uint64_t base_huge = mmu_huge_hits();
    vaddr_t va = mmu_map_auto(pa, NPG, &k);
    g_mmu_last_1g_va = (uint64_t)va;
    MMU_ASSERT(va != 0 && !error_kurd(k), C_I4_1G_MAP);
    if (va) {
        uint64_t now_huge = mmu_huge_hits();
        if (now_huge > base_huge) g_mmu_huge1g_hits = now_huge - base_huge;
        else { g_mmu_huge1g_fallback = 1; WT_LOG("I-4c: 1GB split (not HUGE)\n"); }
        MMU_ASSERT(mmu_verify_va(va, NPG, 0x47), C_I4_1G_DUAL);
        KURD_t u = mmu_unmap(va, pa, NPG);
        MMU_ASSERT(!error_kurd(u), C_I4_1G_MAP);
    }
    mmu_fpa_free(pa, BYTES);
    WT_LOG("I-4c done va=%llx huge_hits=%llu\n",
           (unsigned long long)va, (unsigned long long)g_mmu_huge1g_hits);
}

// ── 专项复现：1GB→4KB 交互 / 1GB 陈旧 TLB ───────────────────
static void mmu_case_repro1gb() {
    // --- 复现 A：1GB 后紧接 4KB map/unmap（原失败序）---
    mmu_case_i4c_1gb();
    vaddr_t v1g = (vaddr_t)g_mmu_last_1g_va;
    WT_LOG("R1gva=%llx\n", (unsigned long long)v1g);
    {
        phyaddr_t out = 0;
        KURD_t t = KspacePageTable::v_to_phyaddrtraslation(v1g, out);
        WT_LOG("R1gt result=%llx out=%llx\n",
               (unsigned long long)t.result, (unsigned long long)out);
    }
    {
        constexpr uint64_t PG = 1;
        phyaddr_t pa = mmu_fpa_alloc(PG << 12, 12);
        mmu_fill_phys(pa, PG, 0x99);
        KURD_t k;
        vaddr_t va = mmu_map_auto(pa, PG, &k);
        WT_LOG("R4kva=%llx\n", (unsigned long long)va);
        if (!va || error_kurd(k)) {
            WT_LOG("R4kmap FAIL raw=%llx\n", (unsigned long long)kurd_get_raw(k));
        } else {
            WT_LOG("R4kpres=%u\n", (unsigned)mmu_mapped_to(va, pa));
            KURD_t u1 = mmu_unmap(va, pa, PG);
            WT_LOG("R4ku1 result=%llx ev=%llx reason=%llx\n",
                   (unsigned long long)u1.result, (unsigned long long)u1.event_code,
                   (unsigned long long)u1.reason);
            WT_LOG("R4kvf=%u\n", (unsigned)mmu_vt_free(va));
        }
        mmu_fpa_free(pa, PG << 12);
    }
    // --- 复现 B：1GB 陈旧 TLB（同 VA 改投 1 页）---
    {
        constexpr uint64_t NPG = 1ull << 18, BYTES = 1ull << 30;
        buddy_alloc_params p = BUDDY_ALLOC_DEFAULT_FLAG; p.align_log2 = 30;
        KURD_t kk;
        phyaddr_t pA = FreePagesAllocator::alloc(BYTES, p, page_state_t::kernel_pinned, kk);
        if (!(pA == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(kk))) {
            mmu_fill_phys(pA, NPG, 0xA0);
            KURD_t mk; vaddr_t vA = mmu_map_auto(pA, NPG, &mk);
            if (vA && !error_kurd(mk)) {
                bool warm = mmu_verify_va(vA, NPG, 0xA0);
                WT_LOG("RBwarm=%u\n", (unsigned)warm);
                KURD_t uu = mmu_unmap(vA, pA, NPG);
                WT_LOG("RBunmap=%llx\n", (unsigned long long)uu.result);
                phyaddr_t pB = mmu_fpa_alloc(4096, 12); mmu_fill_phys(pB, 1, 0xB0);
                KURD_t m2 = mmu_map_at(vA, pB, 1);
                WT_LOG("RBmap2=%llx\n", (unsigned long long)m2.result);
                bool ok = mmu_verify_va(vA, 1, 0xB0);
                WT_LOG("RBsame=%u\n", (unsigned)ok);
                if (!ok) g_mmu_mismatch++;
                (void)mmu_unmap(vA, pB, 1);
                mmu_fpa_free(pB, 4096);
            }
            mmu_fpa_free(pA, BYTES);
        }
    }
    WT_LOG("Rdone\n");
}

// ── I-5：负例/边界（期望特定 KURD fail，不 panic）────────────
static void mmu_case_i5() {
    WT_LOG("|I5|\n");
    // npages=0
    {
        KURD_t k = KURD_t();
        vm_interval iv = { .vpn = 0, .ppn = 0x100, .npages = 0, .access = KspacePageTable::PG_RW };
        vaddr_t va = Kspace_pinterval_alloc_and_map(iv, &k);
        MMU_ASSERT(va == 0 && error_kurd(k), C_I5_BADIV);
    }
    // 非内核 VA：direct_map 必须拒绝
    {
        vm_interval iv = { .vpn = (0x1000) >> 12, .ppn = 0x100, .npages = 1, .access = KspacePageTable::PG_RW };
        KURD_t k = Kspace_phyaddr_direct_map(iv);
        MMU_ASSERT(error_kurd(k), C_I5_NONK);
    }
    // 双重 unmap：第二次必失败
    {
        constexpr uint64_t PG = 1;
        phyaddr_t pa = mmu_fpa_alloc(PG << 12, 12);
        mmu_fill_phys(pa, PG, 0xC5);
        KURD_t k;
        vaddr_t va = mmu_map_auto(pa, PG, &k);
        if (va && !error_kurd(k)) {
            KURD_t u1 = mmu_unmap(va, pa, PG);
            KURD_t u2 = mmu_unmap(va, pa, PG);
            if (error_kurd(u1))
                WT_LOG("I5 |D1!| result=%llx ev=%llx reason=%llx va=%llx pa=%llx\n",
                       (unsigned long long)u1.result, (unsigned long long)u1.event_code,
                       (unsigned long long)u1.reason,
                       (unsigned long long)va, (unsigned long long)pa);
            if (!error_kurd(u2)) WT_LOG("I5 |D2!|\n");
            MMU_ASSERT(!error_kurd(u1), C_I5_DBL);
            MMU_ASSERT(error_kurd(u2), C_I5_DBL);
        } else {
            WT_LOG("I5 |D0!|\n");
            MMU_ASSERT(false, C_I5_DBL);
        }
        mmu_fpa_free(pa, PG << 12);
    }
    WT_LOG("I5 done\n");
}

// ── I-6：泄漏对账（FPA/VM 表）────────────────────────────────
static void mmu_case_i6() {
    WT_LOG("|I6|\n");
    MMU_ASSERT(g_mmu_fpa_alloc == g_mmu_fpa_free, C_I6_LEAK);
    WT_LOG("I6 alloc=%llu free=%llu outstanding=%llu\n",
           (unsigned long long)g_mmu_fpa_alloc, (unsigned long long)g_mmu_fpa_free,
           (unsigned long long)g_mmu_outstanding_bytes);
}

// ── I-PF：故意 #PF（终局；期望命中 FAULT_FREEZE #WF#）────────
static void mmu_case_ipf() {
    WT_LOG("|PF|\n");
    constexpr uint64_t PG = 1;
    phyaddr_t pa = mmu_fpa_alloc(PG << 12, 12);
    mmu_fill_phys(pa, PG, 0xDD);
    KURD_t k;
    vaddr_t va = mmu_map_auto(pa, PG, &k);
    MMU_ASSERT(va != 0 && !error_kurd(k), C_IPF_HIT);
    KURD_t u = mmu_unmap(va, pa, PG);
    MMU_ASSERT(!error_kurd(u), C_IPF_HIT);
    MMU_ASSERT(!mmu_mapped_to(va, pa) && mmu_vt_free(va), C_IPF_HIT);
    // 故意踩空 → 期望 #PF 入口 FAULT_FREEZE（#WF# + 魔法断点）
    g_mmu_pf_expected = 1;
    WT_LOG("I-PF deliberate fault va=%llx\n", (unsigned long long)va);
    uart_marker("#EXPECT-PF#");       // 串口 best-effort；权威判据=#WF#/QMP
    volatile uint64_t sink = *(volatile uint64_t*)va;
    (void)sink;
    MMU_ASSERT(false, C_IPF_HIT);     // 若返回：页表没撤干净（真异常）
}

}  // namespace

// ── 测试入口 ──────────────────────────────────────────────────
void mmu_test_main() {
    wraith_test_ring_init();
    fwcfg_select_mode();
    // 水位线 = 总 FPA 预算 × 75%
    uint64_t total = FreePagesAllocator::get_total_budget_bytes();
    g_mmu_budget_bytes = total / 4 * 3;
    WT_LOG("MMU main: mode=%llu total_budget=%llu watermark=%llu\n",
           (unsigned long long)g_mmu_mode,
           (unsigned long long)total,
           (unsigned long long)g_mmu_budget_bytes);

    if (g_mmu_mode == 1) {          // fonly：功能轮
        mmu_case_i1(); mmu_case_i2(); mmu_case_i3();
        mmu_case_i4("I-4a/4KB", 1, 12, 0x44);
        mmu_case_i4("I-4b/2MB", 512, 21, 0x45);
        mmu_case_i5(); mmu_case_i6();
    } else if (g_mmu_mode == 2) {   // pf：故意 #PF（终局）
        mmu_case_ipf();
    } else if (g_mmu_mode == 4) {   // repro：1GB→4KB 交互 / 陈旧 TLB
        mmu_case_repro1gb();
    } else {                        // full：功能轮 + 1GB（+ S 段后续接入）
        mmu_case_i1(); mmu_case_i2(); mmu_case_i3();
        mmu_case_i4("I-4a/4KB", 1, 12, 0x44);
        mmu_case_i4("I-4b/2MB", 512, 21, 0x45);
        mmu_case_i4c_1gb();     // I-4c/1GB（先当有；拿不到记 fallback 继续）
        mmu_case_i5();          // 原失败序：1GB 紧接 4KB
        mmu_case_i6();
    }

    WT_LOG("MMU done: total=%llu pass=%llu fail=%llu fail_code=%llx\n",
           (unsigned long long)g_mmu_ledger.n_total,
           (unsigned long long)g_mmu_ledger.n_pass,
           (unsigned long long)g_mmu_ledger.n_fail,
           (unsigned long long)g_mmu_ledger.first_fail);
    wraith_freeze("planned");
    for (;;) kthread_sleep(1000000);
}

// 截停闸门：串口 #TB#/reason（best-effort）→ 环留证 → outb(0x80,0xDB) 魔法断点
// （QEMU vm_stop；host 侧以 QMP STOP 事件为权威停机判据）→ cli;hlt。
void wraith_freeze(const char* reason) {
    uart_marker("#TB#");
    uart_marker(reason);          // 串口带 reason（可能被交错；权威以 QMP + ring 为准）
    WT_LOG("TB reason=%s pid=%u rsp=%llx\n", reason,
               (unsigned)fast_get_processor_id(),
               (unsigned long long)wraith::rsp_now());
    io_outb(0x80, 0xDB);
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

#endif  // KTHREAD_TEST_SCENARIO
