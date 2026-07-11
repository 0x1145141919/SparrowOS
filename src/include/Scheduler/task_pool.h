#pragma once
#include "Scheduler/task.h"
#include "util/Ktemplats.h"
#include "util/lock.h"

static int task_tid_compare(const task& a, const task& b) {
    return a.get_tid() - b.get_tid();
}

class task_pool {
    static spinrwlock_cpp_t lock;
    static Ktemplats::RBTree<task, task_tid_compare> m_tree;
public:
    static task* get_by_tid(uint64_t tid, KURD_t& kurd);
    static task* spawn();
    static KURD_t release(uint64_t tid);
    static int Init();
};
