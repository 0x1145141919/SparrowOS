/* ============================================================================
 * 调度器中断入口 — 执行流纪律
 *
 * 本文件的所有 kthread_*_cppenter / block_*_cppenter 函数都是中断入口。
 * 它们最终调用 scheduler.sched() 切换执行流，调用后永不返回（context switch
 * 通过 atomic_load 跳到新任务的 iretq 恢复点）。
 *
 * ── scheduler.sched() ──
 *   执行流飞走性调用。取下一个任务 → atomic_load → iretq 跳到新任务。
 *   调用前必须释放所有 RAII 锁守卫（spinlock_guard / reentrant_spinlock_guard），
 *   否则锁在析构前就飞走，永久泄漏。
 *
 * ── scheduler.sleep_tasks_wake() ──
 *   内部两次获取/释放 this->sched_lock：
 *     ① 遍历 sleep_queue 收集到期的任务到本地 list
 *     ② 遍历本地 list 调用 insert_ready_task
 *   两次获取之间有短暂的 sched_lock 释放窗口。
 *   调用者不可持有任何调度器锁。
 * ========================================================================== */

#include "Scheduler/per_processor_scheduler.h"
#include "panic.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kout.h"
#include "memory/FreePagesAllocator.h"
alignas(64) per_processor_scheduler global_schedulers[MAX_PROCESSORS_COUNT];
namespace {
constexpr uint64_t kthread_yield_saved_stack_delta = 16 * sizeof(uint64_t);
spinlock_cpp_t global_tid_lock;
uint64_t global_tid_counter = 0;


static inline KURD_t self_scheduler_default_kurd()
{
    return KURD_t(0, 0, module_code::SCHEDULER, Scheduler::self_scheduler, 0, 0, err_domain::CORE_MODULE);
}

static inline KURD_t make_self_scheduler_fatal(
    uint8_t event_code, 
    uint16_t reason
)
{
    KURD_t kurd = self_scheduler_default_kurd();
    kurd.event_code = event_code;
    kurd.reason = reason;
    return set_fatal_result_level(kurd);
}

static inline void panic_with_kurd(x64_standard_context *frame, KURD_t kurd,char*message=nullptr)
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
        message,
        &panic_ctx,
        &inshort,
        kurd
    );
}

static inline void panic_with_kurd(KURD_t kurd,char*message=nullptr)
{
    panic_info_inshort inshort{
        .is_bug = true,
        .is_policy = true,
        .is_hw_fault = false,
        .is_mem_corruption = false,
        .is_escalated = false
    };
    Panic::panic(default_panic_behaviors_flags,
        message,
        nullptr,
        &inshort,
        kurd
    );
}
} // namespace
task* kthread_common_save(x64_standard_context*frame,bool expect_running)
{
    task* task_ptr = (task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    if (!task_ptr) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: null running task");
    }
    if (!task_ptr->context.kthread) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        fatal.reason = Scheduler::self_scheduler_events::kthread_common_save_results::fatal_reasons::context_nullptr;
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: kthread context null");
    }
    if (expect_running && task_ptr->get_state() != task_state_t::running) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        fatal.reason = Scheduler::self_scheduler_events::kthread_common_save_results::fatal_reasons::bad_task_state;
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: not running");
    }
    if (task_ptr->get_task_type() != task_type_t::kthreadm) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        fatal.reason = Scheduler::self_scheduler_events::kthread_common_save_results::fatal_reasons::bad_task_type;
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: not kthread");
    }
    vaddr_t stack_bottom = task_ptr->context.kthread->stack_bottom;
    vaddr_t stack_top   = stack_bottom - task_ptr->context.kthread->stacksize;
    if (frame->iret_complex.rsp > stack_bottom || frame->iret_complex.rsp < stack_top) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        fatal.reason = Scheduler::self_scheduler_events::kthread_common_save_results::fatal_reasons::context_stackptr_out_of_range;
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: stack ptr OOR");
    }
    task_ptr->context.kthread->regs = *frame;
    uint64_t latest_run_span = ktime::get_microsecond_stamp() - task_ptr->lastest_run_stamp;
    task_ptr->accumulated_time += latest_run_span;
    return task_ptr;
}
extern "C" void allthread_true_enter(void *(*entry)(void *), void *arg){
    uint64_t return_value=(uint64_t)entry(arg);
    kthread_exit(return_value);
};
uint64_t create_kthread(void *(*entry)(void *), void *arg, KURD_t *out_kurd)
{
    interrupt_guard g;
    kthread_context* context = new kthread_context();
    context->regs.iret_complex.rip = (uint64_t)allthread_true_enter;
    context->regs.rsi = (uint64_t)arg;
    context->regs.rdi = (uint64_t)entry;
    context->regs.iret_complex.cs = K_cs_idx<<3;
    context->regs.iret_complex.ss = K_ds_ss_idx<<3;
    context->stacksize = DEFAULT_STACK_SIZE;
    context->stack_bottom = (uint64_t)stack_alloc(out_kurd,DEFAULT_STACK_PG_COUNT);
    if(error_kurd(*out_kurd)){
        delete context;
        return INVALID_TID;
    }
    context->regs.iret_complex.rsp = context->stack_bottom;
    context->regs.iret_complex.rflags = INIT_DEFAULT_RFLAGS;
    task* new_task = new task(task_type_t::kthreadm, context);
    new_task->task_lock.lock();
    uint64_t assigned_tid = task_pool::alloc(new_task, *out_kurd);
    if(error_kurd(*out_kurd)){
        new_task->task_lock.unlock();
        return INVALID_TID;
    }
    new_task->assign_valid_tid(assigned_tid);
    new_task->set_ready();
    per_processor_scheduler&self_scheduler = global_schedulers[fast_get_processor_id()];
    new_task->task_lock.unlock();
    self_scheduler.sched_lock.lock();
    *out_kurd=self_scheduler.insert_ready_task(new_task);
    if(error_kurd(*out_kurd)){
         delete context;
        delete new_task;
        self_scheduler.ready_queue.pop_back();
        self_scheduler.sched_lock.unlock();
        return INVALID_TID;
    }
    self_scheduler.sched_lock.unlock();
    return assigned_tid;
}
[[noreturn]] void kthread_yield_true_enter(x64_standard_context*context)
{

    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task* yield_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(yield_task->task_lock);
        kthread_common_save(context,true);
        yield_task->set_ready();
    }
    {
        reentrant_spinlock_guard g(scheduler.sched_lock);
        scheduler.insert_ready_task(yield_task);
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
extern "C" [[noreturn]] void resched(x64_standard_context_v2 *frame)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task* interrupted_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    bool is_user_context=((frame->core_ctx.idtctx.iret.cs&3)==3);
    {
        reentrant_spinlock_guard g(interrupted_task->task_lock);
        if(!is_user_context){
            kthread_common_save(frame,true);
        }
        interrupted_task->set_ready();
    }
    {
        reentrant_spinlock_guard g(scheduler.sched_lock);
        scheduler.insert_ready_task(interrupted_task);
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
[[noreturn]] void kthread_exit_cppenter(x64_standard_context*context) 
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task*exit_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(exit_task->task_lock);
        kthread_common_save(context,true);
        Ktemplats::list_doubly<uint64_t>&waiters_queue=exit_task->waiters;
        while(waiters_queue.size()>0){
            uint64_t waiter_tid=waiters_queue.pop_front_value();
            KURD_t wkurd;
            task*waiter_task=task_pool::get_by_tid(waiter_tid,wkurd);
            if(!waiter_task||error_kurd(wkurd)){
                KURD_t fatal=make_self_scheduler_fatal(
                    Scheduler::self_scheduler_events::kthread_exit,0);
                fatal.reason=Scheduler::self_scheduler_events::kthread_exit_results::fatal_reasons::bad_task_state;
                panic_with_kurd(context,fatal);
            }
            if(waiter_task->get_state()!=task_state_t::blocked||
               waiter_task->blocked_reason!=wait_other_kthread){
                KURD_t fatal=make_self_scheduler_fatal(
                    Scheduler::self_scheduler_events::kthread_exit,0);
                fatal.reason=Scheduler::self_scheduler_events::kthread_exit_results::fatal_reasons::waiter_bad_block_reason;
                panic_with_kurd(context,fatal);
            }
            {
                reentrant_spinlock_guard a(waiter_task->task_lock);
                waiter_task->set_ready();
                waiter_task->context.kthread->regs.rax=context->rdi;
                uint32_t waiter_belonged_processor_id=waiter_task->get_belonged_processor_id();
                per_processor_scheduler&target_scheduler=global_schedulers[waiter_belonged_processor_id];
                {
                    reentrant_spinlock_guard b(target_scheduler.sched_lock);
                    target_scheduler.insert_ready_task(waiter_task);
                }
            }
        }
        exit_task->set_zombie();
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
extern "C" uint64_t kthread_wait_truly_wait(uint64_t tid);
uint64_t kthread_wait(uint64_t tid)
{
    interrupt_guard g;
    KURD_t running_task_kurd=KURD_t();
    task*waited_task=task_pool::get_by_tid(tid,running_task_kurd);
    if(error_kurd(running_task_kurd)||!waited_task){
        return ~0ull;
    }
    waited_task->task_lock.lock();
    if(waited_task->get_state()==task_state_t::zombie){
        uint64_t ret=waited_task->context.kthread->regs.rdi;
        waited_task->task_lock.unlock();
        return ret;
    }else{
        waited_task->task_lock.unlock();
        return kthread_wait_truly_wait(tid);
    }
}
void kthread_wait_cppenter(x64_standard_context *context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    uint64_t waitede_tid=context->rdi;
    KURD_t wkurd;
    task*waited_task=task_pool::get_by_tid(waitede_tid,wkurd);
    if(error_kurd(wkurd)||!waited_task){
        panic_with_kurd(wkurd);
    }
    task* waiter_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(waited_task->task_lock);
        if(waited_task->get_state()==task_state_t::zombie){
            context->rax=waited_task->context.kthread->regs.rdi;
            return;
        }
        {
            reentrant_spinlock_guard h(waiter_task->task_lock);
            kthread_common_save(context,true);
            waiter_task->set_blocked();
            waiter_task->blocked_reason=task_blocked_reason_t::wait_other_kthread;
        }
        waited_task->waiters.push_back(waiter_task->get_tid());
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
[[noreturn]] void kthread_self_blocked_cppenter(x64_standard_context* context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(blocked_task->task_lock);
        kthread_common_save(context,true);
        blocked_task->blocked_reason=(task_blocked_reason_t)context->rdi;
        blocked_task->set_blocked();
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
uint64_t wakeup_thread(uint64_t tid, bool front_insert){
    interrupt_guard g;
    KURD_t kurd=KURD_t();
    KURD_t success,fail,fatal;
    success=KURD_t(result_code::SUCCESS,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::wake_up_kthread,level_code::INFO,err_domain::CORE_MODULE);
    fail=KURD_t(result_code::FAIL,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::wake_up_kthread,level_code::ERROR,err_domain::CORE_MODULE);
    fatal=KURD_t(result_code::FATAL,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::wake_up_kthread,level_code::FATAL,err_domain::CORE_MODULE);
    task*task_ptr=task_pool::get_by_tid(tid,kurd);
    if(!success_all_kurd(kurd)){
        return kurd_get_raw(kurd);
    }
    reentrant_spinlock_guard l(task_ptr->task_lock);
    per_processor_scheduler&target_scheduler=global_schedulers[task_ptr->get_belonged_processor_id()];
    if(task_ptr->get_state()==task_state_t::ready||
    task_ptr->get_state()==task_state_t::running){
        success.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::success_reasons::already_wakeup_or_running;
        return kurd_get_raw(success);
        //成功但是已经运行
    }else if(task_ptr->get_state()==task_state_t::blocked){
        if(task_ptr->blocked_reason==sleeping||task_ptr->blocked_reason==wait_other_kthread){
            fail.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::fail_reasons::kthread_cant_wake_for_bad_block_reason;
            return kurd_get_raw(fail);
        }
        task_ptr->set_ready();
        {
            reentrant_spinlock_guard h(target_scheduler.sched_lock);
            kurd=target_scheduler.insert_ready_task(task_ptr, front_insert);
            return kurd_get_raw(kurd);
        }
    }else{
        fail.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::fail_reasons::bad_task_state;
        return kurd_get_raw(fail);
    }
}
[[noreturn]] void kthread_sleep_cppenter(x64_standard_context*context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task* sleeper_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(sleeper_task->task_lock);
        kthread_common_save(context,true);
        sleeper_task->blocked_reason=sleeping;
        sleeper_task->sleep_wakeup_stamp=ktime::get_microsecond_stamp()+context->rdi;
        sleeper_task->set_blocked();
    }
    {
        reentrant_spinlock_guard h(scheduler.sched_lock);
        scheduler.sleep_queue.insert(sleeper_task);
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
[[noreturn]] void block_queue_cppenter(x64_standard_context *context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    tid_wait_queue*waite_queue=(tid_wait_queue*)context->rdi;
    {
        spinlock_interrupt_about_guard g(waite_queue->lock);
        {
            reentrant_spinlock_guard h(blocked_task->task_lock);
            kthread_common_save(context,true);
            blocked_task->set_blocked();
            waite_queue->push(blocked_task->get_tid());
        }
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
void block_if_equal_cppenter(x64_standard_context *context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    tid_wait_queue*waite_queue=(tid_wait_queue*)context->rdi;
    uint64_t*check_address=(uint64_t*)context->rsi;
    uint64_t block_token=context->rdx;
    bool should_block=false;
    {
        spinlock_interrupt_about_guard g(waite_queue->lock);
        if(*check_address==block_token)
        {
            reentrant_spinlock_guard h(blocked_task->task_lock);
            kthread_common_save(context,true);
            blocked_task->set_blocked();
            waite_queue->push(blocked_task->get_tid());
            should_block=true;
        }
    }
    if(should_block){
        scheduler.sleep_tasks_wake();
        scheduler.sched();
    }
}
uint64_t release_kthread(uint64_t tid)
{
    KURD_t kurd=KURD_t();
    task*task_ptr=task_pool::get_by_tid(tid,kurd);
    kurd=__wrapped_pgs_vfree((void*)(task_ptr->context.kthread->stack_bottom-task_ptr->context.kthread->stacksize)
    ,1+task_ptr->context.kthread->stacksize/4096);
    if(error_kurd(kurd)){
        return kurd_get_raw(kurd);
    }
    delete task_ptr->context.kthread;
    delete task_ptr;
    return kurd_get_raw(task_pool::release_tid(tid));
}
void kthread_call_cpp_enter(x64_standard_context *frame)
{
    switch(frame->rax){
        case kthread_call_num::exit:
        {
            kthread_exit_cppenter(frame);
            break;
        }
        case kthread_call_num::sleep:
        {
            kthread_sleep_cppenter(frame);
            break;
        }
        case kthread_call_num::yield:
        {
            kthread_yield_true_enter(frame);
            break;
        }
        case kthread_call_num::wait:
        {
            kthread_wait_cppenter(frame);
            break;
        }
        case kthread_call_num::block:
        {
            kthread_self_blocked_cppenter(frame);
            break;
        }
        case kthread_call_num::block_to_queue:
        {
            block_queue_cppenter(frame);
            break;
        }
        case kthread_call_num::block_to_queue_if_equal:
        {
            block_if_equal_cppenter(frame);
            break;
        }
        default:
        {
            //panic
        }
    }
}