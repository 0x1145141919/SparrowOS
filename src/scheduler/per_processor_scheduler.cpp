#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "Scheduler/per_processor_scheduler.h"
#include "Scheduler/kthread_abi.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kout.h"
#include "panic.h"
#include "util/wraith_probe.h"   // WRAITH 首爆取证：自证指纹 / 面包屑 / 留证环
#include "Scheduler/sched_handoff.h" // [FIX-F3/F4/F5] 跨核 handoff 登记 / 调度门
#include "arch/x86_64/mem_init.h"     // logical_processor_count

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

static inline KURD_t make_sched_set_state_fatal()
{
    KURD_t k = scheduler_default_kurd();
    k.event_code = Scheduler::SCHEDULER_EVENTS::EVENT_CODE_SET_STATE;
    k.reason = Scheduler::SCHEDULER_EVENTS::COMMON_FATAL_REASONS::STATE_TRANSITION_FAIL;
    return set_fatal_result_level(k);
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
        kurd_get_raw(kurd)
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
        kurd_get_raw(kurd)
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
    // ── WRAITH 首爆取证探针（只读自证；不改任何时序）──
    // 报告 w13：本函数帧底保存槽 [rbp-0x268] 被「取时帧」覆盖 → 野 this=0x2C700。
    // 这里只增一个局部 oracle（GS 真值），用 this≠get_self_scheduler() 捕捉帧底被踩：
    //   · GS 真值不受任何栈槽污染影响，是最鲁棒的判定；
    //   · 刻意不加多余局部，尽量减少对帧布局的扰动（降低 Heisenbug 风险）。
    per_processor_scheduler* const self_sched = get_self_scheduler();
    WRAITH_TRACE("W0 sleep_wake pid=%u this=%llx self=%llx gs=%llx rsp=%llx\n",
        (unsigned)fast_get_processor_id(),
        (unsigned long long)(uint64_t)this,
        (unsigned long long)(uint64_t)self_sched,
        (unsigned long long)wraith::gs_now(),
        (unsigned long long)wraith::rsp_now());
    if (this != self_sched) {
        // 入口 this 即野（帧底保存槽在进入前已坏，或调用方传错）。
        WRAITH_LOG("W1 WILD-THIS-ENTRY pid=%u this=%llx self=%llx gs=%llx rsp=%llx\n",
            (unsigned)fast_get_processor_id(),
            (unsigned long long)(uint64_t)this,
            (unsigned long long)(uint64_t)self_sched,
            (unsigned long long)wraith::gs_now(),
            (unsigned long long)wraith::rsp_now());
        panic_with_kurd(default_fatal());
    }

    while (true) {
        constexpr uint8_t BATCH_MAX = 64;
        task* batch[BATCH_MAX];
        uint8_t batch_count = 0;
        KURD_t kurd;
        miusecond_time_stamp_t current_stamp = ktime::get_microsecond_stamp();

        {
            if (this != self_sched) {
                // 帧底 [rbp-0x268] 在同一次调用执行途中被覆盖（w13 崩点=line171）。
                // 用 GS 真值 self_sched 判定：不依赖任何栈槽。
                WRAITH_LOG("W2 SLOT-CLOBBER@1 pid=%u this=%llx self=%llx gs=%llx rsp=%llx\n",
                    (unsigned)fast_get_processor_id(),
                    (unsigned long long)(uint64_t)this,
                    (unsigned long long)(uint64_t)self_sched,
                    (unsigned long long)wraith::gs_now(),
                    (unsigned long long)wraith::rsp_now());
                panic_with_kurd(default_fatal());
            }
            spinlock_interrupt_about_guard g(this->sched_lock);
            while (batch_count < BATCH_MAX) {
                task** candidate = this->sleep_queue.front();
                if (!candidate) break;
                task* candidate_task = *candidate;
                if (candidate_task->min_wakeup_stamp > current_stamp) break;
                this->sleep_queue.pop_front();
                batch[batch_count++] = candidate_task;
            }
        }
        if (batch_count == 0) break;

        for (uint8_t i = 0; i < batch_count; i++) {
            spinlock_interrupt_about_guard g(batch[i]->task_lock);
            batch[i]->on_blockers_queue_bit = false;
            if (!batch[i]->set_ready())
                panic_with_kurd(make_sched_set_state_fatal());
        }

        {
            if (this != self_sched) {
                // ← 与 w13 崩溃点（第三条 g(this->sched_lock)）同址。
                WRAITH_LOG("W3 SLOT-CLOBBER@3 pid=%u this=%llx self=%llx gs=%llx rsp=%llx\n",
                    (unsigned)fast_get_processor_id(),
                    (unsigned long long)(uint64_t)this,
                    (unsigned long long)(uint64_t)self_sched,
                    (unsigned long long)wraith::gs_now(),
                    (unsigned long long)wraith::rsp_now());
                panic_with_kurd(default_fatal());
            }
            spinlock_interrupt_about_guard g(this->sched_lock);
            for (uint8_t i = 0; i < batch_count; i++) {
                kurd = this->insert_ready_task(batch[i]);
                if (error_kurd(kurd)) {
                    panic_with_kurd(kurd);
                }
            }
        }
    }
}
void per_processor_scheduler::sched()
{
    // ── WRAITH 首爆取证：入口指纹（连锁的「叶」；resched→next_task→sched）──
    WRAITH_TRACE("S0 sched pid=%u gs=%llx rsp=%llx tsk=%llx\n",
        (unsigned)fast_get_processor_id(),
        (unsigned long long)wraith::gs_now(),
        (unsigned long long)wraith::rsp_now(),
        (unsigned long long)wraith::now_running_task());
    const uint32_t self_cpu = fast_get_processor_id();
    // 即将切离本核的任务——本函数当前使用的内核栈正是它的栈。
    task* prev = (task*)read_gs_u64(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX);

    // [FIX-F4 / MS-1/2/4/5] 认领 to_run 时，跳过「仍登记在其它核 g_cpu_running[]」的任务：
    // 它可能已被唤醒并入队，但拥有核还没真正切离本栈——此刻绝不能立即恢复它。
    // 放回原队列尾延后，由拥有核在交接点换出登记后再认领（见下方交接段）。
    auto try_take = [&](per_processor_scheduler* s)->task* {
        spinlock_interrupt_about_guard g(s->sched_lock);
        size_t n = s->ready_queue.size();
        size_t examined = 0;
        while (examined < n) {
            task** candidate = s->ready_queue.front();
            if (!candidate) { s->ready_queue.pop_front(); ++examined; continue; }
            task* popped = *candidate;
            s->ready_queue.pop_front();
            ++examined;
            if (popped && sched_running_off_cpu(popped, self_cpu)) {
                s->ready_queue.push_back(popped);   // 延后
                continue;
            }
            return popped;
        }
        return nullptr;
    };
    task* to_run=[&]()->task*{
        {
            task* c = try_take(this);
            if (c) return c;
        }
        for(uint64_t i=0;i<logical_processor_count;i++){
            per_processor_scheduler*other=get_other_scheduler(i);
            if(other==this)continue;
            task* c = try_take(other);
            if (c) return c;
        }
        return &this->idle;
    }();
    if (!to_run || (uint64_t)to_run < 0xFFFF800000000000ULL) {
        // 选出的任务是非内核指针（野 ready_queue 项 / 野 scheduler）—— 首爆即留证。
        WRAITH_LOG("S1 BAD-TO-RUN pid=%u to_run=%llx gs=%llx rsp=%llx\n",
            (unsigned)fast_get_processor_id(),
            (unsigned long long)(uint64_t)to_run,
            (unsigned long long)wraith::gs_now(),
            (unsigned long long)wraith::rsp_now());
        panic_with_kurd(default_fatal());
    }
    // [FIX-F2 / MS-6] 在 to_run->task_lock 内快照上下文；释放锁后再落地。
    x64_standard_context_v2 snap;
    {
    spinlock_interrupt_about_guard g1(to_run->task_lock);
    if (!to_run->set_running())
        panic_with_kurd(make_sched_set_state_fatal());
    to_run->belonged_processor_id=self_cpu;
    gs_u64_write(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX,(uint64_t)to_run);
    // [FIX-F4] 任务已被调度执行 → 任何先前被延后的唤醒意图已被“正在运行”满足，清除 pending
    // （状态已是 running，后续唤醒不会再置位，故此处无需额外锁）。
    to_run->wake_pending = false;
    snap = to_run->priv_ctx;   // [FIX-F2/MS-6] 锁内快照（避免与并发 kthread_common_save 撕裂）
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
    // [FIX-F4 / MS-1/2/4/5] 交接点：prev 不再占用本核。在 prev->task_lock 下原子地
    //   (a) 把本核登记由 prev 换成 to_run（= prev 已确实切离本核）；
    //   (b) 若 prev 在“仍在栈上”期间被唤醒过（wake_pending），就地补投到本核 ready 队列。
    // 与唤醒侧的 task_lock 临界区配对：二者互斥，故“置 pending”与“补投”不会漏、不会重。
    // 刻意放在 set_clock_by_offset 之后，使“清登记→iretq”的残留窗口缩到最小。
    ktime::heart_beat_alarm::set_clock_by_offset(20000);
    if (prev && prev != to_run && self_cpu < MAX_PROCESSORS_COUNT) {
        bool enqueue = false;
        {
            spinlock_interrupt_about_guard gp(prev->task_lock);
            g_cpu_running[self_cpu] = to_run;      // 本核登记换人（清掉 prev）
            if (prev->wake_pending) {
                prev->wake_pending = false;
                if (prev->get_state() == task_state_t::blocked) {
                    prev->priv_ctx.rax = prev->wake_pending_rax;
                    if (!prev->set_ready())
                        panic_with_kurd(make_sched_set_state_fatal());
                    enqueue = true;
                }
            }
        }
        if (enqueue) {
            spinlock_interrupt_about_guard gs2(this->sched_lock);
            KURD_t k = this->insert_ready_task(prev, false);
            if (error_kurd(k)) panic_with_kurd(k);
        }
    } else if (self_cpu < MAX_PROCESSORS_COUNT) {
        if ((const task*)g_cpu_running[self_cpu] != to_run)
            g_cpu_running[self_cpu] = to_run;
    }
    // [FIX-F3 / MS-2/14] 提交切换：清本核“正在调度”门（此后本核不再回到本栈）。
    if (self_cpu < MAX_PROCESSORS_COUNT) g_cpu_in_sched[self_cpu] = 0;
    to_run->atomic_load_from(&snap);   // [FIX-F2] 用锁内快照落地
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
    if (!t.set_ready())
        panic_with_kurd(make_sched_set_state_fatal());
}
per_processor_scheduler* global_schedulers = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
// [FIX-F3/F4/F5] 跨核 handoff 安全登记 + 每核调度门
//   依据样本：w13 `0x2C700` / wh02 双核同爆 / wl02 `this=0`
//   报告：analysis/WRAITH_multischedule_review.md §0/§4.1/§4.3；WRAITH_static_review.md §2.A
// ═══════════════════════════════════════════════════════════════════════════
task* volatile g_cpu_running[MAX_PROCESSORS_COUNT];
volatile uint8_t g_cpu_in_sched[MAX_PROCESSORS_COUNT];
uint32_t g_sched_ncpu = 0;

void sched_handoff_init()
{
    uint32_t n = logical_processor_count;
    if (n == 0 || n > MAX_PROCESSORS_COUNT) n = MAX_PROCESSORS_COUNT;
    g_sched_ncpu = n;
    for (uint32_t i = 0; i < MAX_PROCESSORS_COUNT; ++i) {
        g_cpu_running[i] = nullptr;
        g_cpu_in_sched[i] = 0;
    }
}

int sched_owner_cpu(const task* t)
{
    if (!t) return -1;
    // 未初始化时退化为全界扫描（保证正确性优先于性能）。
    const uint32_t n = g_sched_ncpu ? g_sched_ncpu : MAX_PROCESSORS_COUNT;
    for (uint32_t i = 0; i < n; ++i) {
        if ((const task*)g_cpu_running[i] == t) return (int)i;
    }
    return -1;
}

bool sched_running_off_cpu(const task* t, uint32_t self_cpu)
{
    if (!t) return false;
    const uint32_t n = g_sched_ncpu ? g_sched_ncpu : MAX_PROCESSORS_COUNT;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == self_cpu) continue;
        if ((const task*)g_cpu_running[i] == t) return true;
    }
    return false;
}

per_processor_scheduler *get_self_scheduler()
{
    return (per_processor_scheduler*)read_gs_u64(PROCESSOR_SCHEDULER_GS_INDEX);
}


void per_processor_scheduler::next_task_with_routine()
{
    // ── WRAITH 首爆取证：this 自证（GS 真值 oracle）──
    // 调用者恒为「本核调度器」；this≠get_self_scheduler() 即帧/入参被踩。
    per_processor_scheduler* const self_sched = get_self_scheduler();
    if (this != self_sched) {
        WRAITH_LOG("N1 WILD-THIS pid=%u this=%llx self=%llx gs=%llx rsp=%llx\n",
            (unsigned)fast_get_processor_id(),
            (unsigned long long)(uint64_t)this,
            (unsigned long long)(uint64_t)self_sched,
            (unsigned long long)wraith::gs_now(),
            (unsigned long long)wraith::rsp_now());
        panic_with_kurd(default_fatal());
    }
    // [FIX-F3 / MS-2/14] 进入调度：置本核「正在调度」门。门在 sched() 提交切换前清除；
    // 期间 resched 见门即跳过（不嵌套）。
    {
        const uint32_t c = fast_get_processor_id();
        if (c < MAX_PROCESSORS_COUNT) g_cpu_in_sched[c] = 1;
    }
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