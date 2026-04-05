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
spinlock_interrupt_about_cpp_t global_tid_lock;
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

static inline void panic_with_kurd(const x64_Interrupt_saved_context_no_errcode *frame, KURD_t kurd,char*message=nullptr)
{
    panic_info_inshort inshort{
        .is_bug = true,
        .is_policy = true,
        .is_hw_fault = false,
        .is_mem_corruption = false,
        .is_escalated = false
    };
    panic_context::x64_context panic_ctx = Panic::convert_to_panic_context(
        const_cast<x64_Interrupt_saved_context_no_errcode*>(frame)
    );
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

static inline void convert_interrupt_no_err_to_basic(const x64_Interrupt_saved_context_no_errcode *frame,
                                                     x64_basic_context *out)
{
    if (!frame || !out) {
        return;
    }
    out->rax = frame->rax;
    out->rbx = frame->rbx;
    out->rcx = frame->rcx;
    out->rdx = frame->rdx;
    out->rsi = frame->rsi;
    out->rdi = frame->rdi;
    out->rbp = frame->rbp;
    out->iret_context.rsp = frame->rsp;
    out->r8 = frame->r8;
    out->r9 = frame->r9;
    out->r10 = frame->r10;
    out->r11 = frame->r11;
    out->r12 = frame->r12;
    out->r13 = frame->r13;
    out->r14 = frame->r14;
    out->r15 = frame->r15;
    out->iret_context.rip = frame->rip;
    out->iret_context.rflags = frame->rflags;
    out->iret_context.cs = frame->cs;
    out->iret_context.ss = frame->ss;
}
} // namespace

void allthread_true_enter(void *(*entry)(void *), void *arg){
    uint64_t return_value=(uint64_t)entry(arg);
    kthread_exit(return_value);
};
uint64_t create_kthread(void *(*entry)(void *), void *arg, KURD_t *out_kurd)
{
    interrupt_guard g;
    kthread_context* context = new kthread_context();
    context->regs.iret_context.rip = (uint64_t)allthread_true_enter;
    context->regs.rsi = (uint64_t)arg;
    context->regs.rdi = (uint64_t)entry;
    context->regs.iret_context.cs = x64_local_processor::K_cs_idx<<3;
    context->regs.iret_context.ss = x64_local_processor::K_ds_ss_idx<<3;
    context->stacksize = DEFAULT_STACK_SIZE;
    context->stack_bottom = (uint64_t)stack_alloc(out_kurd,DEFAULT_STACK_PG_COUNT);
    context->regs.iret_context.rsp = context->stack_bottom;
    context->regs.iret_context.rflags = INIT_DEFAULT_RFLAGS;
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
void kthread_yield_true_enter(x64_basic_context*context)
{

    x64_basic_context basic_ctx=*context;
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* interrupted_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    interrupted_task->task_lock.lock();
    if(!interrupted_task){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(running_task_kurd);
    }
    interrupted_task->lastest_span_length=ktime::get_microsecond_stamp()-interrupted_task->lastest_run_stamp;
    interrupted_task->accumulated_time+=interrupted_task->lastest_span_length;
    if(interrupted_task->get_task_type()!=task_type_t::kthreadm){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_yield_enter,
            Scheduler::self_scheduler_events::kthread_yield_enter_results::fatal_reasons::invalid_task_type
        ));
    }
    if(!interrupted_task->context.kthread){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_yield_enter,
            Scheduler::self_scheduler_events::kthread_yield_enter_results::fatal_reasons::null_kthread_context
        ));
    }
    interrupted_task->context.kthread->regs=basic_ctx;
    if(interrupted_task->context.kthread->stacksize==0){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_yield_enter,
            Scheduler::self_scheduler_events::kthread_yield_enter_results::fatal_reasons::invalid_stack_size
        ));
    }
    {
        vaddr_t stack_top = interrupted_task->context.kthread->stack_bottom-interrupted_task->context.kthread->stacksize;
        vaddr_t stack_bottom = interrupted_task->context.kthread->stack_bottom;
        if(basic_ctx.iret_context.rsp < stack_top || basic_ctx.iret_context.rsp >= stack_bottom){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::kthread_yield_enter,
                Scheduler::self_scheduler_events::kthread_yield_enter_results::fatal_reasons::rsp_out_of_range
            ),"stackptr out of range");
        }
    }
    interrupted_task->set_ready();
    interrupted_task->task_lock.unlock();
    scheduler.sched_lock.lock();
    KURD_t kurd=scheduler.insert_ready_task(interrupted_task);
    if(error_kurd(kurd)){
        scheduler.sched_lock.unlock();
        panic_with_kurd(kurd);
    }
    scheduler.sched_lock.unlock();
    scheduler.sleep_tasks_wake();
    scheduler.sched();

}
void timer_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    x64_basic_context basic_ctx{};
    KURD_t kurd=KURD_t();
    convert_interrupt_no_err_to_basic(frame, &basic_ctx);
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* interrupted_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    if(!success_all_kurd(running_task_kurd) || !interrupted_task){
        panic_with_kurd(frame, running_task_kurd);
    }
    interrupted_task->task_lock.lock();
    interrupted_task->lastest_span_length=ktime::get_microsecond_stamp()-interrupted_task->lastest_run_stamp;
    interrupted_task->accumulated_time+=interrupted_task->lastest_span_length;
    switch(interrupted_task->get_task_type()){
        case task_type_t::kthreadm:
        if((frame->cs&3)!=0){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(frame, make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::timer_cpp_enter,
                Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::invalid_cs
            ));
        }
        if(!interrupted_task->context.kthread){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(frame, make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::timer_cpp_enter,
                Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::null_kthread_context
            ));
        }
        interrupted_task->context.kthread->regs=basic_ctx;
        if(interrupted_task->context.kthread->stacksize==0){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(frame, make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::timer_cpp_enter,
                Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::invalid_stack_size
            ));
        }
        {
            vaddr_t stack_top = interrupted_task->context.kthread->stack_bottom-interrupted_task->context.kthread->stacksize;
        vaddr_t stack_bottom = interrupted_task->context.kthread->stack_bottom;
            if(basic_ctx.iret_context.rsp < stack_top || basic_ctx.iret_context.rsp > stack_bottom){
                interrupted_task->task_lock.unlock();
                x2apic::x2apic_driver::broadcast_exself_fixed_ipi(Panic::other_processors_froze_handler);
                GlobalKernelStatus=PANIC;
                bsp_kout<<"Panic: stackptr out of range that current stack ptr:0x"<<HEX<<basic_ctx.iret_context.rsp<<"\n stack top:0x"<<stack_top<<"\n stack bottom:0x"<<stack_bottom<<kendl;
                panic_with_kurd(frame, make_self_scheduler_fatal(
                    Scheduler::self_scheduler_events::timer_cpp_enter,
                    Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::rsp_out_of_range
                ));
            }
        }
            break;
        case task_type_t::userthread:
        if((frame->cs&3)!=3){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(frame, make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::timer_cpp_enter,
                Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::invalid_cs
            ));
        }
        if(!interrupted_task->context.userthread){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(frame, make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::timer_cpp_enter,
                Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::null_userthread_context
            ));
        }

            break;
        default:
        interrupted_task->task_lock.unlock();
            panic_with_kurd(frame, make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::timer_cpp_enter,
                Scheduler::self_scheduler_events::timer_cpp_enter_results::fatal_reasons::invalid_task_type
                ));
            break;
    }
    interrupted_task->set_ready();
    interrupted_task->task_lock.unlock();
    scheduler.sched_lock.lock();
    kurd=scheduler.insert_ready_task(interrupted_task);
    if(error_kurd(kurd)){
        scheduler.sched_lock.unlock();
        panic_with_kurd(frame, kurd);
    }
    scheduler.sched_lock.unlock();
    x2apic::x2apic_driver::write_eoi();
    ktime::heart_beat_alarm::set_clock_by_offset(DEFALUT_TIMER_SPAN_MIUS);
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
void kthread_exit_cppenter(x64_basic_context*context) 
{
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    KURD_t success,fail,fatal;
    success=KURD_t(result_code::SUCCESS,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::kthread_exit,level_code::INFO,err_domain::CORE_MODULE);
    fail=KURD_t(result_code::FAIL,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::kthread_exit,level_code::ERROR,err_domain::CORE_MODULE);
    fatal=KURD_t(result_code::FATAL,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::kthread_exit,level_code::FATAL,err_domain::CORE_MODULE);
    task*exit_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    if(!success_all_kurd(running_task_kurd) || !exit_task){
        panic_with_kurd(running_task_kurd);
    }
    exit_task->task_lock.lock();
    exit_task->lastest_span_length=ktime::get_microsecond_stamp()-exit_task->lastest_run_stamp;
    exit_task->accumulated_time+=exit_task->lastest_span_length;
    exit_task->context.kthread->regs=*context;
    uint64_t will=context->rdi;
    uint64_t stack_bottom=exit_task->context.kthread->stack_bottom;
    uint64_t stack_top=stack_bottom-exit_task->context.kthread->stacksize;
    uint64_t rsp=context->iret_context.rsp;
    if(rsp<stack_top||rsp>=stack_bottom){
        exit_task->task_lock.unlock();
        fatal.reason=Scheduler::self_scheduler_events::kthread_exit_results::fatal_reasons::context_stackptr_out_of_range;
        panic_with_kurd(fatal);
    }
    if(!exit_task->set_zombie()){
        exit_task->task_lock.unlock();
        fatal.reason=Scheduler::self_scheduler_events::kthread_exit_results::fatal_reasons::bad_task_state;
        panic_with_kurd(fatal);
    }
    while(true){
        if(exit_task->waiters.empty()){
            exit_task->task_lock.unlock();
            break;
        }
        uint64_t waiter_tid=exit_task->waiters.pop_front_value();
        task*waiter_task=task_pool::get_by_tid(waiter_tid,running_task_kurd);
        if(!success_all_kurd(running_task_kurd)||!waiter_task){
            panic_with_kurd(running_task_kurd);
        }
        waiter_task->task_lock.lock();
        waiter_task->context.kthread->regs.rax=will;
        per_processor_scheduler&sched=global_schedulers[waiter_task->get_belonged_processor_id()];
        if(waiter_task->get_state()!=task_state_t::blocked||
        waiter_task->blocked_reason!=wait_other_kthread){
            waiter_task->task_lock.unlock();
            fatal.reason=Scheduler::self_scheduler_events::kthread_exit_results::fatal_reasons::waiter_bad_state_and_block_reason;
            panic_with_kurd(fatal);
        }
        waiter_task->set_ready();
        sched.sched_lock.lock();
        running_task_kurd=sched.insert_ready_task(waiter_task);
        if(error_kurd(running_task_kurd)){
            waiter_task->task_lock.unlock();
            sched.sched_lock.unlock();
            panic_with_kurd(running_task_kurd);
        }
        sched.sched_lock.unlock();
        waiter_task->task_lock.unlock();
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
void kthread_wait_cppenter(x64_basic_context *context)
{
    KURD_t kurd=KURD_t();
    KURD_t success,fail,fatal;
    success=KURD_t(result_code::SUCCESS,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::kthread_wait,level_code::INFO,err_domain::CORE_MODULE);
    fail=KURD_t(result_code::FAIL,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::kthread_wait,level_code::ERROR,err_domain::CORE_MODULE);
    fatal=KURD_t(result_code::FATAL,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,Scheduler::self_scheduler_events::kthread_wait,level_code::FATAL,err_domain::CORE_MODULE);
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
    waited_task->task_lock.lock();
    if(waited_task->get_state()==task_state_t::zombie){
        uint64_t will=waited_task->context.kthread->regs.rdi;
        waited_task->task_lock.unlock();
        context->rax=will;
        waiter_task->task_lock.lock();
        waiter_task->lastest_span_length=ktime::get_microsecond_stamp()-waiter_task->lastest_run_stamp;
    waiter_task->accumulated_time+=waiter_task->lastest_span_length;
    
    waiter_task->context.kthread->regs=*context;
    uint64_t stack_top=waiter_task->context.kthread->stack_bottom-waiter_task->context.kthread->stacksize;
    uint64_t stack_bottom=waiter_task->context.kthread->stack_bottom;
    uint64_t rsp=context->iret_context.rsp;
    if(!(rsp<stack_bottom&&rsp>=stack_top)){
        waiter_task->task_lock.unlock();
        fatal.reason=Scheduler::self_scheduler_events::kthread_wait_results::fatal_reasons::context_stackptr_out_of_range;
        panic_with_kurd(fatal);
    }if(!waiter_task->set_ready()){
        fatal.reason=Scheduler::self_scheduler_events::kthread_wait_results::fatal_reasons::bad_task_state;
        panic_with_kurd(fatal);
    }
    scheduler.sched_lock.lock();
    running_task_kurd=scheduler.insert_ready_task(waiter_task);
    if(error_kurd(running_task_kurd)){
        waiter_task->task_lock.unlock();
        scheduler.sched_lock.unlock();
        panic_with_kurd(running_task_kurd);
    }
    scheduler.sched_lock.unlock();
    waiter_task->task_lock.unlock();
    scheduler.sleep_tasks_wake();
    scheduler.sched();
    }
    waiter_task->task_lock.lock();
    if(!waiter_task->set_blocked()){
        fatal.reason=Scheduler::self_scheduler_events::kthread_wait_results::fatal_reasons::bad_task_state;
        panic_with_kurd(fatal);
    }waiter_task->blocked_reason=wait_other_kthread;
    waiter_task->lastest_span_length=ktime::get_microsecond_stamp()-waiter_task->lastest_run_stamp;
    waiter_task->accumulated_time+=waiter_task->lastest_span_length;
    
    waiter_task->context.kthread->regs=*context;
    uint64_t stack_top=waiter_task->context.kthread->stack_bottom-waiter_task->context.kthread->stacksize;
    uint64_t stack_bottom=waiter_task->context.kthread->stack_bottom;
    uint64_t rsp=context->iret_context.rsp;
    if(!(rsp<stack_bottom&&rsp>=stack_top)){
        fatal.reason=Scheduler::self_scheduler_events::kthread_wait_results::fatal_reasons::context_stackptr_out_of_range;
        panic_with_kurd(fatal);
    }
    waiter_task->task_lock.unlock();
    waited_task->waiters.push_back(waiter_task->get_tid());
    waited_task->task_lock.unlock();
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
void kthread_self_blocked_cppenter(x64_basic_context* context)
{
    x64_basic_context basic_ctx=*context;
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
    task* interrupted_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    interrupted_task->task_lock.lock();
    if(!success_all_kurd(running_task_kurd) || !interrupted_task){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(running_task_kurd);
    }
    interrupted_task->lastest_span_length=ktime::get_microsecond_stamp()-interrupted_task->lastest_run_stamp;
    interrupted_task->accumulated_time+=interrupted_task->lastest_span_length;
    if(interrupted_task->get_task_type()!=task_type_t::kthreadm){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_block,
            Scheduler::self_scheduler_events::kthread_block_results::fatal_reasons::bad_task_type
        ));
    }
    if(!interrupted_task->context.kthread){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_block,
            Scheduler::self_scheduler_events::kthread_block_results::fatal_reasons::context_nullptr
        ));
    }
    interrupted_task->context.kthread->regs=basic_ctx;
    if(interrupted_task->context.kthread->stacksize==0){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_block,
            Scheduler::self_scheduler_events::kthread_block_results::fatal_reasons::context_null_stack_size
        ));
    }
    {
        vaddr_t stack_top = interrupted_task->context.kthread->stack_bottom-interrupted_task->context.kthread->stacksize;
        vaddr_t stack_bottom = interrupted_task->context.kthread->stack_bottom;
        if(basic_ctx.iret_context.rsp < stack_top || basic_ctx.iret_context.rsp >= stack_bottom){
            interrupted_task->task_lock.unlock();
            panic_with_kurd(make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::kthread_block,
                Scheduler::self_scheduler_events::kthread_block_results::fatal_reasons::context_stackptr_out_of_range
            ),"stackptr out of range");
        }
    }
    interrupted_task->blocked_reason = static_cast<task_blocked_reason_t>(context->rdi);
    if(interrupted_task->get_state()!=task_state_t::running){
        interrupted_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_block,
            Scheduler::self_scheduler_events::kthread_block_results::fatal_reasons::illeage_state
        ));
    }
    interrupted_task->set_blocked();
    interrupted_task->task_lock.unlock();
    scheduler.sleep_tasks_wake();
    scheduler.sched();
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
    task_ptr->task_lock.lock();
    per_processor_scheduler&task_in_processor=global_schedulers[task_ptr->get_belonged_processor_id()];
    if(task_ptr->get_state()==task_state_t::ready||
    task_ptr->get_state()==task_state_t::running){
        success.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::success_reasons::already_wakeup_or_running;
        task_ptr->task_lock.unlock();
        return kurd_get_raw(success);
        //成功但是已经运行
    }else if(task_ptr->get_state()==task_state_t::blocked){
        if(task_ptr->blocked_reason==sleeping||task_ptr->blocked_reason==wait_other_kthread){
            task_ptr->task_lock.unlock();
            fail.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::fail_reasons::kthread_cant_wake_for_bad_block_reason;
            return kurd_get_raw(fail);
        }
        task_ptr->set_ready();
        task_in_processor.sched_lock.lock();
        kurd=task_in_processor.insert_ready_task(task_ptr);
        task_in_processor.sched_lock.unlock();
        task_ptr->task_lock.unlock();
        return kurd_get_raw(kurd);
    }else{
        task_ptr->task_lock.unlock();
        fail.reason=Scheduler::self_scheduler_events::wake_up_kthread_results::fail_reasons::bad_task_state;
        return kurd_get_raw(fail);
    }
}
void kthread_sleep_cppenter(x64_basic_context*context)
{
    x64_basic_context basic_ctx=*context;
    per_processor_scheduler&scheduler=global_schedulers[fast_get_processor_id()];
    KURD_t running_task_kurd=KURD_t();
     task* sleeper_task=task_pool::get_by_tid(read_gs_u64(PROCESSOR_NOW_RUNNING_TID_GS_INDEX),running_task_kurd);
    sleeper_task->task_lock.lock();
    if(!success_all_kurd(running_task_kurd) || !sleeper_task){
        sleeper_task->task_lock.unlock();
        panic_with_kurd(running_task_kurd);
    }
    sleeper_task->lastest_span_length=ktime::get_microsecond_stamp()-sleeper_task->lastest_run_stamp;
    sleeper_task->accumulated_time+=sleeper_task->lastest_span_length;
    if(sleeper_task->get_task_type()!=task_type_t::kthreadm){
        sleeper_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_sleep,
            Scheduler::self_scheduler_events::kthread_sleep_results::fatal_reasons::bad_task_state
        ));
    }
    if(!sleeper_task->context.kthread){
        sleeper_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_sleep,
            Scheduler::self_scheduler_events::kthread_sleep_results::fatal_reasons::context_nullptr
        ));
    }
    sleeper_task->context.kthread->regs=basic_ctx;
    if(sleeper_task->context.kthread->stacksize==0){
        sleeper_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_sleep,
            Scheduler::self_scheduler_events::kthread_sleep_results::fatal_reasons::context_null_stack_size
        ));
    }
    {
        vaddr_t stack_top = sleeper_task->context.kthread->stack_bottom-sleeper_task->context.kthread->stacksize;
        vaddr_t stack_bottom = sleeper_task->context.kthread->stack_bottom;
        if(basic_ctx.iret_context.rsp < stack_top || basic_ctx.iret_context.rsp >= stack_bottom){
            sleeper_task->task_lock.unlock();
            panic_with_kurd(make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::kthread_sleep,
                Scheduler::self_scheduler_events::kthread_sleep_results::fatal_reasons::context_stackptr_out_of_range
            ),"stackptr out of range");
        }
    }
    sleeper_task->blocked_reason = sleeping;
    sleeper_task->sleep_wakeup_stamp=ktime::get_microsecond_stamp()+context->rdi;
    if(!sleeper_task->set_blocked()){
        sleeper_task->task_lock.unlock();
        panic_with_kurd(make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_sleep,
            Scheduler::self_scheduler_events::kthread_sleep_results::fatal_reasons::illeage_state
        ));
    }
    sleeper_task->task_lock.unlock();
    scheduler.sched_lock.lock();
    KURD_t kurd= scheduler.sleep_queue.insert(sleeper_task);
    if(error_kurd(kurd)){
        scheduler.sched_lock.unlock();
        panic_with_kurd(kurd);
    }
    scheduler.sched_lock.unlock();
    scheduler.sleep_tasks_wake();
    scheduler.sched();
}
void kthread_call_cpp_enter(x64_Interrupt_saved_context_no_errcode *frame)
{
    x64_basic_context basic_ctx{};
    convert_interrupt_no_err_to_basic(frame, &basic_ctx);
    switch(frame->rax){
        case kthread_call_num::exit:
        {
            kthread_exit_cppenter(&basic_ctx);
            break;
        }
        case kthread_call_num::sleep:
        {
            kthread_sleep_cppenter(&basic_ctx);
            break;
        }
        case kthread_call_num::yield:
        {
            kthread_yield_true_enter(&basic_ctx);
            break;
        }
        case kthread_call_num::wait:
        {
            kthread_wait_cppenter(&basic_ctx);
            break;
        }
        case kthread_call_num::block:
        {
            kthread_self_blocked_cppenter(&basic_ctx);
            break;
        }
        default:
        {
            //panic
        }
    }
}