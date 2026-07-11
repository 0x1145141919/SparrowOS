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
#include "Scheduler/bq_system.h"
#include "util/rb_map.h"

rb_map<bq_id_t,block_queue*>*container;
spinrwlock_cpp_t container_lock;
uint64_t next_will_alloc_qid;

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
        t->on_queue_bit = true;
    }*/ //错误的写法,这些状态改变，应该在一个临界区，也就是外部的
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
    namespace ev = Scheduler::BLOCK_QUEUE_SYSTEM_EVENTS;
    KURD_t success = bq_success(ev::EVENT_CODE_BQ_FREE);
    KURD_t fail    = bq_fail(ev::EVENT_CODE_BQ_FREE);

    interrupt_guard g;
    spinrwlock_interrupt_about_write_guard l(container_lock);
    class block_queue**queue = container->find(qid);
    if (queue == nullptr) {
        fail.reason = ev::bq_free_results::FAIL_REASONS::QUEUE_NOT_FOUND;
        return kurd_get_raw(fail);
    }
    bool res = container->remove(qid);
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

// ── BQ 超时扫描线程 ──────────────────────────────────────────
// 每隔 ~5s 遍历所有 BQ，弹走超时任务
void* bq_timeout_sweeper(void*)
{
    while (true) {
        kthread_sleep(5 * 1000 * 1000);  // 5s
        // ── 第一遍：对在线 BQ 计数 ──
        size_t count = 0;
        {
            interrupt_guard gi;
            spinrwlock_interrupt_about_read_guard lc(container_lock);
            for (auto it = container->begin(); it != container->end(); ++it)
                count++;
        }
        if (count == 0) continue;

        // ── 分配快照数组 ──
        bq_id_t* ids = new bq_id_t[count];

        // ── 第二遍：填充 ──
        size_t filled = 0;
        {
            interrupt_guard gi;
            spinrwlock_interrupt_about_read_guard lc(container_lock);
            for (auto it = container->begin(); it != container->end() && filled < count; ++it)
                ids[filled++] = (*it).key;
        }

        // ── 逐个处理（每次拿容器锁查 ID） ──
        blocked_tasks_clamps_t clamps;
        for (size_t i = 0; i < filled; ++i) {
            block_queue* q;
            {
                interrupt_guard gi;
                spinrwlock_interrupt_about_read_guard lc(container_lock);
                block_queue** p = container->find(ids[i]);
                if (!p) continue;  // 已被 bq_free
                q = *p;
            }
            {
                interrupt_guard gi;
                spinlock_interrupt_about_guard gq(q->qlock);
                while(true)
                {q->pop_timeouts(&clamps);
                if(clamps.batch_count==0)
                    break;
                bq_flush_pending(&clamps, true);
                if(clamps.is_timeout_mov_early||clamps.is_queue_empty)
                    break;
                ksetmem_8(&clamps, 0, sizeof(clamps));
                }
                
            }
        }

        delete[] ids;
    }
    return nullptr;
}
