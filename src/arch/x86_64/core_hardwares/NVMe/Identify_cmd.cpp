#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>

static KURD_t make_identify_kurd(NVMe::command::complete_command_common cqe,
                                  uint16_t fail_reason)
{
    return empty_kurd;
}

static KURD_t do_identify(NVMe_Controller* ctrl,
                           uint32_t nsid, uint32_t cns,
                           phyaddr_t buf_pa,
                           KURD_t& kurd)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::IDENTIFY;
    cmd.fiedls.nsid   = nsid;
    cmd.fiedls.DPTR1  = buf_pa;
    cmd.dwords[10]    = cns;

    NVMe::command::complete_command_common cqe =
        ctrl->ADMIN_cmd_submit_and_process(cmd, kurd);

    if (NVMe::status::is_error(cqe.fields.status)) {
        return empty_kurd;
    }
    return empty_kurd;
}

KURD_t NVMe_Controller::identify_ctrl(phyaddr_t buf_pa, KURD_t& kurd)
{
    return do_identify(this, 0, 0x01, buf_pa, kurd);  // CNS 01h
}

KURD_t NVMe_Controller::identify_ns(uint32_t nsid, phyaddr_t buf_pa, KURD_t& kurd)
{
    return do_identify(this, nsid, 0x00, buf_pa, kurd);  // CNS 00h
}

KURD_t NVMe_Controller::identify_ns_list(uint32_t nsid, phyaddr_t buf_pa, KURD_t& kurd)
{
    return do_identify(this, nsid, 0x02, buf_pa, kurd);  // CNS 02h
}

KURD_t NVMe_Controller::identify_ns_indep(uint32_t nsid, phyaddr_t buf_pa, KURD_t& kurd)
{
    return do_identify(this, nsid, 0x08, buf_pa, kurd);  // CNS 08h
}

KURD_t NVMe_Controller::identify_primary_ctrl_caps(uint16_t cntid,
                                                     phyaddr_t buf_pa,
                                                     KURD_t& kurd)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::IDENTIFY;
    cmd.fiedls.nsid   = 0;
    cmd.fiedls.DPTR1  = buf_pa;
    // CDW10: bits[7:0] = CNS, bits[31:16] = CNTID (Figure 308)
    cmd.dwords[10]    = 0x14 | (uint32_t(cntid) << 16);  // CNS 14h + CNTID
    cmd.dwords[11]    = 0;  // reserved for CNS 14h

    NVMe::command::complete_command_common cqe =
        ADMIN_cmd_submit_and_process(cmd, kurd);

    if (NVMe::status::is_error(cqe.fields.status)) {
        return empty_kurd;
    }
    return empty_kurd;
}
