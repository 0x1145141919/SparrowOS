#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/fixed_interrupt_vectors.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/kptrace.h"
#include "util/kout.h"
#include "memory/all_pages_arr.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "panic.h"

logical_idt template_idt[256];
IDTEntry global_idt[256];
soft_interrupt_func_t soft_interrupt_functions[256];
extern void template_idt_apply_region(uint8_t from_vec, uint8_t to_vec);
extern uint32_t logical_processor_count;
extern vm_interval conjucnt_GSs;

static spinlock_cpp_t dispatch_lock;

/* ---- class-level KURD templates ---- */
static KURD_t idt_mgr_default_kurd()
{
    return KURD_t(result_code::FAIL, 0,
                  module_code::INTERRUPT, Interrupt_module::modloc_idt_mgr,
                  0, level_code::ERROR, err_domain::CORE_MODULE);
}
static KURD_t idt_mgr_default_success()
{
    KURD_t k = idt_mgr_default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    return k;
}
static KURD_t idt_mgr_default_error()
{
    KURD_t k = idt_mgr_default_kurd();
    k.result = result_code::FAIL;
    k.level  = level_code::ERROR;
    return k;
}
static KURD_t idt_mgr_default_fatal()
{
    KURD_t k = idt_mgr_default_kurd();
    k.result = result_code::FATAL;
    k.level  = level_code::FATAL;
    return k;
}

/* ---- helper: parse "vec_delivery_<N>_bare_enter" ---- */
static int parse_vec_delivery_name(const char *name)
{
    const char prefix[] = "vec_delivery_";
    const char suffix[] = "_bare_enter";
    size_t plen = strlen_in_kernel(prefix);
    size_t slen = strlen_in_kernel(suffix);
    size_t nlen = strlen_in_kernel(name);
    if (nlen <= plen + slen) return -1;
    if (strncmp_in_kernel(name, prefix, plen) != 0) return -1;
    if (strcmp_in_kernel(name + nlen - slen, suffix) != 0) return -1;
    int v = 0;
    const char *p = name + plen;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    if (*p != '_') return -1;
    return v;
}

/* ===================================================================
 * Init
 * =================================================================== */
KURD_t idt_vec_dispatch_mgr::Init()
{
    KURD_t success_k = idt_mgr_default_success();
    KURD_t fatal_k   = idt_mgr_default_fatal();
    success_k.event_code = Interrupt_module::idt_mgr_events::init;
    fatal_k.event_code   = Interrupt_module::idt_mgr_events::init;

    using namespace Interrupt_module::idt_mgr_events::init_results;

    /* dispatch 表已预分配在 conjunc_GSs (gs_complex_t::dispatch)，仅需清零 */
    vaddr_t gs_vbase = conjucnt_GSs.vbase();
    for (uint32_t p = 0; p < logical_processor_count; p++) {
        gs_complex_t* cx = (gs_complex_t*)(gs_vbase + p * GS_COMPLEX_STRIDE);
        ksetmem_8(cx->dispatch, 0, sizeof(cx->dispatch));
    }

    /* resolve vec_delivery symbols */
    uint32_t sym_count = ksymmanager::get_entry_count();
    const symbol_entry *table = ksymmanager::get_table();
    if (!table || sym_count == 0) {
        fatal_k.reason = fatal_reason_code::SYMBOL_TABLE_UNAVAILABLE;
        panic_context::x64_context ctx = {};
        panic_info_inshort info = {
            .is_bug = 1, .is_policy = 0, .is_hw_fault = 0,
            .is_mem_corruption = 0, .is_escalated = 0 };
        Panic::panic(default_panic_behaviors_flags,
            (char *)"idt_vec_dispatch_mgr::Init: symbol table not available",
            &ctx, &info, fatal_k);
        return fatal_k;
    }

    int found = 0;
    const symbol_entry *base_ptr = nullptr;
    for (uint32_t i = 0; i < sym_count; i++) {
        if (parse_vec_delivery_name(table[i].name) == 32) {
            base_ptr = &table[i];
            break;
        }
    }
    if (base_ptr) {
        uint32_t remain = sym_count - (uint32_t)(base_ptr - table);
        for (int expect = 32; (uint32_t)(expect - 32) < remain; expect++) {
            const symbol_entry *se = &base_ptr[expect - 32];
            int v = parse_vec_delivery_name(se->name);
            if (v != expect) break;
            template_idt[v].handler   = (void *)se->address;
            template_idt[v].type      = 0xE;
            template_idt[v].ist_index = 0;
            template_idt[v].dpl       = 0;
            found++;
        }
    }
    if (found < 224) {
        for (uint32_t i = 0; i < sym_count; i++) {
            int v = parse_vec_delivery_name(table[i].name);
            if (v < 32 || v > 255) continue;
            if (template_idt[v].handler != nullptr) continue;
            template_idt[v].handler   = (void *)table[i].address;
            template_idt[v].type      = 0xE;
            template_idt[v].ist_index = 0;
            template_idt[v].dpl       = 0;
            found++;
        }
    }
    if (found != 224) {
        fatal_k.reason = fatal_reason_code::NOT_ALL_IDT_FOUND;
        panic_context::x64_context ctx = {};
        panic_info_inshort info = {
            .is_bug = 1, .is_policy = 0, .is_hw_fault = 0,
            .is_mem_corruption = 0, .is_escalated = 0 };
        Panic::panic(default_panic_behaviors_flags,
            (char *)"idt_vec_dispatch_mgr::Init: not all vec_delivery (32~255) resolved",
            &ctx, &info, fatal_k);
        return fatal_k;
    }
    template_idt[ivec::USER_ABI_ENTER].dpl=3;
    template_idt_apply_region(32, 255);

    // 初始化软中断函数表
    ksetmem_8(soft_interrupt_functions, 0, sizeof(soft_interrupt_functions));
    extern void kthread_call_cpp_enter(x64_standard_context *frame);
    extern void ipi_cpp_enter(x64_standard_context *frame);
    extern void asm_panic_cpp_enter(x64_standard_context *frame);
    extern void suprious_interrupt_cpp_enter(x64_standard_context *frame);
    extern void user_abi_cpp_enter(x64_standard_context *frame);
    soft_interrupt_functions[ivec::ASM_PANIC] = &asm_panic_cpp_enter;
    soft_interrupt_functions[ivec::IPI] = &ipi_cpp_enter;
    soft_interrupt_functions[ivec::KTHREAD_CALL] = &kthread_call_cpp_enter;
    soft_interrupt_functions[ivec::SUPRIOUS_INTERRUPT] = &suprious_interrupt_cpp_enter;

    bsp_kout << now << "[idt_vec_dispatch_mgr] Init: " << (uint32_t)found
             << " vec_delivery entries, ist=5" << kendl;

    return success_k;
}

/* ===================================================================
 * alloc_vec
 * =================================================================== */
uint8_t idt_vec_dispatch_mgr::alloc_vec(hard_interrupt_func_t func,
                                        uint32_t processor_id,
                                        KURD_t &kurd)
{
    KURD_t success_k = idt_mgr_default_success();
    KURD_t fail_k    = idt_mgr_default_error();
    success_k.event_code = Interrupt_module::idt_mgr_events::alloc_vec;
    fail_k.event_code    = Interrupt_module::idt_mgr_events::alloc_vec;

    using namespace Interrupt_module::idt_mgr_events::alloc_vec_results;
    using namespace Interrupt_module::idt_mgr_events::common_fail_reason_code;

    if (!is_addr_kernel_address((void *)func)) {
        kurd = fail_k;
        kurd.reason = fail_reason_code::BAD_FUNC_PTR;
        return 0;
    }

    /* verify func is a known kernel symbol (must match exactly) */
    {   const symbol_entry* se = ksymmanager::get_entry_near_addr((vaddr_t)func);
        if (!se || se->address != (uint64_t)func) {
            kurd = fail_k;
            kurd.reason = fail_reason_code::SYM_NOT_FOUND;
            return 0;
        }
    }
    if (processor_id >= logical_processor_count) {
        kurd = fail_k;
        kurd.reason = INVALID_PROCESSOR_ID;
        return 0;
    }

    gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + processor_id * GS_COMPLEX_STRIDE);
    hard_interrupt_func_t* slice = cx->dispatch;

    dispatch_lock.lock();
    uint8_t vec = 32;
    for (; vec <= 255; vec++) {
        if (soft_interrupt_functions[vec] != nullptr) continue;
        if (slice[vec] == nullptr) break;
    }
    if (vec > 255) {
        dispatch_lock.unlock();
        kurd = fail_k;
        kurd.reason = fail_reason_code::NO_FREE_VEC;
        return 0;
    }
    slice[vec] = func;
    dispatch_lock.unlock();

    kurd = success_k;
    return vec;
}

/* ===================================================================
 * free_vec
 * =================================================================== */
KURD_t idt_vec_dispatch_mgr::free_vec(uint8_t vec, uint32_t processor_id)
{
    KURD_t success_k = idt_mgr_default_success();
    KURD_t fail_k    = idt_mgr_default_error();
    success_k.event_code = Interrupt_module::idt_mgr_events::free_vec;
    fail_k.event_code    = Interrupt_module::idt_mgr_events::free_vec;

    using namespace Interrupt_module::idt_mgr_events::free_vec_results;
    using namespace Interrupt_module::idt_mgr_events::common_fail_reason_code;

    if (vec < 32 || vec > 255) {
        fail_k.reason = INVALID_VEC;
        return fail_k;
    }
    if (processor_id >= logical_processor_count) {
        fail_k.reason = INVALID_PROCESSOR_ID;
        return fail_k;
    }

    gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + processor_id * GS_COMPLEX_STRIDE);
    hard_interrupt_func_t* slice = cx->dispatch;

    dispatch_lock.lock();
    if (slice[vec] == nullptr) {
        dispatch_lock.unlock();
        fail_k.reason = fail_reason_code::VEC_NOT_ALLOCED;
        return fail_k;
    }
    slice[vec] = nullptr;
    dispatch_lock.unlock();

    return success_k;
}

/* ===================================================================
 * get_vec
 * =================================================================== */
hard_interrupt_func_t *idt_vec_dispatch_mgr::get_vec(uint8_t vec,
                                                     uint32_t processor_id,
                                                     KURD_t &kurd)
{
    KURD_t success_k = idt_mgr_default_success();
    KURD_t fail_k    = idt_mgr_default_error();
    success_k.event_code = Interrupt_module::idt_mgr_events::get_vec;
    fail_k.event_code    = Interrupt_module::idt_mgr_events::get_vec;

    using namespace Interrupt_module::idt_mgr_events::common_fail_reason_code;
    using namespace Interrupt_module::idt_mgr_events::free_vec_results::fail_reason_code;

    if (vec < 32 || vec > 255) {
        kurd = fail_k;
        kurd.reason = INVALID_VEC;
        return nullptr;
    }
    if (processor_id >= logical_processor_count) {
        kurd = fail_k;
        kurd.reason = INVALID_PROCESSOR_ID;
        return nullptr;
    }

    gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + processor_id * GS_COMPLEX_STRIDE);
    hard_interrupt_func_t *entry = &cx->dispatch[vec];

    if (*entry == nullptr) {
        kurd = fail_k;
        kurd.reason = VEC_NOT_ALLOCED;
        return nullptr;
    }

    kurd = success_k;
    return entry;
}

/* ===================================================================
 * Unified dispatch interface — IDT path (FRED placeholder)
 * =================================================================== */
extern "C" uint8_t out_interrupt_vec_alloc(hard_interrupt_func_t func,
                                            uint32_t processor_id,
                                            KURD_t *kurd)
{
    if (!kurd) return 0xff;
    return idt_vec_dispatch_mgr::alloc_vec(func, processor_id, *kurd);
}

extern "C" KURD_t out_interrupt_vec_free(uint8_t vec, uint32_t processor_id)
{
    return idt_vec_dispatch_mgr::free_vec(vec, processor_id);
}

extern "C" hard_interrupt_func_t *out_interrupt_vec_get(uint8_t vec,
                                                        uint32_t processor_id,
                                                        KURD_t *kurd)
{
    if (!kurd) return nullptr;
    return idt_vec_dispatch_mgr::get_vec(vec, processor_id, *kurd);
}

/* ===================================================================
 * all_vec_delivery — asm stubs dispatch to here
 * =================================================================== */
extern "C" void all_vec_delivery(x64_standard_context *frame, uint8_t vec)
{
    if (vec < 32) {
        KURD_t fatal_k = idt_mgr_default_fatal();
        fatal_k.event_code = Interrupt_module::idt_mgr_events::vec_dispatch;
        fatal_k.reason = Interrupt_module::idt_mgr_events::vec_dispatch_results::fatal_reason_code::BAD_VEC_RECIEVED;
        panic_context::x64_context ctx = {};
        panic_info_inshort info = {
            .is_bug = 1, .is_policy = 0, .is_hw_fault = 0,
            .is_mem_corruption = 0, .is_escalated = 0 };
        Panic::panic(default_panic_behaviors_flags,
            (char *)"all_vec_delivery: vec < 32 on vec_delivery path",
            &ctx, &info, fatal_k);
        return;
    }

    uint32_t pid = fast_get_processor_id();

    /* soft (global, synchronous) */
    if (soft_interrupt_functions[vec]) {
        soft_interrupt_functions[vec](frame);
        return;
    }
    //这里可以直接rdmsr获取gs基址作为gs_complex
    /* per-processor hardware — 从 gs_complex_t::dispatch 直接取 */
    gs_complex_t* self = (gs_complex_t*)(conjucnt_GSs.vbase() + pid * GS_COMPLEX_STRIDE);
    if (self->dispatch[vec]) {
        self->dispatch[vec](frame, vec, pid);
        x2apic::x2apic_driver::write_eoi();
        return;
    }
    bsp_kout << now << "[all_vec_delivery] WARNING: no handler for vec "
             << (uint32_t)vec << " on processor " << pid << kendl;
}
