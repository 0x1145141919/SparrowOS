#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/fixed_interrupt_vectors.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/kptrace.h"
#include "util/kout.h"
#include "Scheduler/kthread_abi.h"
#include "memory/all_pages_arr.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "panic.h"
#include "ktime.h"
#include "util/lock.h"

#include "arch/x86_64/intel_processor_trace.h"
bool fred_support_catch_bit;//在vec_demux_init (kernel早期 初始向量解复器 置函数)中bsp测量是否支持fred,若支持则此bit置1,并且ap直接根据这个bit决定是否初始化fred。
logical_idt template_idt[256];
IDTEntry global_idt[256];
soft_interrupt_func_t soft_interrupt_functions[256];
extern "C" void user_abi_cpp_enter(x64_standard_context_v2 *frame);
extern void template_idt_apply_region(uint8_t from_vec, uint8_t to_vec);
extern uint32_t logical_processor_count;
extern vm_interval conjucnt_GSs;

// ── cmpxchg16b 原子 16B CAS（实现在 Processor/cmpxchg16b.asm） ─────
extern "C" bool cmpxchg16b(void* ptr, void* expected, const void* desired);

/* ---- class-level KURD templates ---- */
static KURD_t demux_default_kurd()
{
    return KURD_t(result_code::FAIL, 0,
                  module_code::INTERRUPT, Interrupt_module::modloc_vec_demux,
                  0, level_code::ERROR, err_domain::CORE_MODULE);
}
static KURD_t demux_default_success()
{
    KURD_t k = demux_default_kurd();
    k.result = result_code::SUCCESS;
    k.level  = level_code::INFO;
    return k;
}
static KURD_t demux_default_error()
{
    KURD_t k = demux_default_kurd();
    k.result = result_code::FAIL;
    k.level  = level_code::ERROR;
    return k;
}
static KURD_t demux_default_fatal()
{
    KURD_t k = demux_default_kurd();
    k.result = result_code::FATAL;
    k.level  = level_code::FATAL;
    return k;
}
void cpu_froze(x64_standard_context_v2* ctx){
    ctx=nullptr;
    asm volatile("cli");
    asm volatile("hlt");
}
/* ===================================================================
 * FRED STKLVLS 配置 (Linux 7.0.6 复刻)
 * ===================================================================
 * 栈等级: #DB=1, NMI=2, #MC=2, #DF=3, 其余=0
 * 保证异常嵌套时独立栈域，避免覆写
 * =================================================================== */

/* ── 异常栈等级 ── */
static constexpr uint8_t FRED_DB_STACK_LEVEL  = 1;   // #DB  专用栈 (原 IST1)
static constexpr uint8_t FRED_NMI_STACK_LEVEL = 2;   // NMI  专用栈 (原 IST2)
static constexpr uint8_t FRED_MC_STACK_LEVEL  = 2;   // #MC  共享 NMI 栈
static constexpr uint8_t FRED_DF_STACK_LEVEL  = 3;   // #DF  最高级 (原 IST1)

/* ── FRED_STKLVL: 编码单向量栈级到 MSR 位域 ── */
static constexpr uint64_t FRED_STKLVL(uint8_t vector, uint8_t lvl)
{
    return static_cast<uint64_t>(lvl) << (2 * vector);
}

/* ── 写入 IA32_FRED_STKLVLS MSR (BSP/AP 共用) ── */
static void fred_init_stklvls()
{
    uint64_t val =
        FRED_STKLVL(x86_exceptions::DEBUG,              FRED_DB_STACK_LEVEL)  |
        FRED_STKLVL(x86_exceptions::NMI,                FRED_NMI_STACK_LEVEL) |
        FRED_STKLVL(x86_exceptions::MACHINE_CHECK,       FRED_MC_STACK_LEVEL)  |
        FRED_STKLVL(x86_exceptions::DOUBLE_FAULT,       FRED_DF_STACK_LEVEL);

    wrmsr_func(msr::fred::IA32_FRED_STKLVLS, val);
}

/* ===================================================================
 * early_init — 填 IDT 0–255 (boot 阶段，无 heap)
 * ===================================================================
 * 被 _kernel_Init (asm) 调用。填充所有 256 个 IDT 条目后统一写入 global_idt。
 */
void vec_demux::early_init()
{
    using namespace Interrupt_module::vec_demux_events::init_results;

    // ── 异常入口 0–31（原 exceptions_idt_init） ──
    extern char div_by_zero_bare_enter, nmi_bare_enter, breakpoint_bare_enter;
    extern char overflow_bare_enter, invalid_opcode_bare_enter;
    extern char double_fault_bare_enter, invalid_tss_bare_enter;
    extern char general_protection_bare_enter, page_fault_bare_enter;
    extern char machine_check_bare_enter, simd_floating_point_bare_enter;
    extern char virtualization_bare_enter;

    template_idt[x86_exceptions::DIVIDE_ERROR].handler   = &div_by_zero_bare_enter;

    template_idt[x86_exceptions::NMI].handler            = &nmi_bare_enter;
    template_idt[x86_exceptions::NMI].ist_index          = 3;

    template_idt[x86_exceptions::BREAKPOINT].handler     = &breakpoint_bare_enter;
    template_idt[x86_exceptions::BREAKPOINT].ist_index   = 0;
    template_idt[x86_exceptions::BREAKPOINT].dpl         = 3;

    template_idt[x86_exceptions::OVERFLOW].handler       = &overflow_bare_enter;
    template_idt[x86_exceptions::OVERFLOW].dpl           = 3;

    template_idt[x86_exceptions::INVALID_OPCODE].handler = &invalid_opcode_bare_enter;

    template_idt[x86_exceptions::DOUBLE_FAULT].handler    = &double_fault_bare_enter;
    template_idt[x86_exceptions::DOUBLE_FAULT].ist_index  = 1;

    template_idt[x86_exceptions::INVALID_TSS].handler    = &invalid_tss_bare_enter;

    template_idt[x86_exceptions::GENERAL_PROTECTION_FAULT].handler = &general_protection_bare_enter;

    template_idt[x86_exceptions::PAGE_FAULT].handler     = &page_fault_bare_enter;

    template_idt[x86_exceptions::MACHINE_CHECK].handler   = &machine_check_bare_enter;
    template_idt[x86_exceptions::MACHINE_CHECK].ist_index = 2;

    template_idt[x86_exceptions::SIMD_FLOATING_POINT_EXCEPTION].handler = &simd_floating_point_bare_enter;

    template_idt[x86_exceptions::VIRTUALIZATION_EXCEPTION].handler = &virtualization_bare_enter;

    // ── 向量递送入口 32–255（算术寻址，无符号表） ──
    extern char vec_demux_table;
    uintptr_t table_base = (uintptr_t)&vec_demux_table;
    static constexpr uint32_t ENTRY_STRIDE = 16;

    for (uint16_t v = 32; v <= 255; v++) {
        template_idt[v].handler   = (void*)(table_base + (v - 32) * ENTRY_STRIDE);
        template_idt[v].type      = 0xE;
        template_idt[v].ist_index = 0;
        template_idt[v].dpl       = 0;
    }
    template_idt[x86_softinterrupt_abi::USER_ABI_ENTER].dpl = 3;

    // ── 一次写入 global_idt ──
    template_idt_apply_region(0, 255);
}

// asm boot entry calls this before lidt
extern "C" void vec_demux_init()
{
    vec_demux::real_init();
}
extern "C" char fred_base;
extern "C" void fred_enable(gs_complex_t*gs_complex){

        uint64_t fred_config=(uint64_t)&fred_base;
        uint64_t sl0_rsp=gs_complex->tss.rsp0;
        uint64_t sl1_rsp=gs_complex->tss.ist[1];
        uint64_t sl2_rsp=gs_complex->tss.ist[2];
        uint64_t sl3_rsp=gs_complex->tss.ist[3];

        /* ── IA32_STAR: SYSCALL/SYSRET base selectors ──
         * bits[47:32] = K_cs_idx*8   = 0x0008 → SYSCALL CS=0x08, SS=0x10
         * bits[63:48] = U_ds_idx*8|3 = 0x0023 → SYSRET 64-bit CS=0x33, SS=0x2B
         * bits[49:48]=11 符合 SDM 期望 */
        wrmsr_func(msr::syscall::IA32_STAR,
            ((uint64_t)(U_ds_ss_idx * 8 + 3) << 48) |
            ((uint64_t)(K_cs_idx * 8) << 32));

        /* ── IA32_FRED_CONFIG: 入口 + 初始零配置位 ──
         * fred_base 页对齐 → bits[11:0]=0 即所有配置位初始清零
         * CPL=3→fred_base, CPL=0→fred_base+256 (见 asm) */
        wrmsr_func(msr::fred::IA32_FRED_CONFIG, fred_config);

        /* ── IA32_FRED_STKLVLS: per-vector 栈等级 ── */
        fred_init_stklvls();

        /* ── IA32_FRED_RSP0–3: 复用 TSS 硬件栈指针 ──
         *   SL0: TSS.RSP0   — 内核入口栈
         *   SL1: TSS.IST[1] — #DB 独立栈
         *   SL2: TSS.IST[2] — NMI/#MC 共享栈
         *   SL3: TSS.IST[3] — #DF 最高级栈 */
        wrmsr_func(msr::fred::IA32_FRED_RSP0, sl0_rsp);
        wrmsr_func(msr::fred::IA32_FRED_RSP1, sl1_rsp);
        wrmsr_func(msr::fred::IA32_FRED_RSP2, sl2_rsp);
        wrmsr_func(msr::fred::IA32_FRED_RSP3, sl3_rsp);

        /* ── CR4.FRED = 1: 启用 FRED 事件投递 ── */
        uint64_t cr4_val;
        asm volatile("mov %%cr4, %0" : "=r"(cr4_val));
        cr4_val |= (1ULL << 32);   /* CR4.FRED bit 32 */
        asm volatile("mov %0, %%cr4" : : "r"(cr4_val));
    
}
void vec_demux::real_init()
{
    using namespace Interrupt_module::vec_demux_events::init_results;

    /* ── 检测 BSP 是否支持 FRED (CPUID leaf 0x07, subleaf 1, EAX bit 17) ── */
    {
        cpuid_tmp cpuid7_1(0x07, 0x01);
        fred_support_catch_bit = (cpuid7_1.eax >> 17) & 1;
    }

    /* ── 阶段 1: 填 IDT 模板 ── */
    early_init();

    /* ── 清零软中断函数表 ── */
    ksetmem_8(soft_interrupt_functions, 0, sizeof(soft_interrupt_functions));
    extern void kthread_call_cpp_enter(x64_standard_context_v2 *frame);
    extern void asm_panic_cpp_enter(x64_standard_context_v2 *frame);
    extern void suprious_interrupt_cpp_enter(x64_standard_context_v2 *frame);
    soft_interrupt_functions[x86_softinterrupt_abi::ASM_PANIC]    = &asm_panic_cpp_enter;
    soft_interrupt_functions[x86_softinterrupt_abi::KTHREAD_CALL] = &kthread_call_cpp_enter;
    soft_interrupt_functions[x86_softinterrupt_abi::USER_ABI_ENTER] = &user_abi_cpp_enter;

    bsp_kout << now << "[vec_demux] real_init: vectors 32~255 ready" << kendl;
}

/* ===================================================================
 * alloc_vec
 * =================================================================== */
uint8_t vec_demux::alloc_vec(interrupt_token_t* token,
                             uint32_t processor_id,
                             KURD_t &kurd)
{
    KURD_t success_k = demux_default_success();
    KURD_t fail_k    = demux_default_error();
    success_k.event_code = Interrupt_module::vec_demux_events::alloc_vec;
    fail_k.event_code    = Interrupt_module::vec_demux_events::alloc_vec;

    using namespace Interrupt_module::vec_demux_events::alloc_vec_results;
    using namespace Interrupt_module::vec_demux_events::common_fail_reason_code;

    if (!token || !token->func) {
        kurd = fail_k;
        kurd.reason = fail_reason_code::BAD_FUNC_PTR;
        return 0;
    }
    if (!is_addr_kernel_address((void *)token->func)) {
        kurd = fail_k;
        kurd.reason = fail_reason_code::BAD_FUNC_PTR;
        return 0;
    }
    {   const symbol_entry* se = ksymmanager::get_entry_near_addr((vaddr_t)token->func);
        if (!se || se->address != (uint64_t)token->func) {
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
    interrupt_token_t* slice = cx->tokens;
    uint16_t vec = 32;
    {
        spinlock_interrupt_about_guard l(cx->tokens_lock);
        
        for (; vec <= 255; vec++) {
            if(!fred_support_catch_bit)if (soft_interrupt_functions[vec] != nullptr) continue;
            if (vec >= ipi_vecs::IPI_RESCHED && vec <= SUPRIOUS_INTERRUPT) continue;
            if (slice[vec].func == nullptr) break;
        }
        if (vec > 255) {
            kurd = fail_k;
            kurd.reason = fail_reason_code::NO_FREE_VEC;
            return 0;
        }
        slice[vec] = *token;
    }

    kurd = success_k;
    return vec;
}

/* ===================================================================
 * free_vec
 * =================================================================== */
KURD_t vec_demux::free_vec(uint8_t vec, uint32_t processor_id)
{
    KURD_t success_k = demux_default_success();
    KURD_t fail_k    = demux_default_error();
    success_k.event_code = Interrupt_module::vec_demux_events::free_vec;
    fail_k.event_code    = Interrupt_module::vec_demux_events::free_vec;

    using namespace Interrupt_module::vec_demux_events::free_vec_results;
    using namespace Interrupt_module::vec_demux_events::common_fail_reason_code;

    if (vec < 32 || vec > 255) {
        fail_k.reason = INVALID_VEC;
        return fail_k;
    }
    if (processor_id >= logical_processor_count) {
        fail_k.reason = INVALID_PROCESSOR_ID;
        return fail_k;
    }

    gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + processor_id * GS_COMPLEX_STRIDE);
    interrupt_token_t* slice = cx->tokens;

    {
        spinlock_interrupt_about_guard l(cx->tokens_lock);
        if (slice[vec].func == nullptr) {
            fail_k.reason = fail_reason_code::VEC_NOT_ALLOCED;
            return fail_k;
        }
        ksetmem_8(slice + vec, 0, sizeof(interrupt_token_t));
    }
    return success_k;
}

/* ===================================================================
 * get_vec
 * =================================================================== */
interrupt_token_t *vec_demux::get_vec(uint8_t vec,
                                      uint32_t processor_id,
                                      KURD_t &kurd)
{
    KURD_t success_k = demux_default_success();
    KURD_t fail_k    = demux_default_error();
    success_k.event_code = Interrupt_module::vec_demux_events::get_vec;
    fail_k.event_code    = Interrupt_module::vec_demux_events::get_vec;

    using namespace Interrupt_module::vec_demux_events::common_fail_reason_code;
    using namespace Interrupt_module::vec_demux_events::free_vec_results::fail_reason_code;

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
    interrupt_token_t *entry;

    {
        spinlock_interrupt_about_guard l(cx->tokens_lock);
        entry = &cx->tokens[vec];
        if (entry->func == nullptr) {
        kurd = fail_k;
        kurd.reason = VEC_NOT_ALLOCED;
        return nullptr;
    }

    kurd = success_k;
    return entry;
    }
}

/* ===================================================================
 * alloc_vec_by_apicid
 * =================================================================== */
uint8_t vec_demux::alloc_vec_by_apicid(interrupt_token_t *token, uint32_t x2_apicid, KURD_t &kurd)
{
    KURD_t success_k = demux_default_success();
    KURD_t fail_k    = demux_default_error();
    success_k.event_code = Interrupt_module::vec_demux_events::alloc_vec;
    fail_k.event_code    = Interrupt_module::vec_demux_events::alloc_vec;

    using namespace Interrupt_module::vec_demux_events::alloc_vec_results;
    using namespace Interrupt_module::vec_demux_events::common_fail_reason_code;

    if (!token || !token->func) {
        kurd = fail_k;
        kurd.reason = fail_reason_code::BAD_FUNC_PTR;
        return 0;
    }
    if (!is_addr_kernel_address((void *)token->func)) {
        kurd = fail_k;
        kurd.reason = fail_reason_code::BAD_FUNC_PTR;
        return 0;
    }
    {   const symbol_entry* se = ksymmanager::get_entry_near_addr((vaddr_t)token->func);
        if (!se || se->address != (uint64_t)token->func) {
            kurd = fail_k;
            kurd.reason = fail_reason_code::SYM_NOT_FOUND;
            return 0;
        }
    }
    gs_complex_t* cx = g_gs_by_apicid[x2_apicid];
    if (!cx) {
        kurd = fail_k;
        kurd.reason = INVALID_PROCESSOR_ID;
        return 0;
    }

    interrupt_token_t* slice = cx->tokens;
    uint16_t vec = 32;
    {
        spinlock_interrupt_about_guard l(cx->tokens_lock);
        
    for (;vec <= 255; vec++){
        if(!fred_support_catch_bit)if (soft_interrupt_functions[vec] != nullptr) continue;
        if (vec >= ipi_vecs::IPI_RESCHED && vec <= SUPRIOUS_INTERRUPT) continue;
        if (slice[vec].func == nullptr) break;
    }
    if (vec > 255) {
        kurd = fail_k;
        kurd.reason = fail_reason_code::NO_FREE_VEC;
        return 0;
    }
        slice[vec] = *token;
    }

    kurd = success_k;
    return vec;
}

/* ===================================================================
 * 统一外部接口 (IDT/FRED 切换点)
 * =================================================================== */
extern "C" uint8_t out_interrupt_vec_alloc(interrupt_token_t* token,
                                           uint32_t processor_id,
                                           KURD_t *kurd)
{
    if (!kurd) return 0;
    return vec_demux::alloc_vec(token, processor_id, *kurd);
}
extern "C" uint8_t out_interrupt_vec_alloc_by_apicid(interrupt_token_t* token,
                                                     uint32_t x2_apicid,
                                                     KURD_t *kurd)
{
    if (!kurd) return 0;
    return vec_demux::alloc_vec_by_apicid(token, x2_apicid, *kurd);
}
extern "C" KURD_t out_interrupt_vec_free(uint8_t vec, uint32_t processor_id)
{
    return vec_demux::free_vec(vec, processor_id);
}
extern "C" interrupt_token_t *out_interrupt_vec_get(uint8_t vec,
                                                    uint32_t processor_id,
                                                    KURD_t *kurd)
{
    if (!kurd) return nullptr;
    return vec_demux::get_vec(vec, processor_id, *kurd);
}

/* ===================================================================
 * idt_vec_demux_entry — trampoline 公共入口 (IDT 路径)
 * ===================================================================
 * asm vec_demux_common 保存 GPR 后在此进入。
 * raw_frame 包含 GPR + vec + iretq，vec 在 GPR 与 iret 之间。
 * 调用 handler 前需转换为 x64_standard_context（修正 vec 导致的偏移）。
 */
extern "C" void idt_vec_demux_entry(x64_standard_context_v2* raw_frame)
{
    uint8_t vec = raw_frame->core_ctx.idtctx.num.vec;

    if (vec < 32) {
        KURD_t fatal_k = demux_default_fatal();
        fatal_k.event_code = Interrupt_module::vec_demux_events::dispatch;
        fatal_k.reason = Interrupt_module::vec_demux_events::dispatch_results::fatal_reason_code::BAD_VEC_RECIEVED;
        panic_context::x64_context ctx = {};
        panic_info_inshort info = { .is_bug = 1, .is_policy = 0, .is_hw_fault = 0,
                                    .is_mem_corruption = 0, .is_escalated = 0 };
        Panic::panic(default_panic_behaviors_flags,
            (char *)"idt_vec_demux_entry: vec < 32 on vec_delivery path",
            &ctx, &info, fatal_k);
        return;
    }

    uint32_t pid = fast_get_processor_id();

    /* ── 1. 软中断表 (全局, 同步, int N) ── */
    if (soft_interrupt_functions[vec]) {
        soft_interrupt_functions[vec](raw_frame);
        //asm volatile("ud2");   // 软中断必须不返回
        //事实上软中断必须允许返回，不然到时候哪些int 227的直接返回用户空间的系统调用，以及226的block_if_equal直接返回分支
        return;
    }
    gs_complex_t* self = (gs_complex_t*)rdmsr(msr::syscall::IA32_GS_BASE);
    __uint128_t*local_ipi_complex = (__uint128_t*)&self->local_ipi_complex;
    struct local_ipi_complex_fnbox_t{
        void*(*func)(void*);
        void* arg;
    };
    
    __uint128_t fnbox_copy=*local_ipi_complex;
    /* ── 2. 系统 IPI 表 (全局, LAPIC ICR) ── */
    switch(vec){
        case SUPRIOUS_INTERRUPT:{
            bsp_kout << now << "[vec_demux] SUPRIOUS on CPU " << pid << kendl;
            return;
        }
        case ipi_vecs::IPI_HALT:{
            if (global_pt_blackboxes)
                disable_blackbox(&global_pt_blackboxes[pid]);
            asm volatile("cli;hlt");
            __builtin_unreachable();
        }
        case ipi_vecs::IPI_RETURNABLE:{
            local_ipi_complex_fnbox_t* fnbox=(local_ipi_complex_fnbox_t*)local_ipi_complex;
            uint64_t result=(uint64_t)fnbox->func(fnbox->arg);
            __uint128_t result_box=(__uint128_t)result << 64 | 1;   // hi64=返回值, lo64=1
            cmpxchg16b(local_ipi_complex,&fnbox_copy,&result_box);
            x2apic::x2apic_driver::write_eoi();
            return;
        }
        case ipi_vecs::IPI_RUNAWAY:{
            local_ipi_complex_fnbox_t fnbox=*(local_ipi_complex_fnbox_t*)local_ipi_complex;
            __uint128_t get_func_mail = 1;
            cmpxchg16b(local_ipi_complex,&fnbox_copy,&get_func_mail);
            x2apic::x2apic_driver::write_eoi();
            fnbox.func(fnbox.arg);
            asm volatile("ud2");
        }
        case ipi_vecs::IPI_RESCHED:{ 
            x2apic::x2apic_driver::write_eoi();
            resched(raw_frame);
        }
        default:{
            interrupt_token_t local_tok;
            {
                spinlock_interrupt_about_guard l(self->tokens_lock);
                local_tok = self->tokens[vec];
            }
            if (local_tok.func) {
                uint64_t res = local_tok.func(&local_tok);
                x2apic::x2apic_driver::write_eoi();
                if (res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE)
                    resched(raw_frame);
            }
            return;
        }
    }
    
    

    /* ── 4. 未匹配 → 虚假中断 ── */
    bsp_kout << now << "[vec_demux] WARNING: no handler for vec "
             << (uint32_t)vec << " on processor " << pid << kendl;
}

/* ===================================================================
 * vec_demux_hw_dispatch — FRED type 0 (外部中断) 分发器
 * ===================================================================
 * FRED 下硬件中断可能携带 IPI，统一走 v3 消息槽 + tokens 表。
 * 注意：FRED 自动管理 EOIf，此处不应再调用 write_eoi()。
 */
void fred_vec_demux_hw_dispatch(x64_standard_context_v2* frame, uint8_t vec)
{
    gs_complex_t* self = (gs_complex_t*)rdmsr(msr::syscall::IA32_GS_BASE);
    __uint128_t* slot = &self->local_ipi_complex;
    __uint128_t msg = *slot;
    uint32_t pid = fast_get_processor_id();
    switch (vec) {
        case SUPRIOUS_INTERRUPT:{
            bsp_kout << now << "[vec_demux] SUPRIOUS on CPU " << pid << kendl;
            return;
        }
    case ipi_vecs::IPI_HALT: {
        if (global_pt_blackboxes)
            disable_blackbox(&global_pt_blackboxes[pid]);
        asm volatile("cli;hlt");
        __builtin_unreachable();
    }
    case ipi_vecs::IPI_RETURNABLE: {
        auto fn  = (uint64_t (*)(uint64_t))(uint64_t)msg;
        uint64_t ret = fn((uint64_t)(msg >> 64));
        __uint128_t result_box = (__uint128_t)ret << 64|1;
        __uint128_t expected = msg;
        cmpxchg16b(slot, &expected, &result_box);
        return;  // FRED 自动 EOI
    }
    case ipi_vecs::IPI_RUNAWAY: {
        __uint128_t get_func_mail = 1;
        __uint128_t expected = msg;
        cmpxchg16b(slot, &expected, &get_func_mail);
        auto fn = (void (*)(void*))(uint64_t)msg;
        fn((void*)(uint64_t)(msg >> 64));
        __builtin_unreachable();
    }
    case ipi_vecs::IPI_RESCHED:{ 
            resched(frame);
        }
    default: {
        interrupt_token_t local_tok;
        {
            spinlock_interrupt_about_guard l(self->tokens_lock);
            local_tok = self->tokens[vec];
        }
        if (local_tok.func) {
            uint64_t res = local_tok.func(&local_tok);
            if (res & TOKEN_FLAG_MASK_TOKEN_SCHEDULE)
                resched(frame);
            return;
        }
        bsp_kout << now << "[vec_demux:FRED/HW] no handler for vec "
                 << (uint32_t)vec << kendl;
        return;
    }
    }
}

/* ===================================================================
 * vec_demux_soft_dispatch — FRED type 4 (软中断) 分发器
 * =================================================================== */
void fred_vec_demux_soft_dispatch(x64_standard_context_v2* frame, uint8_t vec)
{
    /* FRED 下软中断不经过 IDT，直接查 soft_interrupt_functions */
    if (soft_interrupt_functions[vec]) {
        soft_interrupt_functions[vec](frame);
        asm volatile("ud2");
    }

    bsp_kout << now << "[vec_demux:FRED/SOFT] no soft handler for vec "
             << (uint32_t)vec << kendl;
}

// ── 辅助：id → gs_complex_t* 解析 ─────────────────────────────────
static gs_complex_t* resolve_target(uint32_t id, bool is_apicid)
{
    if (is_apicid)
        return g_gs_by_apicid[id];

    if (id >= logical_processor_count)
        return nullptr;
    return (gs_complex_t*)(conjucnt_GSs.vbase() + id * GS_COMPLEX_STRIDE);
}

// ── 辅助：取目标的 x2APIC ID（从 slot[1] 编码提取） ──────────────
static uint32_t target_x2apicid(gs_complex_t* cx)
{
    return (uint32_t)(cx->slots[PROCESSOR_ID_GS_INDEX] >> 32);
}

// ── 辅助：构建定向 ICR ────────────────────────────────────────────
static x2apic::x2apic_icr_t make_ipi_icr(uint8_t vec, uint32_t dest_apicid)
{
    x2apic::x2apic_icr_t icr = {};
    icr.param.vector               = vec;
    icr.param.delivery_mode        = LAPIC_PARAMS_ENUM::DELIVERY_MODE_T::FIXED;
    icr.param.destination_mode     = LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL;
    icr.param.destination_shorthand= LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::NO_SHORTHAND;
    icr.param.destination.id       = dest_apicid;
    return icr;
}
// ── 轮询辅助：等待 slot.lo64 变为 1 ────────────────────────────
// 全环境统一使用 pause 自旋
// 返回 true=预期值到达, false=超时
static bool ipi_wait_lo(volatile __uint128_t* slot, uint64_t deadline_us)
{
    while ((uint64_t)*slot != 1) {
        if (ktime::get_microsecond_stamp() >= deadline_us)
            return false;
        asm volatile("pause");
    }
    return true;
}

/* ===================================================================
 * returnable_ipi_send — 返回型 IPI
 * ===================================================================
 * 抢占目标 slot → 发送 IPI_RETURNABLE → 轮询结果（10ms 超时）
 * 返回值: hi64=fn(arg) 返回值, lo64=结果码
 *   1=成功, 2=抢占失败(BUSY), 3=超时, 4=目标不存在
 */
__uint128_t returnable_ipi_send(ipi_package_t *package)
{
    gs_complex_t* complex = resolve_target(package->id, package->is_apicid);
    if (!complex)
        return (__uint128_t)0 << 64 | 4;   // 不存在

    interrupt_guard irq;

    /* 抢占 slot */
    __uint128_t expected = 0;
    __uint128_t desired  = package->func
                         | ((__uint128_t)(uint64_t)package->arg << 64);
    if (!cmpxchg16b(&complex->local_ipi_complex, &expected, &desired))
        return (__uint128_t)0 << 64 | 2;   // BUSY

    /* 发 IPI */
    uint32_t dest_apicid = package->is_apicid
        ? package->id
        : target_x2apicid(complex);
    x2apic::x2apic_driver::raw_send_ipi(make_ipi_icr(ipi_vecs::IPI_RETURNABLE, dest_apicid));

    /* 轮询结果：lo64 != func → target 已消费并写回 */
    uint64_t deadline = ktime::get_microsecond_stamp() + 10000;  // 10ms
    if (!ipi_wait_lo(&complex->local_ipi_complex, deadline)) {
        __uint128_t release=0;
        cmpxchg16b(&complex->local_ipi_complex, &desired, &release);
        return (__uint128_t)0 << 64 | 3;   // 超时
    }
    __uint128_t val = complex->local_ipi_complex;
    uint64_t result = (uint64_t)(val >> 64);
     __uint128_t release_zero=0;
    cmpxchg16b(&complex->local_ipi_complex, &val, &release_zero);     // 释放槽
    return (__uint128_t)result << 64 | 1;  // 成功
}

/* ===================================================================
 * fly_ipi_send — 跑飞型 IPI（不等待结果）
 * ===================================================================
 * 抢占目标 slot → 发送 IPI_RUNAWAY → 立即返回
 * 返回值: lo64=结果码（1=成功, 2=抢占失败, 4=目标不存在）
 */
uint64_t fly_ipi_send(ipi_package_t *package)
{
    gs_complex_t* complex = resolve_target(package->id, package->is_apicid);
    if (!complex)
        return 4;

    interrupt_guard irq;

    __uint128_t expected = 0;
    __uint128_t desired  = package->func
                         | ((__uint128_t)(uint64_t)package->arg << 64);
    if (!cmpxchg16b(&complex->local_ipi_complex, &expected, &desired))
        return 2;   // BUSY

    uint32_t dest_apicid = package->is_apicid
        ? package->id
        : target_x2apicid(complex);
    x2apic::x2apic_driver::raw_send_ipi(make_ipi_icr(ipi_vecs::IPI_RUNAWAY, dest_apicid));

    /* 轮询确认：target 将 lo64 置 1 表示已消费 */
    uint64_t deadline = ktime::get_microsecond_stamp() + 1000000;  // 10ms
    __uint128_t release_zero=0;
    if (!ipi_wait_lo(&complex->local_ipi_complex, deadline)) {
        cmpxchg16b(&complex->local_ipi_complex, &desired, &release_zero);
        return 3;                         // 超时
    }
    __uint128_t val = complex->local_ipi_complex;
    cmpxchg16b(&complex->local_ipi_complex, &val, &release_zero);
    return 1;                               // 成功
}

/* ===================================================================
 * broadcast_halt — 广播停机（ALL_EXCLUDING_SELF，不碰 slot）
 * =================================================================== */
extern "C" void broadcast_halt()
{
    x2apic::x2apic_icr_t icr = {};
    icr.param.vector               = ipi_vecs::IPI_HALT;
    icr.param.delivery_mode        = LAPIC_PARAMS_ENUM::DELIVERY_MODE_T::FIXED;
    icr.param.destination_mode     = LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL;
    icr.param.destination_shorthand= LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::ALL_EXCLUDING_SELF;
    icr.param.destination.id       = 0xFFFFFFFF;
    x2apic::x2apic_driver::raw_send_ipi(icr);
}

/* ===================================================================
 * halt_on — 定向停机（不碰 slot）
 * =================================================================== */
extern "C" void halt_on(uint32_t id, bool is_apicid)
{
    uint32_t apicid = is_apicid ? id : target_x2apicid(
        (gs_complex_t*)(conjucnt_GSs.vbase() + id * GS_COMPLEX_STRIDE));
    x2apic::x2apic_driver::raw_send_ipi(make_ipi_icr(ipi_vecs::IPI_HALT, apicid));
}