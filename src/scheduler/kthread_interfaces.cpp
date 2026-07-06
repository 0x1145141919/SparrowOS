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
task* kthread_common_save(x64_standard_context_v2*frame,bool expect_running)
{
    task* task_ptr = (task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    if (!task_ptr) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: null running task");
    }
    if (expect_running && task_ptr->get_state() != task_state_t::running) {
        KURD_t fatal = make_self_scheduler_fatal(
            Scheduler::self_scheduler_events::kthread_common_save, 0);
        fatal.reason = Scheduler::self_scheduler_events::kthread_common_save_results::fatal_reasons::bad_task_state;
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: not running");
    }
    task_ptr->task_event_shift(task::event_type_t::offline);
    
    bool is_from_vm=!!(frame->core_ctx.fred.errcode&0x8000000000000000ull);
    if(is_from_vm){
        // panic，以其完全不支持的原因
    }else{
        if(IDT_CS(frame->core_ctx.fred.cs) & 0x3==3){
            // panic,暂时不支持
        }else{
            vaddr_t stack_bottom = task_ptr->priv_stack_base+(task_ptr->priv_stack_pages<<12)-64;
            vaddr_t stack_top   = task_ptr->priv_stack_base;
            vaddr_t rsp=frame->core_ctx.fred.rsp;
            if (rsp > stack_bottom || rsp < stack_top) {
                KURD_t fatal = make_self_scheduler_fatal(
                Scheduler::self_scheduler_events::kthread_common_save, 0);
                fatal.reason = Scheduler::self_scheduler_events::kthread_common_save_results::fatal_reasons::context_stackptr_out_of_range;
             panic_with_kurd(frame, fatal, (char*)"kthread_common_save: stack ptr OOR");
            }
            task_ptr->priv_ctx = *frame;
            task_ptr->priv_ctx.core_ctx.idtctx.iret.cs&=0xffff;
            task_ptr->priv_ctx.core_ctx.idtctx.iret.ss&=0xffff;
        }
    }
        
    
    
    
    return task_ptr;
}
KURD_t task_launch(task *t, uint32_t pid)
{//还是要符合从内核上下文线程开始，以及基础iret_complex校验的
    auto mkfail=[]()->KURD_t{
        KURD_t k=KURD_t(0,0,module_code::SCHEDULER,
            Scheduler::self_scheduler,0,0,err_domain::CORE_MODULE);
        k.result=result_code::FAIL;
        k.level=level_code::ERROR;
        return k;
    };

    // ① TID 有效性
    if(t->get_tid()==INVALID_TID){
        return mkfail();
    }

    // ② rip、rsp 必须在内核地址空间
    if(!is_addr_kernel_address((void*)t->priv_ctx.core_ctx.idtctx.iret.rip)||
       !is_addr_kernel_address((void*)t->priv_ctx.core_ctx.idtctx.iret.rsp)){
        return mkfail();
    }

    // ③ 初始上下文必须是从内核态开始的 priv 上下文
    if(t->choose!=task::ctx_choose::priv){
        return mkfail();
    }

    // ④ 目标处理器调度器
    per_processor_scheduler*target=get_other_scheduler(pid);

    // ⑤ 状态机：init → ready
    {
        reentrant_spinlock_guard g(t->task_lock);
        if(!t->set_ready()){
            return mkfail();
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
        kthread_common_save(context,true);
        yield_task->set_ready();
    }
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
            kthread_common_save(frame,true);
        }
        interrupted_task->set_ready();
    }
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
        kthread_common_save(context,true);
        exit_task->set_zombie();
    }
    scheduler.next_task_with_routine();
}
[[noreturn]] void kthread_self_blocked_cppenter(x64_standard_context_v2* context)
{
    per_processor_scheduler&scheduler=*get_self_scheduler();
    task* blocked_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(blocked_task->task_lock);
        kthread_common_save(context,true);
        blocked_task->set_blocked();
    }
    scheduler.next_task_with_routine();
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
    per_processor_scheduler*target_scheduler=get_other_scheduler(task_ptr->belonged_processor_id);
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
[[noreturn]] void kthread_sleep_cppenter(x64_standard_context_v2*context)
{
    per_processor_scheduler&scheduler=get_other_scheduler(fast_get_processor_id());
    task* sleeper_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        reentrant_spinlock_guard g(sleeper_task->task_lock);
        kthread_common_save(context,true);
        sleeper_task->min_wakeup_stamp=ktime::get_microsecond_stamp()+context->rdi;
        sleeper_task->set_blocked();
    }
    {
        reentrant_spinlock_guard h(scheduler.sched_lock);
        scheduler.sleep_queue.insert(sleeper_task);
    }
    scheduler.next_task_with_routine();
}
void block_if_equal_cppenter(x64_standard_context_v2 *context)
{
    per_processor_scheduler&scheduler=get_other_scheduler(fast_get_processor_id());
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
        scheduler.next_task_with_routine();
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