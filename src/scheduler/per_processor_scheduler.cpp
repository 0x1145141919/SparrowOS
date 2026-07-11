#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "Scheduler/per_processor_scheduler.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "util/kout.h"
#include "firmware/ACPI_APIC.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "panic.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/mem_init.h"
#include "util/rb_map.h"
#include "Scheduler/task_pool.h"
#include "Scheduler/bq_system.h"
#include "Scheduler/kthread_abi.h"
extern rb_map<bq_id_t, block_queue*>* container;
extern spinrwlock_cpp_t container_lock;
extern uint64_t next_will_alloc_qid;
#include "arch/x86_64/mem_init.h"
static u64ka g_next_tid{0};  // 0 保留作无效

extern "C" void secure_hlt();
static void* secure_hlt_wrapper(void* unused) {
    (void)unused;
    secure_hlt();
    return nullptr;
}

// ── task_pool 静态成员 ──
spinrwlock_cpp_t task_pool::lock;
Ktemplats::RBTree<task, task_tid_compare> task_pool::m_tree;

int task_pool::Init()
{
    return OS_SUCCESS;
}

task* task_pool::get_by_tid(uint64_t tid, KURD_t& kurd)
{
    task tmp;
    tmp.tid=tid;
    task* found;
    {
        spinrwlock_interrupt_about_read_guard l(lock);
        found = m_tree.find(tmp);
    }
    if (found) return found;
    kurd = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                  Scheduler::TASK_POOL,
                  Scheduler::TASK_POOL_EVENTS::EVENT_CODE_GET_BY_TID,
                  level_code::ERROR, err_domain::CORE_MODULE);
    kurd.reason = Scheduler::TASK_POOL_EVENTS::COMMON_FAIL_REASONS::NOT_FOUND;
    return nullptr;
}

task* task_pool::spawn()
{
    task tmp;
    tmp.tid=g_next_tid.add_ka(1);
    m_tree.insert(tmp);
    return m_tree.find( tmp);
}

KURD_t task_pool::release(uint64_t tid)
{
    spinrwlock_interrupt_about_write_guard l(lock);
    task tmp;
    tmp.tid=tid;
    task* found = m_tree.find(tmp);
    if (!found) {
        KURD_t k = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                          Scheduler::TASK_POOL,
                          Scheduler::TASK_POOL_EVENTS::EVENT_CODE_RELEASE,
                          level_code::ERROR, err_domain::CORE_MODULE);
        k.reason = Scheduler::TASK_POOL_EVENTS::COMMON_FAIL_REASONS::NOT_FOUND;
        return k;
    }
    m_tree.erase(tmp);
    return KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
                  Scheduler::TASK_POOL,
                  Scheduler::TASK_POOL_EVENTS::EVENT_CODE_RELEASE,
                  level_code::INFO, err_domain::CORE_MODULE);
}

// ── sleep_queue_t::insert ──
// 不产生 KURD 语义，仅返回空 KURD_t。
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
task::task()
{
    ksetmem_8(this, 0, sizeof(task));
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
bool task::usage_of_search_set_tid(uint64_t new_tid)
{
    if(this->current_event_start_stamp)//标准初始化时序必然会初始化这个字段
    return false;
    this->tid=new_tid;
    return true;
}

extern "C" void idt_style_load(x64_standard_context_v2* context);
extern "C" void fred_uctx_load(x64_standard_context_v2* context);
extern "C" void fred_pctx_load(x64_standard_context_v2* context);
bool task::set_ready()
{
    if(task_state==task_state_t::init ||
       task_state==task_state_t::blocked ||
       task_state==task_state_t::running){
        task_state=task_state_t::ready;
        return true;
    }
    return false;
}
bool task::set_blocked()
{
    if(task_state==task_state_t::running){
        task_state=task_state_t::blocked;
        return true;
    }
    return false;
}
bool task::set_dead()
{
    if(task_state==task_state_t::zombie){
        task_state=task_state_t::dead;
        return true;
    }
    return false;
}
bool task::set_zombie()
{
    if(task_state==task_state_t::running || task_state==task_state_t::blocked || task_state==task_state_t::ready){
        task_state=task_state_t::zombie;
        return true;
    }
    return false;
}
bool task::set_running()
{
    if(task_state==task_state_t::ready){
        this->task_state=running;
        return true;
    }
    return false;
}
void task::task_event_shift(event_type_t new_event)
{
    if(new_event==this->current_event){

    }else{
        miusecond_time_stamp_t now_stamp=ktime::get_microsecond_stamp();
        uint64_t elapse=now_stamp-this->current_event_start_stamp;
        this->accumulates_time_bank[this->current_event]+=elapse;
        this->accumulates_counters_bank[this->current_event]++;
        this->current_event_start_stamp=now_stamp;
        this->current_event=new_event;
    }
}

task *task::basic_constructor()
{
    task* t=task_pool::spawn();
    ksetmem_8(t,0,sizeof(task));
    t->task_state=task_state_t::init;
    t->current_event=event_type_t::init;
    t->current_event_start_stamp=ktime::get_microsecond_stamp();
    return t;
}
void task::idle_specified_constructor(task *task_ptr)
{
    task_ptr->task_state=task_state_t::init;
    task_ptr->current_event=event_type_t::init;
    task_ptr->current_event_start_stamp=ktime::get_microsecond_stamp();
    task_ptr->tid=g_next_tid.add_ka(1);
}
task_state_t task::get_state()
{
    return task_state;
}
void per_processor_scheduler::placed_init()
{
    task& t=this->idle;
    task::idle_specified_constructor(&t);
    gs_complex_t* gs_complex=(gs_complex_t*)gs_offsetptr_dumper(0);
    per_processor_hardware_stack_t* stacks_ptr=gs_complex->stacks_ptr;
    t.priv_ctx.core_ctx.idtctx.iret.rip=(uint64_t)&common_idle;
    t.priv_ctx.core_ctx.idtctx.iret.cs=K_cs_idx<<3;
    t.priv_ctx.core_ctx.idtctx.iret.ss=K_ds_ss_idx<<3;
    t.priv_stack_base=(vaddr_t)stacks_ptr->stack_ist4;
    t.priv_stack_pages=sizeof(((per_processor_hardware_stack_t*)0)->stack_ist4)>>12;
    t.priv_ctx.core_ctx.idtctx.iret.rsp=t.priv_stack_base+(t.priv_stack_pages<<12)-64;
    t.priv_ctx.core_ctx.idtctx.iret.rflags=INIT_DEFAULT_RFLAGS;
    t.choose=task::ctx_choose::priv;
}
per_processor_scheduler* global_schedulers = nullptr;

per_processor_scheduler *get_self_scheduler()
{
    return (per_processor_scheduler*)read_gs_u64(PROCESSOR_SCHEDULER_GS_INDEX);
}
void task::atomic_load()//只是忠实地根据翻到的牌子运行,不对任何状态机进行改变(因为需要task锁)
{

    switch(this->choose){
        case ctx_choose::priv:{
            if(fred_support_catch_bit){
                fred_pctx_load(&this->priv_ctx);
            }else{
                idt_style_load(&this->priv_ctx);
            }
        }
        case ctx_choose::u_ctx:{

            //别想着那么快实现,uctx要加载的多得多
        };
        case ctx_choose::vCPU:{

        }
    }
}
ckurd kthread_init(task *t,kthread_creating_package*p)
{

    t->priv_ctx.core_ctx.idtctx.iret.cs=K_cs_idx<<3;
    t->priv_ctx.core_ctx.idtctx.iret.ss=K_ds_ss_idx<<3;
    t->priv_ctx.core_ctx.idtctx.iret.rflags=INIT_DEFAULT_RFLAGS;
    t->priv_ctx.rdi=(uint64_t)p->func_raw;
    t->priv_ctx.rsi=p->args[0];
    t->priv_ctx.rdx=p->args[1];
    t->priv_ctx.rcx=p->args[2];
    t->priv_ctx.r8=p->args[3];
    t->priv_ctx.r9=p->args[4];
    KURD_t kurd;
    if(!t->priv_stack_base)
    {t->priv_stack_base=stack_alloc(&kurd,DEFAULT_PRIVSTACK_PGS_COUNT);
    if(error_kurd(kurd))return kurd_get_raw(kurd);
    t->priv_stack_pages=DEFAULT_PRIVSTACK_PGS_COUNT;}
    t->priv_ctx.core_ctx.idtctx.iret.rsp=t->priv_stack_base+(t->priv_stack_pages<<12)-64;
    t->priv_ctx.core_ctx.idtctx.iret.rip=(uint64_t)&allkthread_true_enter;
    t->choose=task::ctx_choose::priv;
    return ckurd();
}
void per_processor_scheduler::next_task_with_routine()
{
    // 睡眠队列超时唤醒
    sleep_tasks_wake();

    // 调度
    sched();
}
uint64_t zombie_observe(uint64_t tid, zombie_observe_results_t *result)
{
    KURD_t kurd;
    task*t=task_pool::get_by_tid(tid,kurd);
    if(error_kurd((kurd))){
        *result=ZOMBIE_TID_NOT_FOUND;
        return INVALID_TID;
    }
    {
        reentrant_spinlock_guard l(t->task_lock);
        if(t->get_state()!=task_state_t::zombie){
            *result=ZOMBIE_ALIVE;
            return INVALID_TID;
        }else{
            *result=ZOMBIE_DEAD;
            return t->priv_ctx.rdi;
        }
    }

}
per_processor_scheduler *get_other_scheduler(uint32_t pid)
{
    return &global_schedulers[pid];
}
bool task::resurrect()
{
    if(this->task_state!=zombie)return false;

    // 关旧帐（offline/等）→ 开 init 新帐
    task_event_shift(event_type_t::init);

    // 清除非 init 的所有历史记录
    for(uint32_t i=1;i<event_type_COUNT;++i){
        accumulates_time_bank[i]=0;
        accumulates_counters_bank[i]=0;
    }

    task_state=task_state_t::init;
    return true;
}
uint64_t task::get_tid() const
{
    return this->tid;
}bool per_processor_scheduler::is_the_idle_task(task *t)
{
    return t==(&this->idle);
}