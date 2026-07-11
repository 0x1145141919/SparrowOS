#include "Scheduler/task_pool.h"
#include "Scheduler/per_processor_scheduler.h"
#include "abi/os_error_definitions.h"

u64ka g_next_tid{0};

spinrwlock_cpp_t task_pool::lock;
Ktemplats::RBTree<task, task_tid_compare> task_pool::m_tree;

int task_pool::Init()
{
    return OS_SUCCESS;
}

task* task_pool::get_by_tid(uint64_t tid, KURD_t& kurd)
{
    task tmp;
    tmp.tid = tid;
    task* found;
    {
        spinrwlock_interrupt_about_read_guard l(lock);
        found = m_tree.find(tmp);
    }
    if (found) return found;
    kurd = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                  Scheduler::TASK_POOL,
                  Scheduler::TASK_POOL_EVENTS::EVENT_CODE_GET_BY_TID,
                  level_code::ERROR, err_domain::CORE_MODULE);
    kurd.reason = Scheduler::TASK_POOL_EVENTS::COMMON_FAIL_REASONS::NOT_FOUND;
    return nullptr;
}

task* task_pool::spawn()
{
    task tmp;
    tmp.tid = g_next_tid.add_ka(1);
    m_tree.insert(tmp);
    return m_tree.find(tmp);
}

KURD_t task_pool::release(uint64_t tid)
{
    spinrwlock_interrupt_about_write_guard l(lock);
    task tmp;
    tmp.tid = tid;
    task* found = m_tree.find(tmp);
    if (!found) {
        KURD_t k = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                          Scheduler::TASK_POOL,
                          Scheduler::TASK_POOL_EVENTS::EVENT_CODE_RELEASE,
                          level_code::ERROR, err_domain::CORE_MODULE);
        k.reason = Scheduler::TASK_POOL_EVENTS::COMMON_FAIL_REASONS::NOT_FOUND;
        return k;
    }
    m_tree.erase(tmp);
    return KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
                  Scheduler::TASK_POOL,
                  Scheduler::TASK_POOL_EVENTS::EVENT_CODE_RELEASE,
                  level_code::INFO, err_domain::CORE_MODULE);
}

extern "C" uint64_t zombie_observe(uint64_t tid, zombie_observe_results_t* result)
{
    KURD_t kurd;
    task* t = task_pool::get_by_tid(tid, kurd);
    if (error_kurd(kurd)) {
        *result = ZOMBIE_TID_NOT_FOUND;
        return INVALID_TID;
    }
    {
        reentrant_spinlock_guard l(t->task_lock);
        if (t->get_state() != task_state_t::zombie) {
            *result = ZOMBIE_ALIVE;
            return INVALID_TID;
        } else {
            *result = ZOMBIE_DEAD;
            return t->priv_ctx.rdi;
        }
    }
}
