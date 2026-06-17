/**
 * @file interrupt_kshell_commands.cpp
 * @brief kshell 中断递送框架诊断命令
 *
 * s_int_spec  — 软中断函数表查询
 * out_int_spec — 硬件中断函数表查询
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "util/kptrace.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/GS_complex.h"

using namespace kio;

extern soft_interrupt_func_t soft_interrupt_functions[256];
extern uint32_t logical_processor_count;
extern vm_interval conjucnt_GSs;

static KURD_t cmd_ok(uint8_t evt) {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, evt, level_code::INFO, err_domain::CORE_MODULE};
}

/* print symbol name for a kernel address, or "<no-sym>" */
static void print_sym_name(uint64_t addr) {
    const symbol_entry* se = ksymmanager::get_entry_near_addr(addr);
    if (se && se->address == addr) {
        bsp_kout << se->name;
    } else {
        bsp_kout << "<no-sym>";
    }
}

/* ────────────────────────────────────────────────────────────────
 * s_int_spec [vec]  — 软中断表
 * ──────────────────────────────────────────────────────────────── */
KURD_t cmd_s_int_spec(const line_t* line) {
    KURD_t ok = cmd_ok(INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE);
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count == 2) {
        /* single vector */
        uint64_t vec;
        if (!token_to_uint64(line->tokens[1], &vec) || vec > 255) {
            bsp_kout << "s_int_spec: invalid vector (0-255)" << kendl;
            return ok;
        }
        bsp_kout << "s_int[" << (uint32_t)vec << "] = 0x"
                 << (uint64_t)soft_interrupt_functions[vec] << "  ";
        if (soft_interrupt_functions[vec])
            print_sym_name((uint64_t)soft_interrupt_functions[vec]);
        else
            bsp_kout << "(null)";
        bsp_kout << kendl;
        return ok;
    }

    /* full dump */
    bsp_kout << "=== soft-interrupt function table ===" << kendl;
    bool any = false;
    for (uint16_t v = 32; v <= 255; v++) {
        if (soft_interrupt_functions[v]) {
            bsp_kout << "  [" << (uint32_t)v << "] 0x"
                     << (uint64_t)soft_interrupt_functions[v] << "  ";
            print_sym_name((uint64_t)soft_interrupt_functions[v]);
            bsp_kout << kendl;
            any = true;
        }
    }
    if (!any)
        bsp_kout << "  (empty)" << kendl;
    return ok;
}

/* ────────────────────────────────────────────────────────────────
 * out_int_spec [proc_id] [vec]  — 硬件中断表
 * ──────────────────────────────────────────────────────────────── */
KURD_t cmd_out_int_spec(const line_t* line) {
    KURD_t ok = cmd_ok(INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE);

    if (line->token_count >= 2) {
        uint64_t pid;
        if (!token_to_uint64(line->tokens[1], &pid) ||
            pid >= logical_processor_count) {
            bsp_kout << "out_int_spec: invalid processor_id (0-"
                     << (uint32_t)(logical_processor_count - 1) << ")" << kendl;
            return ok;
        }

        gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + pid * GS_COMPLEX_STRIDE);
        interrupt_token_t* slice = cx->tokens;

        if (line->token_count >= 3) {
            /* preciser: pid + vec */
            uint64_t vec;
            if (!token_to_uint64(line->tokens[2], &vec) || vec > 255) {
                bsp_kout << "out_int_spec: invalid vector (0-255)" << kendl;
                return ok;
            }
            bsp_kout << "out_int[pid=" << (uint32_t)pid
                     << "][" << (uint32_t)vec << "] = 0x"
                     << HEX << (uint64_t)slice[vec].func << DEC << "  ";
            if (slice[vec].func)
                print_sym_name((uint64_t)slice[vec].func);
            else
                bsp_kout << "(null)";
            bsp_kout << kendl;
        } else {
            /* all vecs on this processor */
            bsp_kout << "=== hard-interrupt table for processor "
                     << (uint32_t)pid << " ===" << kendl;
            bool any = false;
            for (uint16_t v = 32; v <= 255; v++) {
                if (slice[v].func) {
                    bsp_kout << "  [" << (uint32_t)v << "] 0x"
                             << HEX<<(uint64_t)slice[v].func <<DEC<< "  ";
                    print_sym_name((uint64_t)slice[v].func);
                    bsp_kout << kendl;
                    any = true;
                }
            }
            if (!any)
                bsp_kout << "  (empty)" << kendl;
        }
        return ok;
    }

    /* full dump: all processors, all vecs */
    uint32_t pc = logical_processor_count;
    for (uint32_t p = 0; p < pc; p++) {
        bsp_kout << "=== hard-interrupt table for processor " << p << " ===" << kendl;
        gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + p * GS_COMPLEX_STRIDE);
        interrupt_token_t* slice = cx->tokens;
        bool any = false;
        for (uint16_t v = 32; v <= 255; v++) {
            if (slice[v].func) {
                bsp_kout << "  [" << (uint32_t)v << "] 0x"
                         << HEX << (uint64_t)slice[v].func << DEC << "  ";
                print_sym_name((uint64_t)slice[v].func);
                bsp_kout << kendl;
                any = true;
            }
        }
        if (!any)
            bsp_kout << "  (empty)" << kendl;
    }
    return ok;
}
