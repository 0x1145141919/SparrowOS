/* ═══════════════════════════════════════════════════════════════════════
 * BQ (Block Queue) 系统实现
 *
 * 全局 BQ 池 + 句柄分配/释放 + get_lock/wake 原语 + block_if_equal 阻塞入口
 *
 * 锁纪律：bq_lock > task_lock > sched_lock
 *
 * location: block_queue_system (Scheduler::block_queue_system)
 * ═══════════════════════════════════════════════════════════════════════ */

#include "Scheduler/kthread_abi.h"
#include "Scheduler/per_processor_scheduler.h"
#include "Scheduler/bq_system.h"
#include "util/rb_map.h"
#include "panic.h"

rb_map<bq_id_t,block_queue*> container;
spinrwlock_cpp_t container_lock;
uint64_t next_will_alloc_qid;

void bq_system_init()
{
    new (&container) rb_map<bq_id_t, block_queue*>();
    new (&container_lock) spinrwlock_cpp_t();
    next_will_alloc_qid = 0;
}

// ── block_queue_system location 模板 helpers ──
namespace {
static inline KURD_t bq_default_kurd()
{
    return KURD_t(0, 0, module_code::SCHEDULER,
                  Scheduler::BLOCK_QUEUE_SYSTEM, 0, 0,
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
    namespace ev = Scheduler::BLOCK_QUEUE_SYSTEM_EVENTS;
    KURD_t success = bq_success(ev::EVENT_CODE_PUSH_TAIL);
    KURD_t fail    = bq_fail(ev::EVENT_CODE_PUSH_TAIL);

    if (t == nullptr) {
        fail.reason = ev::COMMON_FAIL_REASONS::NULL_PARAM;
        return fail;
    }
    if (t->get_state() != blocked) {
        fail.reason = ev::COMMON_FAIL_REASONS::INVALID_STATE;
        return fail;
    }/*
    {
        reentrant_spinlock_guard g(t->task_lock);
        t->task_event_shift(this->queue_event);
        t->on_blockers_queue_bit = true;
    }*/ //错误的写法,这些状态改变，应该在一个临界区，也就是外部的
    inner_queue.push_back(t);
    return success;
}
bq_id_t bq_alloc(block_queue*q)
{
    interrupt_guard g;
    spinrwlock_interrupt_about_write_guard l(container_lock);
    {
        bool res= container.insert(next_will_alloc_qid,q);
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
    namespace ev = Scheduler::BLOCK_QUEUE_SYSTEM_EVENTS;
    KURD_t success = bq_success(ev::EVENT_CODE_BQ_FREE);
    KURD_t fail    = bq_fail(ev::EVENT_CODE_BQ_FREE);

    interrupt_guard g;
    spinrwlock_interrupt_about_write_guard l(container_lock);
    class block_queue**queue = container.find(qid);
    if (queue == nullptr) {
        fail.reason = ev::bq_free_results::FAIL_REASONS::QUEUE_NOT_FOUND;
        return kurd_get_raw(fail);
    }
    bool res = container.remove(qid);
    if (res) {
        return kurd_get_raw(success);
    } else {
        fail.reason = ev::bq_free_results::FAIL_REASONS::REMOVE_FAIL;
        return kurd_get_raw(fail);
    }
}

// ── block_queue 成员函数 ──────────────────────────────────────

KURD_t block_queue::enable_queue(task::event_type_t type)
{
    namespace ev = Scheduler::BLOCK_QUEUE_SYSTEM_EVENTS;
    KURD_t success = bq_success(ev::EVENT_CODE_ENABLE_QUEUE);
    KURD_t fail    = bq_fail(ev::EVENT_CODE_ENABLE_QUEUE);

    if (state != ready) {
        fail.reason = ev::COMMON_FAIL_REASONS::INVALID_STATE;
        return fail;
    }
    if (inner_queue.size() != 0) {
        fail.reason = ev::COMMON_FAIL_REASONS::QUEUE_NOT_EMPTY;
        return fail;
    }
    state = running;
    queue_event = type;
    return success;
}

KURD_t block_queue::disable_queue()
{
    namespace ev = Scheduler::BLOCK_QUEUE_SYSTEM_EVENTS;
    KURD_t success = bq_success(ev::EVENT_CODE_DISABLE_QUEUE);
    KURD_t fail    = bq_fail(ev::EVENT_CODE_DISABLE_QUEUE);

    if (state != running) {
        fail.reason = ev::COMMON_FAIL_REASONS::INVALID_STATE;
        return fail;
    }
    if (inner_queue.size() != 0) {
        fail.reason = ev::COMMON_FAIL_REASONS::QUEUE_NOT_EMPTY;
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
        batch->arr[batch->batch_count++] = popped;
        if(batch->batch_count>=64)break;
    }
    batch->is_queue_empty = inner_queue.empty();
}

void block_queue::pop_all(blocked_tasks_clamps_t* batch)
{
    while (!inner_queue.empty()) {
        task* t = inner_queue.pop_front_value();
        batch->arr[batch->batch_count++] = t;
        if(batch->batch_count>=64){
            break;
        }
    }
    batch->is_queue_empty = inner_queue.empty();
}
task::event_type_t block_queue::get_queue_event()
{
    return this->queue_event;
}

void bq_flush_pending(blocked_tasks_clamps_t *clamp, bool is_timeout)
{
    if (!clamp || clamp->batch_count == 0) return;

    uint8_t rax_enc = 0b01 | (is_timeout ? 0b10 : 0b00);

    for (uint32_t i = 0; i < clamp->batch_count; ++i) {
        task* t = clamp->arr[i];
        if (!t) continue;
        reentrant_spinlock_guard gt(t->task_lock);
        t->priv_ctx.rax = rax_enc;
        t->set_ready();
        t->on_blockers_queue_bit = false;
        t->task_event_shift(task::event_type_t::offline);
    }

    for (uint32_t i = 0; i < clamp->batch_count; ++i) {
        task* t = clamp->arr[i];
        if (!t) continue;
        per_processor_scheduler* target = get_other_scheduler(t->belonged_processor_id);
        reentrant_spinlock_guard gs(target->sched_lock);
        KURD_t kurd = target->insert_ready_task(t, false);
        if (error_kurd(kurd)) {
            panic_info_inshort inshort = {
                .is_bug = true, .is_policy = true,
                .is_hw_fault = false, .is_mem_corruption = false,
                .is_escalated = false
            };
            Panic::panic(default_panic_behaviors_flags,
                nullptr, nullptr, &inshort, kurd);
        }
    }
}

// ── BQ 超时扫描线程 ──────────────────────────────────────────
// 每隔 ~5s 遍历所有 BQ，弹走超时任务
void* bq_timeout_sweeper(void*)
{
    blocked_tasks_clamps_t clamps;
    while (true) {
        {
            interrupt_guard gi;
            spinrwlock_interrupt_about_read_guard lc(container_lock);
            for (auto it = container.begin(); it != container.end(); ++it) {
                block_queue* q = (*it).value;
                spinlock_interrupt_about_guard gq(q->qlock);
                while (true) {
                    ksetmem_8(&clamps, 0, sizeof(clamps));
                    q->pop_timeouts(&clamps);
                    if (clamps.batch_count == 0) break;
                    bq_flush_pending(&clamps, true);
                    if (clamps.is_timeout_mov_early || clamps.is_queue_empty) break;
                }
            }
        }
        kthread_sleep(5 * 1000 * 1000);  // 5s
    }
    return nullptr;
}
blocked_tasks_clamps_t::blocked_tasks_clamps_t()
{
    ksetmem_8(this,0,sizeof(blocked_tasks_clamps_t));
}
