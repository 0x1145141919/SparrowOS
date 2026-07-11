#pragma once
#include "Scheduler/task.h"
#include "util/Ktemplats.h"
#include "util/lock.h"

struct blocked_tasks_clamps_t {
    uint8_t batch_count;
    bool is_queue_empty;
    bool is_timeout_mov_early;
    task* arr[64];
};

class block_queue {
    enum state_t { ready, running } state = ready;
    task::event_type_t queue_event;
    Ktemplats::list_doubly<task*> inner_queue;
public:
    spinlock_cpp_t qlock;
    block_queue(): state(ready), qlock{}, queue_event{}, inner_queue{} {}
    KURD_t push_tail(task* t);
    KURD_t enable_queue(task::event_type_t type);
    KURD_t disable_queue();
    task::event_type_t get_queue_event();
    bool is_queue_ready();
    task* pop_head();
    void pop_timeouts(blocked_tasks_clamps_t* batch);
    void pop_all(blocked_tasks_clamps_t* batch);
};
