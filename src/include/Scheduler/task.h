#pragma once
#include "Scheduler/scheduler_types.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "arch/x86_64/abi/base.h"
#include "util/lock.h"
#include "memory/AddresSpace.h"

class task_pool;

struct u_ctx_t {
    x64_standard_context_v2 xtd_ctx;
    AddressSpace* as;
    uint32_t xcr0_mask;
    uint64_t cr4_mask;
    uint64_t drs[8];
    vaddr_t gs_base;
    vaddr_t fs_base;
    void* xsave_area;
    uint64_t xsave_size;
};

class task {
public:
    enum event_type_t {
        init,
        run_kthread,
        run_uthread,
        run_vCPU,
        offline,
        sleep,
        wait_io,
        wait_other,
        wait_mutex,
        event_type_COUNT
    };

    bool on_blockers_queue_bit = false;
    // [FIX-F4 / MS-1/4] 延迟唤醒位：唤醒者发现本任务「仍登记在某个核的 g_cpu_running[]」
    // （= 还没真正切离自己那片内核栈）时置位，绝不 set_ready/异核入队；
    // 由拥有核在 sched() 交接点补投（见 per_processor_scheduler::sched）。
    volatile bool wake_pending = false;
    // 延迟唤醒时随行携带的 priv_ctx.rax 编码（block_if_equal 的返回，bit0=已阻塞唤醒 /
    // bit1=超时），确保补投时语义与原即时唤醒一致。
    volatile uint64_t wake_pending_rax = 0;
    uint32_t belonged_processor_id;
    spinlock_cpp_t task_lock;
    miusecond_time_stamp_t min_wakeup_stamp;
    x64_standard_context_v2 priv_ctx;
    vaddr_t priv_stack_base;
    uint32_t priv_stack_pages;
    u_ctx_t* uctx;

    enum ctx_choose { priv, u_ctx, vCPU };
    ctx_choose choose;

    task();
    bool usage_of_search_set_tid(uint64_t new_tid);
    uint64_t get_tid() const;
    static task* basic_constructor();
    static void idle_specified_constructor(task* task_ptr);
    void atomic_load();
    // [FIX-F2 / MS-6] 与 atomic_load() 同 choose 分支，但用【调用方在锁内快照好的】上下文
    // 落地：避免在 task_lock 之外读 priv_ctx 与并发的 kthread_common_save 整结构体赋值竞态。
    void atomic_load_from(x64_standard_context_v2* ctx);
    bool set_ready();
    bool set_blocked();
    bool set_dead();
    bool set_zombie();
    bool set_running();
    bool resurrect();
    void task_event_shift(event_type_t new_event);
    task_state_t get_state();

private:
    event_type_t current_event;
    task_state_t task_state;
    uint64_t tid;
    miusecond_time_stamp_t current_event_start_stamp;
    miusecond_time_stamp_t accumulates_time_bank[event_type_COUNT];
    uint64_t accumulates_counters_bank[event_type_COUNT];

    friend class task_pool;
};
