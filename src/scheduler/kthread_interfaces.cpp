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
#include "Scheduler/kthread_abi.h"
#include "Scheduler/bq_system.h"
#include "Scheduler/task_pool.h"
#include "panic.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kout.h"
#include "util/rb_map.h"
#include "memory/FreePagesAllocator.h"
extern rb_map<bq_id_t, block_queue*> container;
extern spinrwlock_cpp_t container_lock;
namespace {
constexpr uint64_t kthread_yield_saved_stack_delta = 16 * sizeof(uint64_t);
spinlock_cpp_t global_tid_lock;
uint64_t global_tid_counter = 0;


static inline KURD_t kthreads_default_kurd()
{
    return KURD_t(0, 0, module_code::SCHEDULER, Scheduler::KTHREADS, 0, 0, err_domain::CORE_MODULE);
}

static inline KURD_t make_kthreads_fatal(
    uint8_t event_code, 
    uint16_t reason
)
{
    KURD_t kurd = kthreads_default_kurd();
    kurd.event_code = event_code;
    kurd.reason = reason;
    return set_fatal_result_level(kurd);
}

static inline KURD_t make_kthreads_set_state_fatal()
{
    return make_kthreads_fatal(
        Scheduler::KTHREADS_EVENTS::EVENT_CODE_SET_STATE,
        Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::STATE_TRANSITION_FAIL);
}

static inline void panic_with_kurd(x64_standard_context_v2 *frame, KURD_t kurd,char*message=nullptr)
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
void kthread_common_save(x64_standard_context_v2*frame,bool expect_running,task* task_ptr)
{
    if (!task_ptr) {
        KURD_t fatal = make_kthreads_fatal(
            Scheduler::KTHREADS_EVENTS::EVENT_CODE_KTHREAD_COMMON_SAVE,
            Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::NULL_RUNNING_TASK);
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: null running task");
    }
    if (expect_running && task_ptr->get_state() != task_state_t::running) {
        KURD_t fatal = make_kthreads_fatal(
            Scheduler::KTHREADS_EVENTS::EVENT_CODE_KTHREAD_COMMON_SAVE,
            Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::BAD_TASK_STATE);
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: not running");
    }
    task_ptr->task_event_shift(task::event_type_t::offline);
    
    bool is_from_vm=!!(frame->core_ctx.fred.errcode&0x8000000000000000ull);
    if(is_from_vm){
        // panic，以其完全不支持的原因
    }else{
        if((frame->core_ctx.fred.cs & 0x3)==3){
            // panic,暂时不支持
        }else{
            vaddr_t stack_bottom = task_ptr->priv_stack_base+(task_ptr->priv_stack_pages<<12)-64;
            vaddr_t stack_top   = task_ptr->priv_stack_base;
            vaddr_t rsp=frame->core_ctx.fred.rsp;
            if (rsp > stack_bottom || rsp < stack_top) {
                KURD_t fatal = make_kthreads_fatal(
                    Scheduler::KTHREADS_EVENTS::EVENT_CODE_KTHREAD_COMMON_SAVE,
                    Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::PRIVCTX_STACKPTR_OOR);
             panic_with_kurd(frame, fatal, (char*)"kthread_common_save: stack ptr OOR");
            }
            task_ptr->priv_ctx = *frame;
            task_ptr->priv_ctx.core_ctx.idtctx.iret.cs&=0xffff;
            task_ptr->priv_ctx.core_ctx.idtctx.iret.ss&=0xffff;
        }
    }
}
KURD_t task_launch(task *t, uint32_t pid)
{//还是要符合从内核上下文线程开始，以及基础iret_complex校验的
    namespace ev = Scheduler::KTHREADS_EVENTS;
    namespace fr = ev::task_launch_results::FAIL_REASONS;

    auto mkfail = [&]() -> KURD_t {
        KURD_t k = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
            Scheduler::KTHREADS, ev::EVENT_CODE_TASK_LAUNCH,
            level_code::ERROR, err_domain::CORE_MODULE);
        return k;
    };

    // ① TID 有效性
    if(t->get_tid()==INVALID_TID){
        KURD_t k = mkfail();
        k.reason = fr::INVALID_TID;
        return k;
    }

    // ② rip、rsp 必须在内核地址空间
    if(!is_addr_kernel_address((void*)t->priv_ctx.core_ctx.idtctx.iret.rip)||
       !is_addr_kernel_address((void*)t->priv_ctx.core_ctx.idtctx.iret.rsp)){
        KURD_t k = mkfail();
        k.reason = fr::TARGET_NOT_KERNEL_ADDR;
        return k;
    }

    // ③ 初始上下文必须是从内核态开始的 priv 上下文
    if(t->choose!=task::ctx_choose::priv){
        KURD_t k = mkfail();
        k.reason = fr::NOT_PRIV_CTX;
        return k;
    }

    // ④ 目标处理器调度器
    per_processor_scheduler*target=get_other_scheduler(pid);

    // ⑤ 状态机：init → ready
    {
        reentrant_spinlock_guard g(t->task_lock);
        if(!t->set_ready()){
            KURD_t k = mkfail();
            k.reason = fr::STATE_TRANSITION_FAIL;
            return k;
        }
    }

    // ⑥ 插入目标 ready_queue
    KURD_t kurd;
    {
        reentrant_spinlock_guard g(target->sched_lock);
        kurd=target->insert_ready_task(t,false);
    }

    return kurd;
}
[[noreturn]] void kthread_yield_true_enter(x64_standard_context_v2*context)
{

    per_processor_scheduler&scheduler=*get_self_scheduler();
    task* yield_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(yield_task->task_lock);
        kthread_common_save(context,true,yield_task);
        if (!yield_task->set_ready())
            panic_with_kurd(context, make_kthreads_set_state_fatal());
    }
    if(!scheduler.is_the_idle_task(yield_task))
    {
        reentrant_spinlock_guard g(scheduler.sched_lock);
        scheduler.insert_ready_task(yield_task);
    }
    scheduler.next_task_with_routine();
}
extern "C" [[noreturn]] void resched(x64_standard_context_v2 *frame)
{
    per_processor_scheduler&scheduler=*get_self_scheduler();
    task* interrupted_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    bool is_user_context=((frame->core_ctx.idtctx.iret.cs&3)==3);
    {
        reentrant_spinlock_guard g(interrupted_task->task_lock);
        if(!is_user_context){
            kthread_common_save(frame,true,interrupted_task);
        }
        if (!interrupted_task->set_ready())
            panic_with_kurd(frame, make_kthreads_set_state_fatal());
    }
    if(!scheduler.is_the_idle_task(interrupted_task))
    {
        reentrant_spinlock_guard g(scheduler.sched_lock);
        scheduler.insert_ready_task(interrupted_task);
    }
    scheduler.next_task_with_routine();
}
[[noreturn]] void kthread_exit_cppenter(x64_standard_context_v2*context) 
{
    per_processor_scheduler&scheduler=*get_self_scheduler();
    task*exit_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(exit_task->task_lock);
        kthread_common_save(context,true,exit_task);
        if (!exit_task->set_zombie())
            panic_with_kurd(context, make_kthreads_set_state_fatal());
    }
    scheduler.next_task_with_routine();
}
[[noreturn]] void kthread_self_blocked_cppenter(x64_standard_context_v2* context)
{
    per_processor_scheduler&scheduler=*get_self_scheduler();
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(blocked_task->task_lock);
        kthread_common_save(context,true,blocked_task);
        if (!blocked_task->set_blocked())
            panic_with_kurd(context, make_kthreads_set_state_fatal());
    }
    scheduler.next_task_with_routine();
}
ckurd wakeup_thread(uint64_t tid, bool front_insert){
    interrupt_guard g;
    namespace ev = Scheduler::KTHREADS_EVENTS;
    KURD_t success = KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
        Scheduler::KTHREADS, ev::EVENT_CODE_WAKEUP_THREAD,
        level_code::INFO, err_domain::CORE_MODULE);
    KURD_t fail = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
        Scheduler::KTHREADS, ev::EVENT_CODE_WAKEUP_THREAD,
        level_code::ERROR, err_domain::CORE_MODULE);

    KURD_t kurd;
    task*task_ptr=task_pool::get_by_tid(tid,kurd);
    if(!success_all_kurd(kurd)){
        return kurd_get_raw(kurd);
    }
    reentrant_spinlock_guard l(task_ptr->task_lock);
    per_processor_scheduler*target_scheduler=get_other_scheduler(task_ptr->belonged_processor_id);
    if(task_ptr->get_state()==task_state_t::ready||
    task_ptr->get_state()==task_state_t::running){
        success.reason=ev::wakeup_thread_results::SUCCESS_REASONS::ALREADY_RUNNING_OR_WAKEUP;
        return kurd_get_raw(success);
    }else if(task_ptr->get_state()==task_state_t::blocked){
        if(task_ptr->on_blockers_queue_bit){
            fail.reason=ev::wakeup_thread_results::FAIL_REASONS::TASK_ON_BLOCK_QUEUE;
            return kurd_get_raw(fail);
        }
        if (!task_ptr->set_ready())
            panic_with_kurd(make_kthreads_set_state_fatal());
        {
            reentrant_spinlock_guard h(target_scheduler->sched_lock);
            kurd=target_scheduler->insert_ready_task(task_ptr, front_insert);
            return kurd_get_raw(kurd);
        }
    }else{
        fail.reason=ev::wakeup_thread_results::FAIL_REASONS::BAD_TASK_STATE;
        return kurd_get_raw(fail);
    }
}
[[noreturn]] void kthread_sleep_cppenter(x64_standard_context_v2*context)
{
    per_processor_scheduler*scheduler=get_other_scheduler(fast_get_processor_id());
    task* sleeper_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(sleeper_task->task_lock);
        kthread_common_save(context,true,sleeper_task);
        sleeper_task->min_wakeup_stamp=ktime::get_microsecond_stamp()+context->rdi;
        if (!sleeper_task->set_blocked())
            panic_with_kurd(context, make_kthreads_set_state_fatal());
        sleeper_task->on_blockers_queue_bit = true;
        sleeper_task->task_event_shift( task::event_type_t::sleep);
        {
        reentrant_spinlock_guard h(scheduler->sched_lock);
        scheduler->sleep_queue.insert(sleeper_task);
        }
    }
    
    scheduler->next_task_with_routine();
}
void block_if_equal_cppenter(x64_standard_context_v2 *context)
{
    per_processor_scheduler*scheduler=get_self_scheduler();
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    uint64_t qid=context->rdi;
    block_queue**q;
    block_queue*waite_queue;
    {
        spinrwlock_interrupt_about_read_guard l(container_lock);
        q=container.find(qid);
        if(q==nullptr){
            //rax里面放错误码
        }
        waite_queue=*q;
    }
    uint64_t*check_address=(uint64_t*)context->rsi;
    uint64_t block_token=context->rdx;
    bool should_block=false;
    {
        spinlock_interrupt_about_guard g(waite_queue->qlock);
        if(*check_address==block_token)
        {
            {
            context->rax|=1;
            task::event_type_t qevt=waite_queue->get_queue_event();
            reentrant_spinlock_guard h(blocked_task->task_lock);
            kthread_common_save(context,true,blocked_task);
            if (!blocked_task->set_blocked())
                panic_with_kurd(context, make_kthreads_set_state_fatal());
            blocked_task->task_event_shift(qevt);
            blocked_task->on_blockers_queue_bit = true;
            blocked_task->min_wakeup_stamp = ktime::get_microsecond_stamp() + 5000000;
            should_block=true;
            }
            waite_queue->push_tail(blocked_task);//这里面会使用task锁保护一下事件切换历程
        }else{
            context->rax&=(~1);
        }
        
    }
    if(should_block){
        scheduler->next_task_with_routine();
    }
}
void kthread_call_cpp_enter(x64_standard_context_v2 *frame)
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
        case kthread_call_num::block:
        {
            kthread_self_blocked_cppenter(frame);
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
ckurd kthread_init(task *t, kthread_creating_package *p)
{
    t->priv_ctx.core_ctx.idtctx.iret.cs = K_cs_idx << 3;
    t->priv_ctx.core_ctx.idtctx.iret.ss = K_ds_ss_idx << 3;
    t->priv_ctx.core_ctx.idtctx.iret.rflags = INIT_DEFAULT_RFLAGS;
    t->priv_ctx.rdi = (uint64_t)p->func_raw;
    t->priv_ctx.rsi = p->args[0];
    t->priv_ctx.rdx = p->args[1];
    t->priv_ctx.rcx = p->args[2];
    t->priv_ctx.r8  = p->args[3];
    t->priv_ctx.r9  = p->args[4];
    KURD_t kurd;
    if (!t->priv_stack_base) {
        t->priv_stack_base = stack_alloc(&kurd, DEFAULT_PRIVSTACK_PGS_COUNT);
        if (error_kurd(kurd)) return kurd_get_raw(kurd);
        t->priv_stack_pages = DEFAULT_PRIVSTACK_PGS_COUNT;
    }
    t->priv_ctx.core_ctx.idtctx.iret.rsp = t->priv_stack_base + (t->priv_stack_pages << 12) - 64;
    t->priv_ctx.core_ctx.idtctx.iret.rip = (uint64_t)&allkthread_true_enter;
    t->choose = task::ctx_choose::priv;
    return ckurd();
}

uint64_t creat_kthread(kthread_creating_package *p,KURD_t*kurd)
{
    task* t=task::basic_constructor();
    uint64_t tid=t->get_tid();
    ckurd kp=kthread_init(t,p);
    *kurd=raw_analyze(kp);
    if(error_kurd(*kurd))return INVALID_TID;
    *kurd=task_launch(t,p->launch_pid);
    return tid;
}
ckurd release_kthread(uint64_t tid)
{
    namespace ev = Scheduler::KTHREADS_EVENTS;
    KURD_t success = KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
        Scheduler::KTHREADS, ev::EVENT_CODE_RELEASE_KTHREAD,
        level_code::INFO, err_domain::CORE_MODULE);
    KURD_t fail = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
        Scheduler::KTHREADS, ev::EVENT_CODE_RELEASE_KTHREAD,
        level_code::ERROR, err_domain::CORE_MODULE);

    KURD_t k;
    task*t=task_pool::get_by_tid(tid,k);
    if(t==nullptr){
        return kurd_get_raw(k);
    }
    if(t->get_state()!=task_state_t::zombie){
        fail.reason = ev::release_kthread_results::FAIL_REASONS::TASK_NOT_ZOMBIE;
        return kurd_get_raw(fail);
    }
    k=__wrapped_pgs_vfree((void*)t->priv_stack_base,t->priv_stack_pages);
    if(error_kurd(k))return kurd_get_raw(k);
    task_pool::release(tid);
    return kurd_get_raw(success);
}
