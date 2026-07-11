#include "Scheduler/task.h"
#include "Scheduler/task_pool.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "ktime.h"

extern u64ka g_next_tid;

extern "C" void idt_style_load(x64_standard_context_v2* context);
extern "C" void fred_uctx_load(x64_standard_context_v2* context);
extern "C" void fred_pctx_load(x64_standard_context_v2* context);

task::task()
{
    ksetmem_8(this, 0, sizeof(task));
}

bool task::usage_of_search_set_tid(uint64_t new_tid)
{
    if (this->current_event_start_stamp)
        return false;
    this->tid = new_tid;
    return true;
}

bool task::set_ready()
{
    if (task_state == task_state_t::init ||
        task_state == task_state_t::blocked ||
        task_state == task_state_t::running) {
        task_state = task_state_t::ready;
        return true;
    }
    return false;
}

bool task::set_blocked()
{
    if (task_state == task_state_t::running) {
        task_state = task_state_t::blocked;
        return true;
    }
    return false;
}

bool task::set_dead()
{
    if (task_state == task_state_t::zombie) {
        task_state = task_state_t::dead;
        return true;
    }
    return false;
}

bool task::set_zombie()
{
    if (task_state == task_state_t::running ||
        task_state == task_state_t::blocked ||
        task_state == task_state_t::ready) {
        task_state = task_state_t::zombie;
        return true;
    }
    return false;
}

bool task::set_running()
{
    if (task_state == task_state_t::ready) {
        this->task_state = running;
        return true;
    }
    return false;
}

void task::task_event_shift(event_type_t new_event)
{
    if (new_event == this->current_event) {
    } else {
        miusecond_time_stamp_t now_stamp = ktime::get_microsecond_stamp();
        uint64_t elapse = now_stamp - this->current_event_start_stamp;
        this->accumulates_time_bank[this->current_event] += elapse;
        this->accumulates_counters_bank[this->current_event]++;
        this->current_event_start_stamp = now_stamp;
        this->current_event = new_event;
    }
}

task* task::basic_constructor()
{
    task* t = task_pool::spawn();
    ksetmem_8(t, 0, sizeof(task));
    t->task_state = task_state_t::init;
    t->current_event = event_type_t::init;
    t->current_event_start_stamp = ktime::get_microsecond_stamp();
    return t;
}

void task::idle_specified_constructor(task* task_ptr)
{
    task_ptr->task_state = task_state_t::init;
    task_ptr->current_event = event_type_t::init;
    task_ptr->current_event_start_stamp = ktime::get_microsecond_stamp();
    task_ptr->tid = g_next_tid.add_ka(1);
}

task_state_t task::get_state()
{
    return task_state;
}

void task::atomic_load()
{
    switch (this->choose) {
    case ctx_choose::priv:
        if (fred_support_catch_bit) {
            fred_pctx_load(&this->priv_ctx);
        } else {
            idt_style_load(&this->priv_ctx);
        }
    case ctx_choose::u_ctx: {
    };
    case ctx_choose::vCPU: {
    }
    }
}

bool task::resurrect()
{
    if (this->task_state != zombie) return false;
    task_event_shift(event_type_t::init);
    for (uint32_t i = 1; i < event_type_COUNT; ++i) {
        accumulates_time_bank[i] = 0;
        accumulates_counters_bank[i] = 0;
    }
    task_state = task_state_t::init;
    return true;
}

uint64_t task::get_tid() const
{
    return this->tid;
}
