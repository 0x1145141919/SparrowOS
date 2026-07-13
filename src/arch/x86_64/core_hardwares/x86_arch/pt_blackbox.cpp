#include "arch/x86_64/intel_processor_trace.h"
#include "util/OS_utils.h"            // rdmsr, wrmsr_func
#include "util/arch/x86-64/cpuid_intel.h"  // fast_get_processor_id
#include "exec_env_detect.h"               // g_env, ENV_TCG
#include "abi/os_error_definitions.h"
#include "memory/memory_base.h"
#include "memory/all_pages_arr.h"
#include "memory/AddresSpace.h"

// ─────────────────────────────────────────────────────────────────────
// 所有函数只操作全局数组中当前 CPU 自己的槽位, 不跨核, 不加锁.
// ─────────────────────────────────────────────────────────────────────

pt_blackbox* global_pt_blackboxes;
// ── prepare ──────────────────────────────────────────────────────────
KURD_t prepare_blackbox(pt_blackbox *bb)
{
    if (!bb)
        return set_result_fail_and_error_level(empty_kurd);

    // 幂等: 已经 prepared 或 running 都是"已就绪"
    if (bb->state != pt_blackbox_state::not_prepared)
        return KURD_t(result_code::SUCCESS, 0,
                      module_code::HARDWARE_DEBUG, 0, 0,
                      level_code::INFO, err_domain::ARCH);

    // TCG 下永久禁用
    if (g_env == ENV_TCG)
        return set_result_fail_and_error_level(empty_kurd);

    // 分配 8 MB 物理+虚拟连续缓冲区, 按大小对齐 (ToPA 要求)
    uint32_t buf_size = default_PT_buffer_size;
    uint32_t pages    = buf_size >> 12;
    uint8_t  align_log2 = 0;
    {
        uint32_t s = buf_size;
        while (s > 1) { s >>= 1; ++align_log2; }
    }

    KURD_t alloc_kurd;
    void* va = __wrapped_pgs_valloc(&alloc_kurd, pages,
                                    page_state_t::kernel_pinned, align_log2);
    if (!va)
        return alloc_kurd;

    phyaddr_t pa;
    KURD_t trans_kurd = KspacePageTable::v_to_phyaddrtraslation(
        reinterpret_cast<vaddr_t>(va), pa);
    if (!success_all_kurd(trans_kurd))
        return trans_kurd;

    // 填入 vm_interval
    bb->ring_interval.vpn     = reinterpret_cast<vaddr_t>(va) >> 12;
    bb->ring_interval.ppn     = pa >> 12;
    bb->ring_interval.npages  = pages;
    bb->ring_interval.access  = {};  // 默认权限

    bb->tail_offset = 0;
    bb->state       = pt_blackbox_state::prepared;

    return KURD_t(result_code::SUCCESS, 0,
                  module_code::HARDWARE_DEBUG, 0, 0,
                  level_code::INFO, err_domain::ARCH);
}

// ── recycle ──────────────────────────────────────────────────────────
KURD_t recycle_blackbox(pt_blackbox *bb)
{
    if (!bb)
        return set_result_fail_and_error_level(empty_kurd);

    // 幂等
    if (bb->state == pt_blackbox_state::not_prepared)
        return KURD_t(result_code::SUCCESS, 0,
                      module_code::HARDWARE_DEBUG, 0, 0,
                      level_code::INFO, err_domain::ARCH);

    // running 态必须先 disable
    if (bb->state == pt_blackbox_state::running)
        return set_result_fail_and_error_level(empty_kurd);

    // state == prepared: 释放缓冲区
    uint64_t npages = bb->ring_interval.npages;
    void* vbase = reinterpret_cast<void*>(bb->ring_interval.vbase());

    KURD_t fr = __wrapped_pgs_vfree(vbase, npages);
    if (error_kurd(fr))
        return fr;

    // 清零 descriptor
    bb->ring_interval = {};
    bb->tail_offset   = 0;
    bb->state         = pt_blackbox_state::not_prepared;

    return KURD_t(result_code::SUCCESS, 0,
                  module_code::HARDWARE_DEBUG, 0, 0,
                  level_code::INFO, err_domain::ARCH);
}

// ── enable ───────────────────────────────────────────────────────────
void enable_blackbox(pt_blackbox *bb)
{
    if (!bb || bb->state != pt_blackbox_state::prepared)
        return;

    auto buf_size = bb->ring_interval.byte_cnt();
    auto pbase    = bb->ring_interval.pbase();
    auto mask     = pt_output_mask_from_size(buf_size);

    wrmsr_func(pt_msr::OUTPUT_BASE,       pbase);
    wrmsr_func(pt_msr::OUTPUT_MASK_PTRS,  static_cast<uint64_t>(mask));

    // TraceEn | OS | BranchEn  (ToPA=0, User=0, CR3Filter=0)
    uint64_t ctl = pt_ctl::TraceEn | pt_ctl::OS | pt_ctl::BranchEn|pt_ctl::TSCEn;
    wrmsr_func(pt_msr::CTL, ctl);

    bb->tail_offset = 0;
    bb->state       = pt_blackbox_state::running;
}

// ── disable ──────────────────────────────────────────────────────────
void disable_blackbox(pt_blackbox *bb)
{
    if (!bb || bb->state != pt_blackbox_state::running)
        return;

    // 清 TraceEn → flush 内部 buffer
    wrmsr_func(pt_msr::CTL, 0);

    // 读 OutputOffset
    uint64_t mask_ptrs = rdmsr(pt_msr::OUTPUT_MASK_PTRS);
    bb->tail_offset    = static_cast<uint32_t>(mask_ptrs >> 32);
    bb->state          = pt_blackbox_state::prepared;
}

// ── calibrate_offset ────────────────────────────────────────────────
void calibrate_offset(pt_blackbox *bb)
{
    if (!bb || bb->state != pt_blackbox_state::prepared)
        return;

    // 仅合法性钳位; 环形缓冲区 wrap 由离线解码器处理
    auto buf_size = bb->ring_interval.byte_cnt();
    if (bb->tail_offset > buf_size)
        bb->tail_offset = buf_size;
}
