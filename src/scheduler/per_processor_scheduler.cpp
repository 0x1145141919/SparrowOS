#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "Scheduler/per_processor_scheduler.h"
#include "Scheduler/kthread_abi.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kout.h"
#include "panic.h"

extern "C" void secure_hlt();
static void* secure_hlt_wrapper(void* unused) {
    (void)unused;
    secure_hlt();
    return nullptr;
}

// ── sleep_queue_t::insert ──
KURD_t per_processor_scheduler::sleep_queue_t::insert(task* task_ptr)
{
    if (task_ptr == nullptr) return KURD_t();

    node* n = alloc_node(task_ptr);

    if (!m_head) {
        m_head = m_tail = n;
        ++m_size;
        return KURD_t();
    }

    const miusecond_time_stamp_t new_stamp = task_ptr->min_wakeup_stamp;
    node* cur = m_head;
    while (cur) {
        task* cur_task = cur->value;
        if (cur_task && cur_task->min_wakeup_stamp > new_stamp) {
            break;
        }
        cur = cur->next;
    }

    if (!cur) {
        n->prev = m_tail;
        m_tail->next = n;
        m_tail = n;
        ++m_size;
        return KURD_t();
    }

    if (cur == m_head) {
        n->next = m_head;
        m_head->prev = n;
        m_head = n;
        ++m_size;
        return KURD_t();
    }

    n->next = cur;
    n->prev = cur->prev;
    cur->prev->next = n;
    cur->prev = n;
    ++m_size;
    return KURD_t();
}
namespace {
constexpr uint64_t kthread_yield_saved_stack_delta = 16 * sizeof(uint64_t);
constexpr uint32_t invalid_task_id = ~0u;

static inline KURD_t scheduler_default_kurd()
{
    return KURD_t(0, 0, module_code::SCHEDULER, Scheduler::SCHEDULER, 0, 0, err_domain::CORE_MODULE);
}

static inline void panic_with_kurd(x64_standard_context_v2 *frame, KURD_t kurd)
{
    panic_info_inshort inshort{
        .is_bug = true,
        .is_policy = true,
        .is_hw_fault = false,
        .is_mem_corruption = false,
        .is_escalated = false
    };
    panic_context::x64_context panic_ctx;
    panic_frame(frame, &panic_ctx);
    Panic::panic(default_panic_behaviors_flags,
        nullptr,
        &panic_ctx,
        &inshort,
        kurd
    );
}

static inline void panic_with_kurd(KURD_t kurd)
{
    panic_info_inshort inshort{
        .is_bug = true,
        .is_policy = true,
        .is_hw_fault = false,
        .is_mem_corruption = false,
        .is_escalated = false
    };
    Panic::panic(default_panic_behaviors_flags,
        nullptr,
        nullptr,
        &inshort,
        kurd
    );
}

} // namespace


KURD_t per_processor_scheduler::default_kurd()
{
    return KURD_t(0,0,module_code::SCHEDULER,Scheduler::SCHEDULER,0,0,err_domain::CORE_MODULE);
}

KURD_t per_processor_scheduler::default_success()
{
    KURD_t kurd = default_kurd();
    kurd.result = result_code::SUCCESS;
    kurd.level = level_code::INFO;
    return kurd;
}

KURD_t per_processor_scheduler::default_fail()
{
    return set_result_fail_and_error_level(default_kurd());
}

KURD_t per_processor_scheduler::default_fatal()
{
    return set_fatal_result_level(default_kurd());
}
void per_processor_scheduler::sleep_tasks_wake()
{
    constexpr  uint8_t arr_len_max = 16;
    int arr_len = 0;
    task* arr[arr_len_max];
    ksetmem_8(arr,0,arr_len_max);
    Ktemplats::list_doubly<task*> to_run_list;
    KURD_t kurd;
    miusecond_time_stamp_t current_stamp=ktime::get_microsecond_stamp();
    {
        reentrant_spinlock_guard g(this->sched_lock);
        while(true)
        {
        task**candidate=this->sleep_queue.front();
        if(candidate==nullptr){
            break;
        }
        task*candidate_task=*candidate;
        if(candidate_task->min_wakeup_stamp<=current_stamp){
            this->sleep_queue.pop_front();
            to_run_list.push_back(candidate_task);
        }else{
            break;
        }
        }
    }
    if(to_run_list.empty())return;
    for(auto it=to_run_list.begin();it!=to_run_list.end();++it){
        task*candidate=*it;
        {
            reentrant_spinlock_guard g(candidate->task_lock);
            candidate->on_queue_bit = false;
            candidate->set_ready();
        }
    }
    {
        reentrant_spinlock_guard g(this->sched_lock);
        while(!to_run_list.empty()){
            task*candidate=to_run_list.pop_front_value();
            kurd=this->insert_ready_task(candidate);
            if(error_kurd(kurd)){
                panic_with_kurd(kurd);
            }
        }
    }
}
void per_processor_scheduler::sched()
{
    task* to_run=[&]()->task*{
        {
            reentrant_spinlock_guard g(this->sched_lock);
            if(this->ready_queue.size()){
                task**candidate=this->ready_queue.front();
                if(*candidate){
                    this->ready_queue.pop_front();
                    return *candidate;
                }
            }
        }
        for(uint64_t i=0;i<logical_processor_count;i++){
            per_processor_scheduler*other=get_other_scheduler(i);
            if(other==this)continue;
            {
            reentrant_spinlock_guard g(other->sched_lock);
                if(other->ready_queue.size()){
                    task**candidate=other->ready_queue.front();
                    if(*candidate){
                    other->ready_queue.pop_front();
                    return *candidate;
                    }
                }
            }
        }
        return &this->idle;
    }();
    {
    reentrant_spinlock_guard(to_run->task_lock);
    to_run->set_running();
    to_run->belonged_processor_id=fast_get_processor_id();
    gs_u64_write(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX,(uint64_t)to_run);
    switch(to_run->choose){
        case task::ctx_choose::priv :
        to_run->task_event_shift(task::event_type_t::run_kthread);
        break;
        case task::ctx_choose::u_ctx :
        to_run->task_event_shift(task::event_type_t::run_uthread   );
        break;
        case task::ctx_choose::vCPU :
        to_run->task_event_shift(task::event_type_t::run_vCPU);
        break;
        default:
        break;
    }
    }
    ktime::heart_beat_alarm::set_clock_by_offset(20000);
    to_run->atomic_load();
}
KURD_t per_processor_scheduler::insert_ready_task(task *task_ptr, bool front)
{
    namespace ev = Scheduler::SCHEDULER_EVENTS;
    KURD_t fail=default_fail();
    KURD_t success=default_success();
    fail.event_code=ev::EVENT_CODE_INSERT_READY_TASK;
    success.event_code=ev::EVENT_CODE_INSERT_READY_TASK;
    if(task_ptr==&idle){
        return success;
    }
    if(task_ptr==nullptr){
        fail.reason=ev::COMMON_FAIL_REASONS::NULL_TASK_PTR;
        return fail;
    }
    if(task_ptr->get_state()!=ready){
        fail.reason=ev::COMMON_FAIL_REASONS::BAD_TASK_TYPE;
        return fail;
    }
    if(front){
        if(!ready_queue.push_front(task_ptr)){
            fail.reason=ev::COMMON_FAIL_REASONS::INSERT_FAIL;
            return fail;
        }
    }else{
        if(!ready_queue.push_back(task_ptr)){
            fail.reason=ev::COMMON_FAIL_REASONS::INSERT_FAIL;
            return fail;
        }
    }
    return success;
}
void per_processor_scheduler::placed_init(per_processor_hardware_stack_t* stacks_ptr)
{
    task& t=this->idle;
    task::idle_specified_constructor(&t);
    t.priv_ctx.core_ctx.idtctx.iret.rip=(uint64_t)&common_idle;
    t.priv_ctx.core_ctx.idtctx.iret.cs=K_cs_idx<<3;
    t.priv_ctx.core_ctx.idtctx.iret.ss=K_ds_ss_idx<<3;
    t.priv_stack_base=(vaddr_t)stacks_ptr->stack_idle_task;
    t.priv_stack_pages=sizeof(((per_processor_hardware_stack_t*)0)->stack_idle_task)>>12;
    t.priv_ctx.core_ctx.idtctx.iret.rsp=t.priv_stack_base+(t.priv_stack_pages<<12)-64;
    t.priv_ctx.core_ctx.idtctx.iret.rflags=INIT_DEFAULT_RFLAGS;
    t.choose=task::ctx_choose::priv;
}
per_processor_scheduler* global_schedulers = nullptr;

per_processor_scheduler *get_self_scheduler()
{
    return (per_processor_scheduler*)read_gs_u64(PROCESSOR_SCHEDULER_GS_INDEX);
}


void per_processor_scheduler::next_task_with_routine()
{
    // 睡眠队列超时唤醒
    sleep_tasks_wake();

    // 调度
    sched();
}
per_processor_scheduler *get_other_scheduler(uint32_t pid)
{
    return &global_schedulers[pid];
}
bool per_processor_scheduler::is_the_idle_task(task *t)
{
    return t==(&this->idle);
}