#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>

static KURD_t make_set_kurd(NVMe::command_result_t r)
{
    return empty_kurd;
}

static KURD_t make_get_kurd(NVMe::command_result_t r,
                             uint32_t cqe_dword0)
{
    return empty_kurd;
}

NVMe::command_result_t
NVMe_Controller::get_features_cmd(uint8_t fid, uint8_t sel)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::GET_FEATURES;
    cmd.fiedls.nsid   = 0;
    cmd.dwords[10]    = fid | (uint32_t(sel) << 8);

    return cmd_submit_and_process(0, cmd);
}

NVMe::command_result_t
NVMe_Controller::set_features_cmd(uint8_t fid, uint32_t cdw11,
                                   phyaddr_t buf_pa)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::SET_FEATURES;
    cmd.fiedls.nsid   = 0;
    cmd.dwords[10]    = fid;
    cmd.dwords[11]    = cdw11;
    cmd.fiedls.DPTR1  = buf_pa;

    return cmd_submit_and_process(0, cmd);
}

// ============================================================
// 便捷包装器：Number of Queues
// ============================================================
KURD_t NVMe_Controller::get_features_num_queues(KURD_t& kurd)
{
    NVMe::command_result_t r =
        get_features_cmd(NVMe::features::FID_NUMBER_OF_QUEUES,
                         NVMe::features::GET_SEL_CURRENT);
    if (NVMe::status::is_error(r.fields.status)) {
        return make_get_kurd(r, 0);
    }
    return make_get_kurd(r, r.dwords[0]);
}

KURD_t NVMe_Controller::set_features_num_queues(uint16_t nsqr, uint16_t ncqr,
                                                  KURD_t& kurd)
{
    NVMe::features_detail::nq_cdw11_t cdw11;
    cdw11.nsqr = nsqr;
    cdw11.ncqr = ncqr;

    NVMe::command_result_t r =
        set_features_cmd(NVMe::features::FID_NUMBER_OF_QUEUES,
                         cdw11.raw, 0);
    if (NVMe::status::is_error(r.fields.status)) {
        return make_set_kurd(r);
    }
    return empty_kurd;
}

// ============================================================
// 便捷包装器：Interrupt Coalescing
// ============================================================
KURD_t NVMe_Controller::get_features_int_coalescing(KURD_t& kurd)
{
    NVMe::command_result_t r =
        get_features_cmd(NVMe::features::FID_INTERRUPT_COALESCING,
                         NVMe::features::GET_SEL_CURRENT);
    return make_get_kurd(r, r.dwords[0]);
}

KURD_t NVMe_Controller::set_features_int_coalescing(uint8_t thr, uint8_t time,
                                                      KURD_t& kurd)
{
    NVMe::features_detail::int_coal_cdw11_t cdw11;
    cdw11.thr  = thr;
    cdw11.time = time;

    NVMe::command_result_t r =
        set_features_cmd(NVMe::features::FID_INTERRUPT_COALESCING,
                         cdw11.raw, 0);
    if (NVMe::status::is_error(r.fields.status)) {
        return make_set_kurd(r);
    }
    return empty_kurd;
}

// ============================================================
// 便捷包装器：Interrupt Vector Config
// ============================================================
KURD_t NVMe_Controller::get_features_int_vector_config(KURD_t& kurd)
{
    NVMe::command_result_t r =
        get_features_cmd(NVMe::features::FID_INTERRUPT_VECTOR_CONFIG,
                         NVMe::features::GET_SEL_CURRENT);
    return make_get_kurd(r, r.dwords[0]);
}

KURD_t NVMe_Controller::set_features_int_vector_config(uint16_t iv,
                                                         KURD_t& kurd)
{
    NVMe::features_detail::iv_config_cdw11_t cdw11;
    cdw11.iv = iv;
    cdw11.cd = 0;

    NVMe::command_result_t r =
        set_features_cmd(NVMe::features::FID_INTERRUPT_VECTOR_CONFIG,
                         cdw11.raw, 0);
    if (NVMe::status::is_error(r.fields.status)) {
        return make_set_kurd(r);
    }
    return empty_kurd;
}

// ============================================================
// 便捷包装器：Async Event Config
// ============================================================
KURD_t NVMe_Controller::get_features_async_event_config(KURD_t& kurd)
{
    NVMe::command_result_t r =
        get_features_cmd(NVMe::features::FID_ASYNC_EVENT_CONFIG,
                         NVMe::features::GET_SEL_CURRENT);
    return make_get_kurd(r, r.dwords[0]);
}

KURD_t NVMe_Controller::set_features_async_event_config(
    uint8_t sm, uint8_t err, uint8_t ns, uint8_t fw, uint8_t tel, KURD_t& kurd)
{
    NVMe::features_detail::aer_cfg_cdw11_t cdw11;
    cdw11.sm  = sm;
    cdw11.err = err;
    cdw11.ns  = ns;
    cdw11.fw  = fw;
    cdw11.tel = tel;

    NVMe::command_result_t r =
        set_features_cmd(NVMe::features::FID_ASYNC_EVENT_CONFIG,
                         cdw11.raw, 0);
    if (NVMe::status::is_error(r.fields.status)) {
        return make_set_kurd(r);
    }
    return empty_kurd;
}

// ============================================================
// 便捷包装器：Host Controller Thermal Management
// ============================================================
KURD_t NVMe_Controller::get_features_hctm(KURD_t& kurd)
{
    NVMe::command_result_t r =
        get_features_cmd(NVMe::features::FID_HOST_CTRL_THERMAL_MGMT,
                         NVMe::features::GET_SEL_CURRENT);
    return make_get_kurd(r, r.dwords[0]);
}

KURD_t NVMe_Controller::set_features_hctm(uint16_t tmt2, uint16_t tmt1,
                                            KURD_t& kurd)
{
    NVMe::features_detail::hctm_cdw11_t cdw11;
    cdw11.tmt2 = tmt2;
    cdw11.tmt1 = tmt1;

    NVMe::command_result_t r =
        set_features_cmd(NVMe::features::FID_HOST_CTRL_THERMAL_MGMT,
                         cdw11.raw, 0);
    if (NVMe::status::is_error(r.fields.status)) {
        return empty_kurd;
    }
    return empty_kurd;
}
