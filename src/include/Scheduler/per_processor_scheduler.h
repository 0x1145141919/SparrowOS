#pragma once
#include "Scheduler/task.h"
#include "Scheduler/task_pool.h"
#include "Scheduler/kthread_abi.h"
#include "util/Ktemplats.h"
#include "util/lock.h"

class task_pool;

class alignas(64) per_processor_scheduler {
private:
    KURD_t default_kurd();
    KURD_t default_success();
    KURD_t default_fail();
    KURD_t default_fatal();
    task idle;
    void sleep_tasks_wake();
    void sched();
public:
    static constexpr uint32_t GANTT_CAPACITY = 4096;
    dts_gantt_entry* dts_gantt = nullptr;
    uint32_t dts_gantt_head = 0;
    Ktemplats::list_doubly<task*> ready_queue;

    class sleep_queue_t : Ktemplats::list_doubly<task*> {
    public:
        using list_doubly<task*>::empty;
        using list_doubly<task*>::front;
        using list_doubly<task*>::back;
        using list_doubly<task*>::pop_front_value;
        using list_doubly<task*>::size;
        using list_doubly<task*>::pop_front;
        sleep_queue_t() = default;
        KURD_t insert(task* task_ptr);
    };
    sleep_queue_t sleep_queue;
    reentrant_spinlock_cpp_t sched_lock;
    bool is_idle;
    void next_task_with_routine();
    KURD_t insert_ready_task(task* task_ptr, bool front = false);
    KURD_t dts_gantt_enable();
    void dts_gantt_disable();
    void dts_gantt_write(task* to_run, uint8_t reason, uint8_t io_urgency);
    friend task;
    friend class task_pool;
    bool is_the_idle_task(task* t);
    void placed_init();
};

extern per_processor_scheduler* global_schedulers;
per_processor_scheduler* get_self_scheduler();
per_processor_scheduler* get_other_scheduler(uint32_t pid);
