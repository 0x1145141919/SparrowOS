#pragma once
#include <stdint.h>
#include "Scheduler/scheduler_types.h"
#include "arch/x86_64/abi/pt_regs.h"
struct kthread_creating_package;
class task;
struct blocked_tasks_clamps_t;
class block_queue;

struct kthread_creating_package {
    uint64_t func_raw;
    uint64_t args[5];
    uint32_t launch_pid;
};

extern "C" {
    ckurd kthread_init(task* t, kthread_creating_package* p);
    KURD_t task_launch(task* t, uint32_t pid);
    uint64_t creat_kthread(kthread_creating_package* p, KURD_t* kurd);
    void kthread_yield();
    uint64_t* get_scheduler_private_stack_top();
    void kthread_exit(uint64_t will);
    void kthread_self_blocked(task_blocked_reason_t reason);
    void kthread_sleep(miusecond_time_stamp_t offset);
    ckurd wakeup_thread(uint64_t tid, bool front_insert = false);
    uint64_t block_if_equal(bq_id_t qid, uint64_t* checker, uint64_t block_token);
    uint64_t zombie_observe(uint64_t tid, zombie_observe_results_t* result);
    ckurd release_kthread(uint64_t tid);
    bq_id_t bq_alloc(block_queue* q);
    ckurd bq_free(bq_id_t qid);
    void bq_flush_pending(blocked_tasks_clamps_t* clamp, bool is_timeout);
    void common_idle();
    extern char allkthread_true_enter;
    [[noreturn]] void resched(x64_standard_context_v2* frame);
}
