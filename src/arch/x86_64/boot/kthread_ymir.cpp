// ════════════════════════════════════════════════════════════════
// kthread_ymir.cpp — 内核线程初始化（始祖线程 YMIR 及其派生线程）
//
// 从 kinit.cpp 拆出（2026-09-18）：kinit 只留「内存成熟 → 跳入线程运行时」的
// 引导胶水；凡由 kthread_ymir 派生/管理的线程初始化集中于此。
// 后续调度器/线程压测（硬编码场景）落点亦在此文件。
// 接口与符号声明见 src/include/boot/kthread_ymir.h。
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
#include "util/wraith_probe.h"                              // WRAITH_LOG / wraith::now_running_task / rsp_now
#include "Scheduler/task.h"                                 // task::get_tid
#endif
bool if_real_init=false;
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

void*kthread_ymir(void*null){//所有内核线程的始祖之"尤米尔线程"（出自进击的巨人）
    (void)null;
    KURD_t kurd = KURD_t();
    {
        // 启动 BQ 超时扫描线程
        kthread_creating_package pkg = {};
        pkg.func_raw = (uint64_t)bq_timeout_sweeper;
        pkg.args[0]  = (uint64_t)nullptr;
        pkg.launch_pid = 0;
        KURD_t kurd2{};
        uint64_t tid = creat_kthread(&pkg, &kurd2);
        if (error_kurd(kurd2)) {
            bsp_kout << "[BQ] sweeper thread spawn failed" << kendl;
        } else {
            bsp_kout << "[BQ] sweeper thread tid="<<tid  << kendl;
        }
    }
    if(if_real_init){
    i8042_char_subscriber_init();
    //pcie_text_praser();
    // 并行初始化所有 NVMe 控制器（每控制器一线程，共享 u64 汇报画板，≤5s 轮询提前退出）
    nvme_parallel_init_all();
    //text_input_subscriber_init();



    // 初始化 kshell 框架
    kurd=kshell_framework_t::Init();
    if (error_kurd(kurd)) {
        bsp_kout << "[KSHELL] Failed to initialize framework!" << kendl;
    } else {
        bsp_kout << "[KSHELL] Framework initialized, ready for commands" << kendl;
    }
    }
else
{
    kthread_test_main();

}
    while (true)
    {
        kthread_sleep(1000000);
    }

    return nullptr;
}

// ════════════════════════════════════════════════════════════════
// WRAITH 验收 · 测试分支实现（仅 -DKTHREAD_TEST_SCENARIO）
// 见 kthread_ymir.h 顶部说明；战报 Docs/Debug/WRAITH/；工具 Tools/wraith/。
// ════════════════════════════════════════════════════════════════
#ifdef KTHREAD_TEST_SCENARIO

wraith_test_slot g_wraith_slots[WRAITH_TEST_MAX];
volatile uint64_t g_wraith_slot_count = 0;

// 测试线程入口（内部链接；func_raw 取的就是它的地址）。前向声明供 spawner 使用。
static void* wraith_worker_entry(void* arg);

namespace {

// 与 asm FAULT_FREEZE 同款：裸端口 I/O，不依赖任何内核服务/锁/分配。
inline void io_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
inline uint8_t io_inb(uint16_t port) {
    uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
void uart_marker(const char* s) {              // 有界 THRE 轮询后写标记
    for (const char* p = s; *p; ++p) {
        uint32_t spins = 200000;
        while (!(io_inb(0x3FD) & 0x20) && --spins) {}
        io_outb(0x3F8, (uint8_t)*p);
    }
}

enum wraith_role : uint64_t {
    WROLE_BURNER  = 1,   // 纯 CPU 竞争 + yield
    WROLE_SLEEPER = 2,   // sleep，由 kicker 跨核唤醒（打 F4 唤醒/偷取窗口）
    WROLE_SPAWNER = 3,   // 自派生（线程树）
    WROLE_EXITER  = 4,   // 跑一段后 exit（set_zombie，不 release ⇒ 栈停车）
};

int slot_claim(uint64_t tid, uint64_t role, uint64_t canary_addr, uint64_t canary_expected) {
    for (uint32_t i = 0; i < WRAITH_TEST_MAX; ++i) {
        if (!g_wraith_slots[i].in_use) {
            g_wraith_slots[i].tid             = tid;
            g_wraith_slots[i].task_ptr        = wraith::now_running_task();
            g_wraith_slots[i].role            = role;
            g_wraith_slots[i].seq             = 0;
            g_wraith_slots[i].last_cpu        = fast_get_processor_id();
            g_wraith_slots[i].canary_addr     = canary_addr;
            g_wraith_slots[i].canary_expected = canary_expected;
            g_wraith_slots[i].in_use          = 1;
            if (i + 1 > g_wraith_slot_count) g_wraith_slot_count = i + 1;
            return (int)i;
        }
    }
    return -1;
}

// 【热循环】：每轮先自校验浅层金丝雀（被异核写过⇒立即冻结留证），再按角色行为。
void wraith_hotloop(uint64_t role, int slot) {
    volatile uint64_t* canary = (volatile uint64_t*)g_wraith_slots[slot].canary_addr;
    const uint64_t expect = g_wraith_slots[slot].canary_expected;
    while (true) {
        if (*canary != expect) wraith_freeze("stack-canary-corrupt");
        g_wraith_slots[slot].seq++;
        g_wraith_slots[slot].last_cpu = fast_get_processor_id();

        switch (role) {
        case WROLE_BURNER:
            for (volatile uint64_t k = 0; k < 200000; ++k) {}
            kthread_yield();
            break;
        case WROLE_SLEEPER:
            kthread_sleep(50000);                    // kicker 会提前唤醒
            break;
        case WROLE_SPAWNER:
            if (g_wraith_slot_count < 48 && ((g_wraith_slots[slot].seq & 15) == 1)) {
                kthread_creating_package pkg = {};
                pkg.func_raw   = (uint64_t)wraith_worker_entry;
                pkg.args[0]    = (uint64_t)WROLE_BURNER;
                pkg.launch_pid = fast_get_processor_id();
                KURD_t k = KURD_t();
                creat_kthread(&pkg, &k);
            }
            kthread_yield();
            break;
        case WROLE_EXITER:
            for (volatile uint64_t k = 0; k < 500000; ++k) {}
            kthread_exit(0);                         // zombie；调用方不 release ⇒ 栈停车
            break;
        default:
            kthread_yield();
            break;
        }
    }
}

}  // namespace

// 线程入口：外层帧持金丝雀（浅），热循环在内层（深）——WRAITH 的浅写更易命中金丝雀。
static void* wraith_worker_entry(void* arg) {
    const uint64_t role = (uint64_t)arg;
    task* self = (task*)wraith::now_running_task();
    const uint64_t tid = self ? self->get_tid() : 0;
    volatile uint64_t canary = WRAITH_CANARY_MAGIC ^ tid;
    const int slot = slot_claim(tid, role, (uint64_t)&canary, (uint64_t)canary);
    if (slot < 0) {
        WRAITH_LOG("WT! slot-full tid=%llu\n", (unsigned long long)tid);
        for (;;) kthread_sleep(1000000);
    }
    WRAITH_LOG("WT+ tid=%llu role=%llu slot=%d\n",
               (unsigned long long)tid, (unsigned long long)role, slot);
    wraith_hotloop(role, slot);
    return nullptr;
}

namespace {
void* wraith_root(void* arg) {
    (void)arg;
    const uint64_t roles[] = { WROLE_BURNER, WROLE_BURNER, WROLE_SLEEPER,
                               WROLE_SPAWNER, WROLE_EXITER };
    // 派生阶段：多轮混合线程（自相似线程树的根）。
    for (int rep = 0; rep < 6; ++rep) {
        for (uint64_t r : roles) {
            kthread_creating_package pkg = {};
            pkg.func_raw   = (uint64_t)wraith_worker_entry;
            pkg.args[0]    = r;
            pkg.launch_pid = fast_get_processor_id();
            KURD_t k = KURD_t();
            creat_kthread(&pkg, &k);
        }
        kthread_sleep(20000);
    }
    // kicker 阶段：反复跨核唤醒 sleeper（打 F4「仍未切离窗口」）。
    constexpr uint64_t KICK_ROUNDS = 1500;           // ~1.5s（guest 时间）
    for (uint64_t round = 0; round < KICK_ROUNDS; ++round) {
        const uint64_t n = g_wraith_slot_count;
        for (uint64_t i = 0; i < n; ++i) {
            if (g_wraith_slots[i].in_use && g_wraith_slots[i].role == WROLE_SLEEPER) {
                wakeup_thread(g_wraith_slots[i].tid, false);
            }
        }
        kthread_sleep(1000);
    }
    // 计划截停：冻结整机，交给外部栈检查工具（Tools/wraith/）。
    wraith_freeze("planned");
    for (;;) kthread_sleep(1000000);
}
}  // namespace

void kthread_test_main() {
    kthread_creating_package pkg = {};
    pkg.func_raw   = (uint64_t)wraith_root;
    pkg.launch_pid = fast_get_processor_id();
    KURD_t k = KURD_t();
    uint64_t tid = creat_kthread(&pkg, &k);
    WRAITH_LOG("WT main: root tid=%llu kurd=%llx\n",
               (unsigned long long)tid, (unsigned long long)kurd_get_raw(k));
}

// 截停闸门：串口 #TB# → 环留证 → outb(0x80,0xDB) → cli;hlt。无 QEMU 补丁时也停在原地。
void wraith_freeze(const char* reason) {
    uart_marker("#TB#");
    WRAITH_LOG("TB reason=%s pid=%u rsp=%llx\n", reason,
               (unsigned)fast_get_processor_id(),
               (unsigned long long)wraith::rsp_now());
    io_outb(0x80, 0xDB);
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

#endif  // KTHREAD_TEST_SCENARIO
