#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>

static NVMe::command_result_t do_identify(NVMe_Controller* ctrl,
                                           uint32_t nsid, uint32_t cns,
                                           phyaddr_t buf_pa)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::IDENTIFY;
    cmd.fiedls.nsid   = nsid;
    cmd.fiedls.DPTR1  = buf_pa;
    cmd.dwords[10]    = cns;

    return ctrl->cmd_submit_and_process(0, cmd);
}

NVMe::command_result_t NVMe_Controller::identify_ctrl(phyaddr_t buf_pa)
{
    return do_identify(this, 0, 0x01, buf_pa);  // CNS 01h
}

NVMe::command_result_t NVMe_Controller::identify_ns(uint32_t nsid, phyaddr_t buf_pa)
{
    return do_identify(this, nsid, 0x00, buf_pa);  // CNS 00h
}

NVMe::command_result_t NVMe_Controller::identify_ns_list(uint32_t nsid, phyaddr_t buf_pa)
{
    return do_identify(this, nsid, 0x02, buf_pa);  // CNS 02h
}

NVMe::command_result_t NVMe_Controller::identify_ns_indep(uint32_t nsid, phyaddr_t buf_pa)
{
    return do_identify(this, nsid, 0x08, buf_pa);  // CNS 08h
}

NVMe::command_result_t NVMe_Controller::identify_primary_ctrl_caps(uint16_t cntid,
                                                                     phyaddr_t buf_pa)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::IDENTIFY;
    cmd.fiedls.nsid   = 0;
    cmd.fiedls.DPTR1  = buf_pa;
    // CDW10: bits[7:0] = CNS, bits[31:16] = CNTID (Figure 308)
    cmd.dwords[10]    = 0x14 | (uint32_t(cntid) << 16);  // CNS 14h + CNTID
    cmd.dwords[11]    = 0;  // reserved for CNS 14h

    return cmd_submit_and_process(0, cmd);
}
