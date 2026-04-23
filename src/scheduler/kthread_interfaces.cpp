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
KURD_t kthread_common_save(task*task_ptr,x64_standard_context*frame,bool expect_running){//默认外部有task_ptr的task_lock
    KURD_t fatal=make_self_scheduler_fatal(
        Scheduler::self_scheduler_events::kthread_common_save,0
    );
    using namespace Scheduler::self_scheduler_events::kthread_common_save_results;
    KURD_t fail=fatal;
    fail=set_result_fail_and_error_level(fail);
    if(task_ptr==nullptr||frame==nullptr){
        fail.reason=fail_reasons::nullptr_param;
        return fail;
    }
    if(task_ptr->context.kthread==nullptr){
        fatal.reason=fatal_reasons::context_nullptr;
        return fatal;
    }
    if(expect_running)
        if(task_ptr->get_state()!=task_state_t::running){
            fatal.reason=fatal_reasons::bad_task_state;
            return fatal;
        }
    if(task_ptr->get_task_type()!=task_type_t::kthreadm){
        fatal.reason=fatal_reasons::bad_task_type;  
        return fatal;
    }
    vaddr_t stack_bottom=task_ptr->context.kthread->stack_bottom;
    vaddr_t stack_top=stack_bottom-task_ptr->context.kthread->stacksize;
    if(frame->iret_complex.rsp<=stack_bottom&&frame->iret_complex.rsp>=stack_top){
        task_ptr->context.kthread->regs=*frame;
        uint64_t latest_run_span=ktime::get_microsecond_stamp()-task_ptr->lastest_run_stamp;
        task_ptr->accumulated_time+=latest_run_span;
        return KURD_t();
    }else{
        fatal.reason=fatal_reasons::context_stackptr_out_of_range;
        return fatal;
    }
}
void allthread_true_enter(void *(*entry)(void *), void *arg){
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
    context->regs.iret_complex.cs = x64_local_processor::K_cs_idx<<3;
    context->regs.iret_complex.ss = x64_local_processor::K_ds_ss_idx<<3;
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
void kthread_yield_true_enter(x64_standard_context*context)
{

    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* yield_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    if(error_kurd(running_task_kurd)||yield_task==nullptr){
        panic_with_kurd(context,running_task_kurd);
    }
    {
        reentrant_spinlock_guard g(yield_task->task_lock);
        running_task_kurd=kthread_common_save(yield_task,context,true);
        if(error_kurd(running_task_kurd)){
            goto fatals;
        }
        yield_task->set_ready();
    }
    {
        reentrant_spinlock_guard g(scheduler.sched_lock);
        running_task_kurd=scheduler.insert_ready_task(yield_task);
        if(error_kurd(running_task_kurd)){
            goto fatals;    
        }
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(context, running_task_kurd);
}
void timer_cpp_enter(x64_standard_context *frame)
{
    KURD_t kurd=KURD_t();
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* interrupted_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    if(!success_all_kurd(running_task_kurd) || !interrupted_task){
        panic_with_kurd(frame, running_task_kurd);
    }
    bool is_user_context=((frame->iret_complex.cs&3)==3);
    {
        reentrant_spinlock_guard g(interrupted_task->task_lock);
        if(is_user_context){
            //not support
        }else{
            running_task_kurd=kthread_common_save(interrupted_task,frame,true);
            if(error_kurd(running_task_kurd)){
                goto fatals;
            }
        }
        interrupted_task->set_ready();
    }
    {
        reentrant_spinlock_guard g(scheduler.sched_lock);
        running_task_kurd=scheduler.insert_ready_task(interrupted_task);
        if(error_kurd(running_task_kurd)){
            goto fatals;    
        }
    }
    x2apic::x2apic_driver::write_eoi();
    ktime::heart_beat_alarm::set_clock_by_offset(DEFALUT_TIMER_SPAN_MIUS);
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(frame, running_task_kurd);
}
void kthread_exit_cppenter(x64_standard_context*context) 
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    KURD_t fatal=make_self_scheduler_fatal(Scheduler::self_scheduler_events::kthread_exit,0);
    task*exit_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    if(!success_all_kurd(running_task_kurd) || !exit_task){
        panic_with_kurd(running_task_kurd);
    }
    {
        reentrant_spinlock_guard g(exit_task->task_lock);
        running_task_kurd=kthread_common_save(exit_task,context,true);
        if(error_kurd(running_task_kurd)){
            goto fatals;
        }
        Ktemplats::list_doubly<uint64_t>&waiters_queue=exit_task->waiters;
        for(uint64_t i=0;i<waiters_queue.size();i++){
            using namespace Scheduler::self_scheduler_events::kthread_exit_results::fatal_reasons;
            uint64_t waiter_tid=waiters_queue.pop_front_value();
            task*waiter_task=task_pool::get_by_tid(waiter_tid,running_task_kurd);
            if(waiter_task->get_state()!=task_state_t::blocked){
                fatal.reason=bad_task_state;
                running_task_kurd=fatal;
                goto fatals;
            }
            if(waiter_task->blocked_reason!=wait_other_kthread){
                fatal.reason=waiter_bad_block_reason;
                running_task_kurd=fatal;
                goto fatals;
            }
            if(error_kurd(running_task_kurd)||!waiter_task)goto fatals;
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
        };
        exit_task->set_zombie();
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(context,running_task_kurd);
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
    KURD_t kurd=KURD_t();
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    uint64_t waitede_tid=context->rdi;
    task*waited_task=task_pool::get_by_tid(waitede_tid,running_task_kurd);
    if(error_kurd(running_task_kurd)||!waited_task){
        panic_with_kurd(running_task_kurd);
    }
    task* waiter_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    if(!success_all_kurd(running_task_kurd)||!waiter_task){
        panic_with_kurd(running_task_kurd);
    }
    {
        reentrant_spinlock_guard g(waited_task->task_lock);
        if(waited_task->get_state()==task_state_t::zombie){
            context->rax=waited_task->context.kthread->regs.rdi;
            return;
        }
        {
            reentrant_spinlock_guard h(waiter_task->task_lock);
            kurd=kthread_common_save(waiter_task,context,true);
            if(error_kurd(kurd))goto fatals;
            waiter_task->set_blocked();
            waiter_task->blocked_reason=task_blocked_reason_t::wait_other_kthread;
        }
        waited_task->waiters.push_back(waiter_task->get_tid());
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(running_task_kurd);
}
void kthread_self_blocked_cppenter(x64_standard_context* context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* blocked_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    {
        reentrant_spinlock_guard g(blocked_task->task_lock);
        running_task_kurd=kthread_common_save(blocked_task,context,true);
        if(error_kurd(running_task_kurd))goto fatals;
        blocked_task->blocked_reason=(task_blocked_reason_t)context->rdi;
        blocked_task->set_blocked();
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(context,running_task_kurd);
}
uint64_t wakeup_thread(uint64_t tid){
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
            kurd=target_scheduler.insert_ready_task(task_ptr);
            return kurd_get_raw(kurd);
        }
    }else{
        fail.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::fail_reasons::bad_task_state;
        return kurd_get_raw(fail);
    }
}
void kthread_sleep_cppenter(x64_standard_context*context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* sleeper_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    {
        reentrant_spinlock_guard g(sleeper_task->task_lock);
        running_task_kurd=kthread_common_save(sleeper_task,context,true);
        if(error_kurd(running_task_kurd))goto fatals;
        sleeper_task->blocked_reason=sleeping;
        sleeper_task->sleep_wakeup_stamp=ktime::get_microsecond_stamp()+context->rdi;
        sleeper_task->set_blocked();
    }
    {
        reentrant_spinlock_guard h(scheduler.sched_lock);
        running_task_kurd=scheduler.sleep_queue.insert(sleeper_task);
        if(error_kurd(running_task_kurd))goto fatals;
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(running_task_kurd);
}
void block_queue_cppenter(x64_standard_context *context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* blocked_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    tid_wait_queue*waite_queue=(tid_wait_queue*)context->rdi;
    KURD_t fatal=make_self_scheduler_fatal(
        Scheduler::self_scheduler_events::kthread_block_queue,0
    );
    if(!success_all_kurd(running_task_kurd) || !blocked_task){
        panic_with_kurd(running_task_kurd);
    }
    {
        spinlock_interrupt_about_guard(waite_queue->lock);
        {
            reentrant_spinlock_guard g(blocked_task->task_lock);
            running_task_kurd=kthread_common_save(blocked_task,context,true);
            if(error_kurd(running_task_kurd))goto fatals;
            blocked_task->set_blocked();
            waite_queue->push_back(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX));
        }
    }
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    fatals:
    panic_with_kurd(fatal);
}
void block_if_equal_cppenter(x64_standard_context *context)
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* blocked_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    tid_wait_queue*waite_queue=(tid_wait_queue*)context->rdi;
    uint64_t*check_address=(uint64_t*)context->rsi;
    uint64_t block_token=context->rdx;
    KURD_t fatal=make_self_scheduler_fatal(
        Scheduler::self_scheduler_events::kthread_block_queue_if_equal,0
    );
    if(!success_all_kurd(running_task_kurd) || !blocked_task){
        panic_with_kurd(running_task_kurd);
    }
    {
        spinlock_interrupt_about_guard g(waite_queue->lock);
        if(*check_address==block_token)
        {
            reentrant_spinlock_guard h(blocked_task->task_lock);
            running_task_kurd=kthread_common_save(blocked_task,context,true);
            if(error_kurd(running_task_kurd))goto fatals;
            blocked_task->set_blocked();
            waite_queue->push_back(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX));
            goto sched;
        }else{
            return;
        }
    }
    {
    sched:
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    }
    fatals:
    panic_with_kurd(fatal);
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