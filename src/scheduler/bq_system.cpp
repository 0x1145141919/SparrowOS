/* ═══════════════════════════════════════════════════════════════════════
 * BQ (Block Queue) 系统实现
 *
 * 全局 BQ 池 + 句柄分配/释放 + get_lock/wake 原语 + block_if_equal 阻塞入口
 *
 * 锁纪律：bq_lock > task_lock > sched_lock
 *
 * location: block_queue_system (Scheduler::block_queue_system)
 * ═══════════════════════════════════════════════════════════════════════ */

#include "Scheduler/per_processor_scheduler.h"
#include "util/rb_map.h"

rb_map<bq_id_t,block_queue*>*container;
spinrwlock_cpp_t container_lock;
uint64_t next_will_alloc_qid;

// ── block_queue_system location 模板 helpers ──
namespace {
static inline KURD_t bq_default_kurd()
{
    return KURD_t(0, 0, module_code::SCHEDULER,
                  Scheduler::block_queue_system, 0, 0,
                  err_domain::CORE_MODULE);
}
static inline KURD_t bq_success(uint8_t event)
{
    KURD_t k = bq_default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    k.event_code = event;
    return k;
}
static inline KURD_t bq_fail(uint8_t event)
{
    KURD_t k = bq_default_kurd();
    k.result = result_code::FAIL;
    k.level  = level_code::ERROR;
    k.event_code = event;
    return k;
}
} // namespace

KURD_t block_queue::push_tail(task *t)
{
    using namespace Scheduler::block_queue_system_events;
    KURD_t success = bq_success(push_tail);
    KURD_t fail    = bq_fail(push_tail);

    if (t == nullptr) {
        fail.reason = common_fail_reasons::null_param;
        return fail;
    }
    if (t->get_state() != blocked) {
        fail.reason = common_fail_reasons::invalid_state;
        return fail;
    }
    {
        reentrant_spinlock_guard g(t->task_lock);
        t->task_event_shift(this->queue_event);
    }
    t->out_of_task_lock_is_task_on_block_queue_bit = true;
    inner_queue.push_back(t);
    return success;
}
bq_id_t bq_alloc(block_queue*q)
{
    interrupt_guard g;
    spinrwlock_interrupt_about_write_guard l(container_lock);
    {
        bool res= container->insert(next_will_alloc_qid,q);
        if(res){
            bq_id_t id_get= next_will_alloc_qid;
            ++next_will_alloc_qid;
            return id_get;
        }else{
            return 0;
        }
    }
}
ckurd bq_free(bq_id_t qid)
{
    using namespace Scheduler::block_queue_system_events;
    KURD_t success = bq_success(bq_free);
    KURD_t fail    = bq_fail(bq_free);

    interrupt_guard g;
    spinrwlock_interrupt_about_write_guard l(container_lock);
    class block_queue**queue = container->find(qid);
    if (queue == nullptr) {
        fail.reason = bq_free_results::fail_reasons::queue_not_found;
        return kurd_get_raw(fail);
    }
    bool res = container->remove(qid);
    if (res) {
        return kurd_get_raw(success);
    } else {
        fail.reason = bq_free_results::fail_reasons::remove_fail;
        return kurd_get_raw(fail);
    }
}

// ── block_queue 成员函数 ──────────────────────────────────────

KURD_t block_queue::enable_queue(task::event_type_t type)
{
    using namespace Scheduler::block_queue_system_events;
    KURD_t success = bq_success(enable_queue);
    KURD_t fail    = bq_fail(enable_queue);

    if (state != ready) {
        fail.reason = common_fail_reasons::invalid_state;
        return fail;
    }
    if (inner_queue.size() != 0) {
        fail.reason = common_fail_reasons::queue_not_empty;
        return fail;
    }
    state = running;
    queue_event = type;
    return success;
}

KURD_t block_queue::disable_queue()
{
    using namespace Scheduler::block_queue_system_events;
    KURD_t success = bq_success(disable_queue);
    KURD_t fail    = bq_fail(disable_queue);

    if (state != running) {
        fail.reason = common_fail_reasons::invalid_state;
        return fail;
    }
    if (inner_queue.size() != 0) {
        fail.reason = common_fail_reasons::queue_not_empty;
        return fail;
    }
    state = ready;
    return success;
}

bool block_queue::is_queue_ready()
{
    return state == ready && inner_queue.empty();
}

task* block_queue::pop_head()
{
    if (inner_queue.empty()) return nullptr;
    task* t = inner_queue.pop_front_value();
    t->out_of_task_lock_is_task_on_block_queue_bit = false;
    return t;
}

void block_queue::pop_timeouts(blocked_tasks_clamps_t* batch)
{
    uint64_t now = ktime::get_microsecond_stamp();
    while (!inner_queue.empty()) {
        task* front_t = *inner_queue.front();
        if (front_t->min_wakeup_stamp == 0 || now <= front_t->min_wakeup_stamp)
            {
                batch->is_timeout_mov_early = true;
                break;
            }
        task* popped = inner_queue.pop_front_value();
        popped->out_of_task_lock_is_task_on_block_queue_bit = false;
        batch->arr[batch->batch_count++] = popped;
        if(batch->batch_count>=64)break;
    }
    batch->is_queue_empty = inner_queue.empty();
}

void block_queue::pop_all(blocked_tasks_clamps_t* batch)
{
    while (!inner_queue.empty()) {
        task* t = inner_queue.pop_front_value();
        t->out_of_task_lock_is_task_on_block_queue_bit = false;
        batch->arr[batch->batch_count++] = t;
        if(batch->batch_count>=64){
            break;
        }
    }
    batch->is_queue_empty = inner_queue.empty();
}
