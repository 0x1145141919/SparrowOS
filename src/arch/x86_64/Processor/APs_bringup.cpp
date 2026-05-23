#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/AP_Init_error_observing_protocol.h"
#include "util/kout.h"
#include "firmware/ACPI_APIC.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "ktime.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/textConsole.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"

/* ── 全局：x2APIC ID → gs_complex_t 映射表 ──────────────────────
 * 由 ap_init_one_by_one 的第一遍扫描分配，第二遍填值。
 * AP 查表：g_gs_by_apicid[query_x2apicid()] → 自己的 gs_complex_t */
gs_complex_t** g_gs_by_apicid = nullptr;
extern vm_interval conjucnt_GSs;
/* 辅助：遍历 MADT 处理器链表 */
static void walk_processors(void (*cb)(const APICtb_analyzed_structures::processor_x64_lapic_struct&, void*), void* ctx) {
    for (auto it = gAnalyzer->processor_x64_list->begin();
         it != gAnalyzer->processor_x64_list->end(); ++it)
        cb(*it, ctx);
}

constexpr uint32_t error_code_bitmap = 0
    | (1 << 8)   // #DF
    | (1 << 10)  // #TS
    | (1 << 11)  // #NP
    | (1 << 12)  // #SS
    | (1 << 13)  // #GP
    | (1 << 14)  // #PF
    | (1 << 17)  // #AC
    | (1 << 21); // #CP

/* ── AP 启动：按图索骥唤醒 ──────────────────────────────────────────
 *
 * 遍历 gAnalyzer->processor_x64_list，对每个非 BSP 处理器发送 INIT + SIPI，
 * 通过 4 个 checkpoint 观测 AP 启动进度：
 *   realmode_enter → pemode_enter → longmode_enter → init_finished
 */
extern "C" KURD_t ap_init_one_by_one()
{
    /* ── KURD 模板 ─────────────────────────────────────────────────── */
    auto fail_k   = []->KURD_t{ return KURD_t(result_code::FAIL,   0, module_code::INTERRUPT,
                    INTERRUPT_SUB_MODULES_LOCATIONS::loc_processor, 0,
                    level_code::ERROR, err_domain::CORE_MODULE); };
    auto fatal_k  = []->KURD_t{ return KURD_t(result_code::FATAL,  0, module_code::INTERRUPT,
                    INTERRUPT_SUB_MODULES_LOCATIONS::loc_processor, 0,
                    level_code::FATAL, err_domain::CORE_MODULE); };
    auto succ_k   = []->KURD_t{ return KURD_t(result_code::SUCCESS, 0, module_code::INTERRUPT,
                    INTERRUPT_SUB_MODULES_LOCATIONS::loc_processor, 0,
                    level_code::INFO, err_domain::CORE_MODULE); };

    KURD_t fail  = fail_k();
    fail.event_code   = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::EVENT_CODE_APS_INIT;
    KURD_t fatal = fatal_k();
    fatal.event_code  = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::EVENT_CODE_APS_INIT;
    KURD_t succ  = succ_k();
    succ.event_code   = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::EVENT_CODE_APS_INIT;

    /* ── 前置检查 ─────────────────────────────────────────────────── */
    if ((uint64_t)&AP_realmode_start % 4096) {
        bsp_kout << now << "APs_bringup: AP_realmode_start not 4K aligned" << kendl;
        fatal.result = result_code::FATAL;
        return fatal;
    }
    if (gAnalyzer == nullptr) {
        bsp_kout << now << "APs_bringup: gAnalyzer is null" << kendl;
        fail.result = result_code::RETRY;
        fail.reason = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::RETRY_REASON_CODE::RETRY_REASON_CODE_DEPENDIES_NOT_INITIALIZED;
        return fail;
    }

    x2apicid_t self_x2apicid = query_x2apicid();
    phyaddr_t  AP_realmode_start_addr = (phyaddr_t)&AP_realmode_start;

    /* ══════════════════════════════════════════════════════════════════
     * 第一遍：扫描 x2APIC ID 空间，定位最大值
     * ══════════════════════════════════════════════════════════════════ */
    uint32_t max_apicid = 0;
    {
        struct FirstPassCtx { uint32_t* max; };
        FirstPassCtx fp{&max_apicid};
        walk_processors(
            [](const APICtb_analyzed_structures::processor_x64_lapic_struct& p, void* ctx) {
                auto* fp = static_cast<FirstPassCtx*>(ctx);
                if (p.apicid > *fp->max) *fp->max = p.apicid;
            },
            &fp);
        /* Ensure BSP is covered even if MADT omits it (shouldn't, but be safe) */
        if (self_x2apicid > max_apicid) max_apicid = self_x2apicid;
    }

    /* ── 分配映射表 ───────────────────────────────────────────────── */
    uint32_t gs_array_size = max_apicid + 1;
    g_gs_by_apicid = new gs_complex_t*[gs_array_size]();
    bsp_kout << now << "APs_bringup: g_gs_by_apicid[" << (uint32_t)gs_array_size
             << "], max_apicid=0x" << HEX << max_apicid << DEC << kendl;

    /* ══════════════════════════════════════════════════════════════════
     * 第二遍：分配 processor_id，填 gs_complex_t* + slot 中的处理器 ID
     * ══════════════════════════════════════════════════════════════════ */
    uint32_t processor_id = 0;   /* BSP = 0 */
    vaddr_t  gs_vbase     = conjucnt_GSs.vbase();
    {
        struct SecondPassCtx {
            uint32_t*    next_id;
            vaddr_t      gs_vbase;
            gs_complex_t** table;
            x2apicid_t   self_apicid;
        };
        SecondPassCtx sp{&processor_id, gs_vbase, g_gs_by_apicid, self_x2apicid};

        /* BSP 优先：固定 processor_id = 0 */
        gs_complex_t* bsp_cx = (gs_complex_t*)(gs_vbase + 0 * GS_COMPLEX_STRIDE);
        sp.table[self_x2apicid] = bsp_cx;
        bsp_cx->slots[PROCESSOR_ID_GS_INDEX] = 0;
        (*sp.next_id)++;  /* next_id = 1 for first AP */

        walk_processors(
            [](const APICtb_analyzed_structures::processor_x64_lapic_struct& p, void* ctx) {
                auto* sp  = static_cast<SecondPassCtx*>(ctx);
                if (p.apicid == sp->self_apicid) return;  /* BSP already done */

                uint32_t pid = *sp->next_id;
                gs_complex_t* cx = (gs_complex_t*)(sp->gs_vbase + pid * GS_COMPLEX_STRIDE);
                sp->table[p.apicid] = cx;
                cx->slots[PROCESSOR_ID_GS_INDEX] = pid;
                bsp_kout << now << "APs_bringup: APIC 0x" << HEX << (uint32_t)p.apicid
                         << DEC << " → proc_id=" << pid << kendl;
                (*sp->next_id)++;
            },
            &sp);
    }
    bsp_kout << now << "APs_bringup: first AP processor_id starts at 1, total "
             << (uint32_t)(processor_id - 1) << " APs" << kendl;

    /* ── SIPI ICR 模板（逐 AP 覆盖 destination） ──────────────────── */
    x2apic::x2apic_icr_t icr_sipi = {
        .param = {
            .vector               = (uint8_t)(AP_realmode_start_addr / 4096),
            .delivery_mode        = 6,
            .destination_mode     = LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
            .reserved1            = 0,
            .level                = 1,
            .trigger_mode         = 0,
            .reserved2            = 0,
            .destination_shorthand = LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::NO_SHORTHAND,
            .reserved3            = 0,
            .destination          = {.raw = 0}
        }
    };

    /* ── INIT IPI（ALL_EXCLUDING_SELF，断言） ──────────────────────── */
    {
        x2apic::x2apic_icr_t icr_init = {
            .param = {
                .vector               = 0,
                .delivery_mode        = 5,
                .destination_mode     = LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
                .reserved1            = 0,
                .level                = 1,
                .trigger_mode         = 0,
                .reserved2            = 0,
                .destination_shorthand = LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::ALL_EXCLUDING_SELF,
                .reserved3            = 0,
                .destination          = {.raw = 0}
            }
        };
        bsp_kout << now << "APs_bringup: INIT IPI all-excluding-self" << kendl;
        x2apic::x2apic_driver::raw_send_ipi(icr_init);
        bsp_kout << now << "APs_bringup: INIT done" << kendl;
        ktime::microsecond_polling_delay_by_hpet(20000);
    }

    /* ── INIT de-assert IPI（ALL_INCLUDING_SELF） ──────────────────── */
    {
        x2apic::x2apic_icr_t icr_init_de_assert = {
            .param = {
                .vector               = 0,
                .delivery_mode        = 5,
                .destination_mode     = LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
                .reserved1            = 0,
                .level                = 0,
                .trigger_mode         = 1,
                .reserved2            = 0,
                .destination_shorthand = LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::ALL_INCLUDING_SELF,
                .reserved3            = 0,
                .destination          = {.raw = 0}
            }
        };
        bsp_kout << now << "APs_bringup: INIT de-assert" << kendl;
        x2apic::x2apic_driver::raw_send_ipi(icr_init_de_assert);
        bsp_kout << now << "APs_bringup: INIT de-assert done" << kendl;
        ktime::microsecond_polling_delay_by_hpet(1000);
    }

    /* ── Checkpoint 观察器枚举 ────────────────────────────────────── */
    enum ap_observe_once_result_t : uint8_t {
        WAIT_THAT_TIME,
        SUCCESS_THAT_TIME,
        FAIL_THAT_TIME,
    };
    enum ap_observe_result_t : uint8_t {
        CHECKPOINT_SUCCESS,
        CHECKPOINT_FAIL,
        CHECKPOINT_TIMEOUT,
    };

    /* ── 观察器 lambda ────────────────────────────────────────────── */
    auto observe_realmode = [](uint32_t word) -> ap_observe_once_result_t {
        if (realmode_enter_checkpoint.success_word == word) return SUCCESS_THAT_TIME;
        return WAIT_THAT_TIME;
    };
    auto observe_pemode = [](uint32_t word) -> ap_observe_once_result_t {
        if (pemode_enter_checkpoint.success_word == word)  return SUCCESS_THAT_TIME;
        if (realmode_enter_checkpoint.failure_flags & 1)   return FAIL_THAT_TIME;
        return WAIT_THAT_TIME;
    };
    auto observe_longmode = [](uint32_t word) -> ap_observe_once_result_t {
        if (longmode_enter_checkpoint.success_word == word) return SUCCESS_THAT_TIME;
        if (pemode_enter_checkpoint.failure_flags & 1)     return FAIL_THAT_TIME;
        return WAIT_THAT_TIME;
    };
    auto observe_finish = [](uint32_t word) -> ap_observe_once_result_t {
        if (init_finish_checkpoint.success_word == word)   return SUCCESS_THAT_TIME;
        if (longmode_enter_checkpoint.failure_flags & 1)   return FAIL_THAT_TIME;
        return WAIT_THAT_TIME;
    };

    /* ── 通用到期轮巡 ──────────────────────────────────────────────── */
    auto wait_for_checkpoint = [](
        uint64_t delay_us,
        ap_observe_once_result_t (*observe)(uint32_t),
        void (*failer_dealing)(),
        uint32_t success_word) -> ap_observe_result_t
    {
        uint64_t deadline = ktime::get_microsecond_stamp() + delay_us;
        while (ktime::get_microsecond_stamp() < deadline) {
            auto r = observe(success_word);
            if (r == SUCCESS_THAT_TIME) return CHECKPOINT_SUCCESS;
            if (r == FAIL_THAT_TIME) { failer_dealing(); return CHECKPOINT_FAIL; }
        }
        return CHECKPOINT_TIMEOUT;
    };

    /* ── 失败诊断 ─────────────────────────────────────────────────── */

    auto fail_dealing = []() {
        bsp_kout << now << "APs_bringup: realmode enter fail" << kendl;
        uint8_t vec = realmode_enter_checkpoint.failure_caused_excption_num;
        bsp_kout << now << "APs_bringup: realmode exception #" << (uint32_t)vec << kendl;
        using namespace AP_Init_error_observing_protocol;
    };
    auto pemode_fail_dealing = []() {
        bsp_kout << now << "APs_bringup: pemode enter fail, flags=0x"
                 << (uint32_t)pemode_enter_checkpoint.failure_flags << kendl;
        if (!(pemode_enter_checkpoint.failure_flags & 2)) return;
        uint8_t vec = pemode_enter_checkpoint.failure_caused_excption_num;
        bsp_kout << now << "APs_bringup: pemode exception #" << (uint32_t)vec << kendl;

        using namespace AP_Init_error_observing_protocol;
        if ((1ULL << vec) & error_code_bitmap) {
            auto* f = (pemode_final_stack_frame_with_errcode*)(uint64_t)pemode_enter_checkpoint.failure_final_stack_top;
            if (f->magic != PE_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC) return;
            bsp_kout << now << "[Pemode Exception Frame with Error Code]" << kendl
                     << "  Magic: 0x"  << f->magic << kendl
                     << "  EFER: 0x"   << f->IA32_EFER << kendl
                     << "  CR4: 0x"    << f->cr4  << kendl
                     << "  CR3: 0x"    << f->cr3  << kendl
                     << "  CR2: 0x"    << f->cr2  << kendl
                     << "  CR0: 0x"    << f->cr0  << kendl
                     << "  GS: 0x"     << f->gs   << kendl
                     << "  FS: 0x"     << f->fs   << kendl
                     << "  SS: 0x"     << f->ss   << kendl
                     << "  DS: 0x"     << f->ds   << kendl
                     << "  ES: 0x"     << f->es   << kendl
                     << "  EDI: 0x"    << f->edi  << kendl
                     << "  ESI: 0x"    << f->esi  << kendl
                     << "  EBP: 0x"    << f->ebp  << kendl
                     << "  ESP: 0x"    << f->esp  << kendl
                     << "  EDX: 0x"    << f->edx  << kendl
                     << "  ECX: 0x"    << f->ecx  << kendl
                     << "  EBX: 0x"    << f->ebx  << kendl
                     << "  EAX: 0x"    << f->eax  << kendl
                     << "  ErrCode: 0x" << f->errcode << kendl
                     << "  CS: 0x"     << f->cs   << kendl
                     << "  EIP: 0x"    << f->eip  << kendl
                     << "  EFLAGS: 0x" << f->eflags << kendl;
        } else {
            auto* f = (pemode_final_stack_frame*)(uint64_t)pemode_enter_checkpoint.failure_final_stack_top;
            if (f->magic != PE_FINAL_STACK_NO_ERRCODE_TOP_MAGIC) return;
            bsp_kout << now << "[Pemode Exception Frame without Error Code]" << kendl
                     << "  Magic: 0x"  << f->magic << kendl
                     << "  EFER: 0x"   << f->IA32_EFER << kendl
                     << "  CR4: 0x"    << f->cr4  << kendl
                     << "  CR3: 0x"    << f->cr3  << kendl
                     << "  CR2: 0x"    << f->cr2  << kendl
                     << "  CR0: 0x"    << f->cr0  << kendl
                     << "  GS: 0x"     << f->gs   << kendl
                     << "  FS: 0x"     << f->fs   << kendl
                     << "  SS: 0x"     << f->ss   << kendl
                     << "  DS: 0x"     << f->ds   << kendl
                     << "  ES: 0x"     << f->es   << kendl
                     << "  EDI: 0x"    << f->edi  << kendl
                     << "  ESI: 0x"    << f->esi  << kendl
                     << "  EBP: 0x"    << f->ebp  << kendl
                     << "  ESP: 0x"    << f->esp  << kendl
                     << "  EDX: 0x"    << f->edx  << kendl
                     << "  ECX: 0x"    << f->ecx  << kendl
                     << "  EBX: 0x"    << f->ebx  << kendl
                     << "  EAX: 0x"    << f->eax  << kendl
                     << "  CS: 0x"     << f->cs   << kendl
                     << "  EIP: 0x"    << f->eip  << kendl
                     << "  EFLAGS: 0x" << f->eflags << kendl;
        }
    };

    /* ── 逐 AP 唤醒循环 ─────────────────────────────────────────────
     * 每个 AP 的 processor_id 已在第二遍扫描时从 gs_complex_t slot 读回
     * ──────────────────────────────────────────────────────────────── */
    uint64_t ipi_fail_count = 0;

    for (auto it = gAnalyzer->processor_x64_list->begin();
         it != gAnalyzer->processor_x64_list->end(); ++it)
    {
        const auto& proc = *it;
        if (proc.apicid == self_x2apicid) continue;   // 跳过 BSP

        /* 从映射表查 processor_id（由第二遍扫描写入 slot） */
        gs_complex_t* ap_cx = g_gs_by_apicid[proc.apicid];
        uint32_t pid = ap_cx->slots[PROCESSOR_ID_GS_INDEX];

        icr_sipi.param.destination.raw = proc.apicid;
        assigned_processor_id = pid;
        asm volatile("sfence");

        bsp_kout << now << "APs_bringup: SIPI to APIC " << proc.apicid
                 << " (id=" << pid << ")" << kendl;
        GfxPrim::Flush();

        x2apic::x2apic_driver::raw_send_ipi(icr_sipi);

        /* realmode enter (1ms) — SIPI 可能未送达，超时则跳过此 AP */
        {
            auto st = wait_for_checkpoint(1000, observe_realmode, fail_dealing, pid);
            if (st == CHECKPOINT_TIMEOUT) {
                bsp_kout << now << "APs_bringup: realmode timeout for APIC "
                         << proc.apicid << kendl;
                ipi_fail_count++;
                continue;
            }
        }

        /* pemode enter (1ms) */
        {
            auto st = wait_for_checkpoint(1000, observe_pemode, pemode_fail_dealing, proc.apicid);
            if (st == CHECKPOINT_FAIL  || st == CHECKPOINT_TIMEOUT) {
                fatal.reason = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::FATAL_REASON::AP_STAGE_FAIL;
                return fatal;
            }
        }

        /* longmode enter (1ms) */
        {
            auto st = wait_for_checkpoint(1000, observe_longmode, fail_dealing, ~pid);
            if (st == CHECKPOINT_FAIL  || st == CHECKPOINT_TIMEOUT) {
                fatal.reason = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::FATAL_REASON::AP_STAGE_FAIL;
                return fatal;
            }
        }

        /* init finish (2s) */
        {
            auto st = wait_for_checkpoint(2000000, observe_finish, fail_dealing, ~proc.apicid);
            if (st == CHECKPOINT_FAIL  || st == CHECKPOINT_TIMEOUT) {
                fatal.reason = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::FATAL_REASON::AP_STAGE_FAIL;
                return fatal;
            }
        }
    }

    if (ipi_fail_count) {
        succ.result = result_code::PARTIAL_SUCCESS;
        succ.reason = INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::PARTIAL_SUCCESS_CODE::PARTIAL_SUCCESS_CODE_SOME_APS_IPI_TIME_OUT;
    }
    return succ;
}
