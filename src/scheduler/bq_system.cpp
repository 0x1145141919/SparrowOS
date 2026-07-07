/* ═══════════════════════════════════════════════════════════════════════
 * BQ (Block Queue) 系统实现
 *
 * 全局 BQ 池 + 句柄分配/释放 + get_lock/wake 原语 + block_if_equal 阻塞入口
 *
 * 锁纪律：bq_lock > task_lock > sched_lock
 *
 * 当前阶段：空 KURD 占位。所有 ckurd 返回值为临时构造，未编排模块错误树。
 * ═══════════════════════════════════════════════════════════════════════ */

#include "Scheduler/per_processor_scheduler.h"
#include "util/rb_map.h"

rb_map<bq_id_t,block_queue*>*container;
spinrwlock_cpp_t container_lock;
uint64_t next_will_alloc_qid;
KURD_t block_queue::push_tail(task *t)
{
    if(t==nullptr)return KURD_t();
    if(t->get_state()!=blocked)return KURD_t();
    {
        reentrant_spinlock_guard(t->task_lock);
        t->task_event_shift(this->queue_event);
    }
    t->out_of_task_lock_is_task_on_block_queue_bit = true;
    inner_queue.push_back(t);
    return KURD_t();
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
    interrupt_guard g;
    spinrwlock_interrupt_about_write_guard l(container_lock);
    class block_queue**queue=container->find(qid);
    if(queue==nullptr)
    {return ckurd();}else{
        bool res=container->remove(qid);
        if(res)
        {return ckurd();}//成功kurd
        else{
            return ckurd();//失败kurd
        }
    }
}

// ── block_queue 成员函数 ──────────────────────────────────────

KURD_t block_queue::enable_queue(task::event_type_t type)
{
    if (state != ready) return KURD_t{};
    if (inner_queue.size() != 0) return KURD_t{};
    state = running;
    queue_event = type;
    return KURD_t{};
}

KURD_t block_queue::disable_queue()
{
    if (state != running) return KURD_t{};
    if (inner_queue.size() != 0) return KURD_t{};
    state = ready;
    return KURD_t{};
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
