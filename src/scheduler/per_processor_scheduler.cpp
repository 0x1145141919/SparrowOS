#include "Scheduler/per_processor_scheduler.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "util/kout.h"
#include "firmware/ACPI_APIC.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "panic.h"
static u64ka g_next_tid{0};  // 0 保留作无效

extern "C" void secure_hlt();
static void* secure_hlt_wrapper(void* unused) {
    (void)unused;
    secure_hlt();
    return nullptr;
}

// ── task_pool 静态成员 ──
spinrwlock_cpp_t task_pool::lock;
Ktemplats::RBTree<task*, task_tid_compare> task_pool::m_tree;

int task_pool::Init()
{
    return OS_SUCCESS;
}

task* task_pool::get_by_tid(uint64_t tid, KURD_t& kurd)
{
    task tmp;
    tmp.usage_of_search_set_tid(tid);
    task** found;
    {
        spinrwlock_interrupt_about_read_guard l(lock);
        found = m_tree.find(&tmp);
    }
    if (found) return *found;
    kurd = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                  Scheduler::scheduler_task_pool, 0, level_code::ERROR, err_domain::CORE_MODULE);
    return nullptr;
}

KURD_t task_pool::insert(task* t)
{
    if (!t) {
        return KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                      Scheduler::scheduler_task_pool, 0, level_code::ERROR, err_domain::CORE_MODULE);
    }
    {
        spinrwlock_interrupt_about_write_guard l(lock);
        bool ok = m_tree.insert(t);
        if (!ok) {
            return KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                          Scheduler::scheduler_task_pool, 0, level_code::ERROR, err_domain::CORE_MODULE);
        }
    }
    return KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
                  Scheduler::scheduler_task_pool, 0, level_code::INFO, err_domain::CORE_MODULE);
}

KURD_t task_pool::release(uint64_t tid)
{
    spinrwlock_interrupt_about_write_guard l(lock);
    task tmp;
    tmp.usage_of_search_set_tid(tid);
    task** found = m_tree.find(&tmp);
    if (!found) {
        return KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                      Scheduler::scheduler_task_pool, 0, level_code::ERROR, err_domain::CORE_MODULE);
    }
    m_tree.erase(*found);
    return KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
                  Scheduler::scheduler_task_pool, 0, level_code::INFO, err_domain::CORE_MODULE);
}

// ── sleep_queue_t::insert（未改动，仅移至此位置）──
KURD_t per_processor_scheduler::sleep_queue_t::insert(task* task_ptr)
{
    KURD_t success = KURD_t(result_code::SUCCESS, 0, module_code::SCHEDULER,
                             Scheduler::self_scheduler, Scheduler::self_scheduler_events::insert_ready_task, level_code::INFO,
                             err_domain::CORE_MODULE);
    KURD_t fail = KURD_t(result_code::FAIL, 0, module_code::SCHEDULER,
                          Scheduler::self_scheduler, Scheduler::self_scheduler_events::insert_ready_task, level_code::ERROR,
                          err_domain::CORE_MODULE);
    if (task_ptr == nullptr) {
        fail.reason = Scheduler::self_scheduler_events::sleep_task_insert_results::fail_reasons::null_task_ptr;
        return fail;
    }

    node* n = alloc_node(task_ptr);

    if (!m_head) {
        m_head = m_tail = n;
        ++m_size;
        return success;
    }

    const miusecond_time_stamp_t new_stamp = task_ptr->sleep_wakeup_stamp;
    node* cur = m_head;
    while (cur) {
        task* cur_task = cur->value;
        if (cur_task && cur_task->sleep_wakeup_stamp > new_stamp) {
            break;
        }
        cur = cur->next;
    }

    if (!cur) {
        n->prev = m_tail;
        m_tail->next = n;
        m_tail = n;
        ++m_size;
        return success;
    }

    if (cur == m_head) {
        n->next = m_head;
        m_head->prev = n;
        m_head = n;
        ++m_size;
        return success;
    }

    n->next = cur;
    n->prev = cur->prev;
    cur->prev->next = n;
    cur->prev = n;
    ++m_size;
    return success;
}
task::task()
{
    ksetmem_8(this, 0, sizeof(task));
}
// ── per_processor_scheduler 构造函数 ──
per_processor_scheduler::per_processor_scheduler()
{
    KURD_t kurd;
    if(error_kurd(kurd)){
        Panic::panic(default_panic_behaviors_flags,"stack alloc failed when alloc per_processor_scheduler private stack",nullptr,nullptr,kurd);
    }
    // Idle task must not be enqueued into ready_queue.
    kthread_context* idle_ctx = new kthread_context();
    idle_ctx->regs.iret_complex.cs = K_cs_idx<<3;
    idle_ctx->regs.iret_complex.ss = K_ds_ss_idx<<3;
    idle_ctx->regs.iret_complex.rip = (uint64_t)secure_hlt_wrapper;
    idle_ctx->regs.rsi = 0;
    idle_ctx->regs.rdi = 0;
    idle_ctx->stacksize = 0x2000;
    idle_ctx->stack_bottom = (uint64_t)stack_alloc(&kurd,1);
    idle_ctx->regs.iret_complex.rsp = idle_ctx->stack_bottom;
    idle_ctx->regs.iret_complex.rflags = INIT_DEFAULT_RFLAGS;
    if(error_kurd(kurd) || idle_ctx->stack_bottom == 0){
        Panic::panic(default_panic_behaviors_flags,"idle task stack alloc failed",nullptr,nullptr,kurd);
    }
    task* idle_task = new task(task_type_t::kthreadm, idle_ctx);
    idle_task->task_lock.lock();
    uint64_t idle_tid = task_pool::alloc(idle_task, kurd);
    if(error_kurd(kurd) || idle_tid==INVALID_TID){
        idle_task->task_lock.unlock();
        Panic::panic(default_panic_behaviors_flags,"alloc idle task failed",nullptr,nullptr,kurd);
    }
    idle_task->assign_valid_tid(idle_tid);
    idle_task->set_ready();
    idle_task->set_belonged_processor_id(fast_get_processor_id());
    idle_task->task_lock.unlock();
    idle = idle_task;
}
per_processor_scheduler::~per_processor_scheduler()
{
}
namespace {
constexpr uint64_t kthread_yield_saved_stack_delta = 16 * sizeof(uint64_t);
constexpr uint32_t invalid_task_id = ~0u;

static inline KURD_t self_scheduler_default_kurd()
{
    return KURD_t(0, 0, module_code::SCHEDULER, Scheduler::self_scheduler, 0, 0, err_domain::CORE_MODULE);
}

static inline KURD_t make_self_scheduler_fatal(uint8_t event_code, uint16_t reason)
{
    KURD_t kurd = self_scheduler_default_kurd();
    kurd.event_code = event_code;
    kurd.reason = reason;
    return set_fatal_result_level(kurd);
}

static inline void panic_with_kurd(x64_standard_context *frame, KURD_t kurd)
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
    return KURD_t(0,0,module_code::SCHEDULER,Scheduler::self_scheduler,0,0,err_domain::CORE_MODULE);
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
        if(candidate_task->sleep_wakeup_stamp<=current_stamp){
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
            if(global_schedulers+i==this)continue;
            per_processor_scheduler&other=*(global_schedulers+i);
            {
            reentrant_spinlock_guard g(other.sched_lock);
                if(other.ready_queue.size()){
                    task**candidate=other.ready_queue.front();
                    if(*candidate){
                    other.ready_queue.pop_front();
                    return *candidate;
                    }
                } 
            }
        }
        return this->idle;
    }();
    to_run->task_lock.lock();
    to_run->set_running();
    to_run->set_belonged_processor_id(fast_get_processor_id());
    to_run->lastest_run_stamp=ktime::get_microsecond_stamp();
    gs_u64_write(PROCESSOR_NOW_RUNNING_TASK_GS_INDEX,(uint64_t)to_run);
    to_run->task_lock.unlock();
    ktime::heart_beat_alarm::set_clock_by_offset(20000);
    to_run->atomic_load();
}
KURD_t per_processor_scheduler::insert_ready_task(task *task_ptr, bool front)
{
    KURD_t fail=default_fail();
    KURD_t success=default_success();
    fail.event_code=Scheduler::self_scheduler_events::insert_ready_task;
    success.event_code=Scheduler::self_scheduler_events::insert_ready_task;
    if(task_ptr==idle){
        return success;
    }
    if(task_ptr==nullptr){
        fail.reason=Scheduler::self_scheduler_events::insert_ready_task_results::fail_reasons::null_task_ptr;
        return fail;
    }
    if(task_ptr->get_state()!=ready){
        fail.reason=Scheduler::self_scheduler_events::insert_ready_task_results::fail_reasons::bad_task_type;
        return fail;
    }
    if(front){
        if(!ready_queue.push_front(task_ptr)){
            fail.reason=Scheduler::self_scheduler_events::insert_ready_task_results::fail_reasons::insert_fail;
            return fail;
        }
    }else{
        if(!ready_queue.push_back(task_ptr)){
            fail.reason=Scheduler::self_scheduler_events::insert_ready_task_results::fail_reasons::insert_fail;
            return fail;
        }
    }
    return success;
}

uint64_t task::get_tid()
{
    return tid;
}
bool task::usage_of_search_set_tid(uint64_t new_tid)
{
    if(this->current_event_start_stamp)//标准初始化时序必然会初始化这个字段
    return false;
    this->tid=new_tid;
    return true;
}

task_type_t task::get_task_type()
{
    return task_type;
}

uint32_t task::get_belonged_processor_id()
{
    return belonged_processor_id;
}

extern "C" void atoimc_kthread_load(x64_standard_context* context);
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
        this->accumulates_bank[this->current_event]+=elapse;
        this->current_event_start_stamp=now_stamp;
        this->current_event=new_event;
    }
}
task *task::basic_constructor()
{
    task* t=new task();
    placement_constructor(t);
    task_pool::insert(t);
    return t;
}
void task::placement_constructor(task *task_ptr)
{
    task_ptr->task_state=task_state_t::init;
    task_ptr->current_event=event_type_t::init;
    task_ptr->current_event_start_stamp=ktime::get_microsecond_stamp();
    task_ptr->tid=g_next_tid.load();
    g_next_tid.add_ka(1);
}
task_state_t task::get_state()
{
    return task_state;
}
void task::set_belonged_processor_id(uint32_t pid)
{
    belonged_processor_id=pid;
}
void task::atomic_load()
{
    switch(task_type){
        case task_type_t::kthreadm:{
            atoimc_kthread_load(&context.kthread->regs);
        }
        case task_type_t::userthread:{

        }
        case task_type_t::vCPU:{

        }
        default:{
            //特殊kurd
            //return KURD_t(0,0,module_code::SCHEDULER,Scheduler::scheduler_task_pool,0,0,err_domain::CORE_MODULE);
        }
    }
}
void tid_wait_queue::wakeup_all()
{
    for(uint64_t i=0;i<this->size();i++){
        uint64_t tid=this->pop_front_value();
        wakeup_thread(tid,this->m_insert_front);
        //这里要把返回的uint64_t转成KURD并且对于
        //Scheduler::self_scheduler_events::wake_up_kthread_results::success_reasons::already_wakeup_or_running;
        //和Scheduler::self_scheduler_events::wake_up_kthread_results::fail_reasons::bad_task_state;
        //要发起panic
        //或者改成手动展开定制唤醒逻辑
    }
}
