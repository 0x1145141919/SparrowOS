/* ============================================================================
 * 调度器中断入口 — 执行流纪律
 *
 * 本文件的所有 kthread_*_cppenter / block_*_cppenter 函数都是中断入口。
 * 它们最终调用 scheduler.sched() 切换执行流，调用后永不返回（context switch
 * 通过 atomic_load 跳到新任务的 iretq 恢复点）。
 *
 * ── scheduler.sched() ──
 *   执行流飞走性调用。取下一个任务 → atomic_load → iretq 跳到新任务。
 *   调用前必须释放所有 RAII 锁守卫（spinlock_guard / spinlock_interrupt_about_guard），
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
#include "util/wraith_probe.h"   // WRAITH 首爆取证：帧自证 / 留证环
#include "Scheduler/sched_handoff.h" // [FIX-F3/F4/F5] 跨核 handoff 登记 / 调度门
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
        kurd_get_raw(kurd)
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
        kurd_get_raw(kurd)
    );
}
} // namespace
void kthread_common_save(x64_standard_context_v2*frame,bool expect_running,task* task_ptr)
{
    if (!task_ptr) {
        KURD_t fatal = make_kthreads_fatal(
            Scheduler::KTHREADS_EVENTS::EVENT_CODE_KTHREAD_COMMON_SAVE,
            Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::NULL_RUNNING_TASK);
        WRAITH_LOG("KS0 null-task pid=%u cs=%llx rip=%llx rsp=%llx gs=%llx\n",
            (unsigned)fast_get_processor_id(),
            (unsigned long long)frame->core_ctx.idtctx.iret.cs,
            (unsigned long long)frame->core_ctx.idtctx.iret.rip,
            (unsigned long long)frame->core_ctx.idtctx.iret.rsp,
            (unsigned long long)wraith::gs_now());
        panic_with_kurd(frame, fatal, (char*)"kthread_common_save: null running task");
    }
    if (expect_running && task_ptr->get_state() != task_state_t::running) {
        KURD_t fatal = make_kthreads_fatal(
            Scheduler::KTHREADS_EVENTS::EVENT_CODE_KTHREAD_COMMON_SAVE,
            Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::BAD_TASK_STATE);
        WRAITH_LOG("KS1 bad-state pid=%u tsk=%llx cs=%llx rip=%llx rsp=%llx gs=%llx\n",
            (unsigned)fast_get_processor_id(),
            (unsigned long long)(uint64_t)task_ptr,
            (unsigned long long)frame->core_ctx.idtctx.iret.cs,
            (unsigned long long)frame->core_ctx.idtctx.iret.rip,
            (unsigned long long)frame->core_ctx.idtctx.iret.rsp,
            (unsigned long long)wraith::gs_now());
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
                // 栈指针越界 = 上下文已被踩（WRAITH 受害链常见首爆）——先落证再 panic。
                WRAITH_LOG("KS2 STACK-OOR pid=%u tsk=%llx rsp=%llx sbase=%llx spg=%u top=%llx bot=%llx rip=%llx cs=%llx gs=%llx\n",
                    (unsigned)fast_get_processor_id(),
                    (unsigned long long)(uint64_t)task_ptr,
                    (unsigned long long)rsp,
                    (unsigned long long)stack_top,
                    (unsigned)task_ptr->priv_stack_pages,
                    (unsigned long long)stack_top,
                    (unsigned long long)stack_bottom,
                    (unsigned long long)frame->core_ctx.idtctx.iret.rip,
                    (unsigned long long)frame->core_ctx.idtctx.iret.cs,
                    (unsigned long long)wraith::gs_now());
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
        spinlock_interrupt_about_guard g(t->task_lock);
        if(!t->set_ready()){
            KURD_t k = mkfail();
            k.reason = fr::STATE_TRANSITION_FAIL;
            return k;
        }
    }

    // ⑥ 插入目标 ready_queue
    KURD_t kurd;
    {
        spinlock_interrupt_about_guard g(target->sched_lock);
        kurd=target->insert_ready_task(t,false);
    }

    return kurd;
}
[[noreturn]] void kthread_yield_true_enter(x64_standard_context_v2*context)
{

    per_processor_scheduler&scheduler=*get_self_scheduler();
    task* yield_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        spinlock_interrupt_about_guard g(yield_task->task_lock);
        kthread_common_save(context,true,yield_task);
        if (!yield_task->set_ready())
            panic_with_kurd(context, make_kthreads_set_state_fatal());
    }
    if(!scheduler.is_the_idle_task(yield_task))
    {
        spinlock_interrupt_about_guard g(scheduler.sched_lock);
        scheduler.insert_ready_task(yield_task);
    }
    scheduler.next_task_with_routine();
}
extern "C" void resched(x64_standard_context_v2 *frame)
{
    const uint32_t sched_cpu = fast_get_processor_id();
    // [FIX-F3 / MS-2/14] 每核调度不可重入门：本核已在调度中（next_task_with_routine/sched
    // 尚未提交切换）→ 跳过本次 resched，直接返回让【进行中的】那次调度在 sched() 提交
    // （iretq 飞走）时完成切换。
    //   · 不丢调度：进行中的调度会切走到某个任务；本次中断里的设备处理（唤醒/入队）
    //     已经在其 handler 里完成，被唤醒的任务已在 ready 队列，下一次调度必然取到。
    //   · 同核重入本被中断门(IF=0)挡住，此门为防御性双保险。
    if (sched_cpu < MAX_PROCESSORS_COUNT && g_cpu_in_sched[sched_cpu]) {
        WRAITH_TRACE("R2 resched-skip pid=%u rsp=%llx gs=%llx tsk=%llx\n",
            (unsigned)sched_cpu,
            (unsigned long long)wraith::rsp_now(),
            (unsigned long long)wraith::gs_now(),
            (unsigned long long)wraith::now_running_task());
        return;
    }
    if (sched_cpu < MAX_PROCESSORS_COUNT) g_cpu_in_sched[sched_cpu] = 1;
    // ── WRAITH 首爆取证：中断内嵌套调度的「首入口」指纹 ──
    // 报告 w22：NVMe CQ 中断 → idt_vec_demux_entry(默认分支) → resched(raw_frame)。
    // 记录被中断上下文的 iret.cs/rip/rsp + 当前 rsp/gs，并断言 iret.cs 合法；
    // 另给「帧内 rsp / 当前 rsp 是否落在当前 task 私栈」自证位（栈被踩的自证指纹）。
    const uint64_t rcs   = frame->core_ctx.idtctx.iret.cs;
    const uint64_t rlo   = rcs & 0x3ULL;
    const uint64_t rrip  = frame->core_ctx.idtctx.iret.rip;
    const uint64_t rirsp = frame->core_ctx.idtctx.iret.rsp;
    const uint64_t cur_rsp = wraith::rsp_now();
    task* const cur_task = (task*)wraith::now_running_task();
    uint64_t sb = 0; uint32_t sp = 0;
    if (cur_task && (uint64_t)cur_task >= 0xFFFF800000000000ULL) {
        sb = cur_task->priv_stack_base;
        sp = cur_task->priv_stack_pages;
    }
    WRAITH_TRACE("R0 resched pid=%u cs=%llx rip=%llx irsp=%llx rsp=%llx gs=%llx tsk=%llx sbase=%llx spg=%u irsp_in=%u rsp_in=%u\n",
        (unsigned)fast_get_processor_id(),
        (unsigned long long)rcs,
        (unsigned long long)rrip,
        (unsigned long long)rirsp,
        (unsigned long long)cur_rsp,
        (unsigned long long)wraith::gs_now(),
        (unsigned long long)(uint64_t)cur_task,
        (unsigned long long)sb, (unsigned)sp,
        (unsigned)wraith::in_range(rirsp, sb, sp),
        (unsigned)wraith::in_range(cur_rsp, sb, sp));
    if (rlo != 0 && rlo != 3) {
        // iret.cs 低位既非内核(0) 也非用户(3) ⇒ 中断帧已被踩坏（w22 首爆的形态）。
        // 不修时序，只把「本该静默地 iretq 回野帧」变成「留证 + panic」。
        WRAITH_LOG("R1 BAD-FRAME-CS pid=%u cs=%llx lo=%llx rip=%llx irsp=%llx rsp=%llx gs=%llx tsk=%llx\n",
            (unsigned)fast_get_processor_id(),
            (unsigned long long)rcs, (unsigned long long)rlo,
            (unsigned long long)rrip,
            (unsigned long long)rirsp,
            (unsigned long long)cur_rsp,
            (unsigned long long)wraith::gs_now(),
            (unsigned long long)(uint64_t)cur_task);
        panic_with_kurd(frame,
            make_kthreads_fatal(
                Scheduler::KTHREADS_EVENTS::EVENT_CODE_KTHREAD_COMMON_SAVE,
                Scheduler::KTHREADS_EVENTS::COMMON_FATAL_REASONS::BAD_TASK_STATE),
            (char*)"resched: iret.cs corrupted (interrupt frame clobbered)");
    }
    per_processor_scheduler&scheduler=*get_self_scheduler();
    task* interrupted_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    bool is_user_context=((frame->core_ctx.idtctx.iret.cs&3)==3);
    {
        spinlock_interrupt_about_guard g(interrupted_task->task_lock);
        if(!is_user_context){
            kthread_common_save(frame,true,interrupted_task);
        }
        if (!interrupted_task->set_ready())
            panic_with_kurd(frame, make_kthreads_set_state_fatal());
    }
    if(!scheduler.is_the_idle_task(interrupted_task))
    {
        spinlock_interrupt_about_guard g(scheduler.sched_lock);
        scheduler.insert_ready_task(interrupted_task);
    }
    scheduler.next_task_with_routine();
}
[[noreturn]] void kthread_exit_cppenter(x64_standard_context_v2*context) 
{
    per_processor_scheduler&scheduler=*get_self_scheduler();
    task*exit_task=(task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);
    {
        spinlock_interrupt_about_guard g(exit_task->task_lock);
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
        spinlock_interrupt_about_guard g(blocked_task->task_lock);
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
    spinlock_interrupt_about_guard l(task_ptr->task_lock);
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
        // [FIX-F4 / MS-4] 仍在别核执行（未真正切离本栈）→ 延后唤醒，不 set_ready/入队。
        // 拥有核会在 sched() 交接点补投（见 per_processor_scheduler::sched）。
        if (sched_owner_cpu(task_ptr) >= 0) {
            task_ptr->wake_pending = true;
            task_ptr->wake_pending_rax = task_ptr->priv_ctx.rax;
            success.reason=ev::wakeup_thread_results::SUCCESS_REASONS::ALREADY_RUNNING_OR_WAKEUP;
            return kurd_get_raw(success);
        }
        if (!task_ptr->set_ready())
            panic_with_kurd(make_kthreads_set_state_fatal());
        {
            spinlock_interrupt_about_guard h(target_scheduler->sched_lock);
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
        spinlock_interrupt_about_guard g(sleeper_task->task_lock);
        kthread_common_save(context,true,sleeper_task);
        sleeper_task->min_wakeup_stamp=ktime::get_microsecond_stamp()+context->rdi;
        if (!sleeper_task->set_blocked())
            panic_with_kurd(context, make_kthreads_set_state_fatal());
        sleeper_task->on_blockers_queue_bit = true;
        sleeper_task->task_event_shift( task::event_type_t::sleep);
        {
        spinlock_interrupt_about_guard h(scheduler->sched_lock);
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
            spinlock_interrupt_about_guard h(blocked_task->task_lock);
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
    // [FIX-F5 / MS-3] 退出者可能仍在本核/别核的栈上跑完 next_task_with_routine→sched（
    // kthread_exit 先 set_zombie 发布、之后才切走）。只要它还登记在任一核 g_cpu_running[]，
    // 就绝不能被回收其内核栈（否则栈 UAF / 物理页被二次分配为另一任务的栈）。
    // 拥有核的 sched() 交接点必然把它换出登记，且不需要本核做任何事（不会死锁）→ 有界自旋等待。
    if (sched_owner_cpu(t) >= 0) {
        for (uint32_t relax = 0; relax < 2000000u; ++relax) {
            if (sched_owner_cpu(t) < 0) break;
            asm volatile("pause" ::: "memory");
        }
    }
    if (sched_owner_cpu(t) >= 0) {
        // 极端情况仍未切离（不应发生）：本轮不释放，返回可重试——绝不在其仍占栈时 vfree。
        fail.reason = ev::release_kthread_results::FAIL_REASONS::TASK_STILL_ON_CPU;
        return kurd_get_raw(fail);
    }
    k=__wrapped_pgs_vfree((void*)t->priv_stack_base,t->priv_stack_pages);
    if(error_kurd(k))return kurd_get_raw(k);
    task_pool::release(tid);
    return kurd_get_raw(success);
}
