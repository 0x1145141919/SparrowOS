#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include <util/arch/x86-64/cpuid_intel.h>

// ============================================================
// 命令特定数据结构
// ============================================================

// FLUSH: opcode 00h — 无数据指针，无命令特定字段

// -------- WRITE ZEROES CDW12 (opcode 08h) --------
// Figure 82: LR(31), FUA(30), PRINFO(29:26), DEAC(25), STC(24),
//            NSZ(23), DTYPE(22:20), CETYPE(19:16), NLB(15:00)
union wz_cdw12_t {
    uint32_t raw;
    struct {
        uint32_t nlb     : 16;  // Number of Logical Blocks (0's based)
        uint32_t cetype  :  4;  // Command Extension Type
        uint32_t dtype   :  3;  // Directive Type (bits 22:20)
        uint32_t nsz     :  1;  // Namespace Zeroes (bit 23)
        uint32_t stc     :  1;  // Storage Tag Check (must be 0)
        uint32_t deac    :  1;  // Deallocate (bit 25)
        uint32_t prinfo  :  4;  // Protection Information (bits 29:26)
        uint32_t fua     :  1;  // Force Unit Access
        uint32_t lr      :  1;  // Limited Retry
    } __attribute__((packed));
};

// -------- DATASET MANAGEMENT Range Definition (opcode 09h) --------
// Figure 46: 每 range 16 字节
//            Bytes 03:00 = CATTR, 07:04 = LLB (1's based), 15:08 = SLBA
struct dsm_range_t {
    uint32_t context_attributes;  // CATTR
    uint32_t length_in_lbs;       // LLB, 1's based
    uint64_t starting_lba;        // SLBA
} __attribute__((packed));

// -------- DSM CDW10 (Figure 44) --------
union dsm_cdw10_t {
    uint32_t raw;
    struct {
        uint32_t nr      :  8;  // Number of Ranges (0's based)
        uint32_t rsvd    : 24;
    } __attribute__((packed));
};

// -------- DSM CDW11 (Figure 45) --------
union dsm_cdw11_t {
    uint32_t raw;
    struct {
        uint32_t rsvd0   :  1;
        uint32_t idw     :  1;  // Integral Dataset for Write
        uint32_t ad      :  1;  // Attribute - Deallocate
        uint32_t rsvd1   : 29;
    } __attribute__((packed));
};

// ============================================================
// io_cmd_result_kurd: 复用 io_rwc.cpp 的同名模式，此处重定义
// ============================================================
static KURD_t fdw_result_kurd(NVMe::command::complete_command_common cqe,
                               uint8_t event_code)
{
    if (!NVMe::status::is_error(cqe.fields.status)) {
        return KURD_t(
            result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            event_code,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    KURD_t kurd(
        result_code::FAIL, 0,
        module_code::DEVICE, DEVICES_locs::NVMe,
        event_code,
        level_code::ERROR, err_domain::CORE_MODULE);
    kurd.reason = cqe.fields.status;
    return kurd;
}

// ============================================================
// param_error helpers
// ============================================================
static KURD_t param_error_flush(uint16_t reason)
{
    return KURD_t(result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::flush,
        level_code::ERROR, err_domain::CORE_MODULE);
}
static KURD_t param_error_discard(uint16_t reason)
{
    return KURD_t(result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::discard,
        level_code::ERROR, err_domain::CORE_MODULE);
}
static KURD_t param_error_wz(uint16_t reason)
{
    return KURD_t(result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::wz,
        level_code::ERROR, err_domain::CORE_MODULE);
}

// ============================================================
// submit_to_io_queue: 提交命令到当前 CPU 绑定的 I/O 队列并等待完成
// ============================================================
static NVMe::command::complete_command_common
submit_to_io_queue(NVMe_Controller* ctrl,
                   NVMe::command::submit_command_common cmd,
                   KURD_t& kurd)
{
    uint32_t cpu_id = fast_get_processor_id();
    uint16_t qid    = cpu_id + 1;
    return ctrl->cmd_submit_and_process(qid, cmd, kurd);
}

// ============================================================
// NVMe_Controller::flush (static)
//
// 刷新指定 Namespace 的所有已写入数据到 NVM。
// opcode 00h, 无数据传输, 仅需 NSID。
//
// 参数:
//   dev   — BlockDevice（对应一个 namespace）
//   flags — 控制标志（当前未使用）
// ============================================================
KURD_t NVMe_Controller::flush(BlockDevice* dev, uint64_t flags)
{
    (void)flags;

    auto* priv = static_cast<NVMe_device_private*>(dev->private_data);
    if (!priv) return param_error_flush(0x1);

    uint32_t controller_id = priv->controller_id;
    uint32_t nsid          = priv->nsid;

    if (controller_id >= controllers_count || !node_array[controller_id].controller)
        return param_error_flush(0x2);

    NVMe_Controller* ctrl = node_array[controller_id].controller;

    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::io_opcode::FLUSH;
    cmd.fiedls.nsid   = nsid;

    KURD_t kurd;
    NVMe::command::complete_command_common cqe =
        submit_to_io_queue(ctrl, cmd, kurd);

    return fdw_result_kurd(cqe, DEVICES_locs::NVMe_events::flush);
}

// ============================================================
// NVMe_Controller::write_zero (static)
//
// 将指定 LBA 区间写零（可选 deallocate）。
// opcode 08h, 无数据传输, 仅需 SLBA + NLB。
//
// 参数:
//   dev      — BlockDevice
//   interval — LBA 区间（起始 + 数量）
//   flags    — 控制标志（当前未使用）
// ============================================================
KURD_t NVMe_Controller::write_zero(BlockDevice* dev,
                                    LBA_interval_t interval,
                                    uint64_t flags)
{
    (void)flags;

    auto* priv = static_cast<NVMe_device_private*>(dev->private_data);
    if (!priv) return param_error_wz(0x1);

    uint32_t controller_id = priv->controller_id;
    uint32_t nsid          = priv->nsid;

    if (controller_id >= controllers_count || !node_array[controller_id].controller)
        return param_error_wz(0x2);

    NVMe_Controller* ctrl = node_array[controller_id].controller;

    uint64_t count = interval.LBA_count;
    if (count == 0) {
        return KURD_t(result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::wz,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    // LBA 范围
    if (interval.start + count > dev->sector_count)
        return param_error_wz(0x3);

    // NLB 上限
    if (count - 1 > 0xFFFF)
        return param_error_wz(0x8);

    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::io_opcode::WRITE_ZEROES;
    cmd.fiedls.nsid   = nsid;
    cmd.dwords[10]    = (uint32_t)(interval.start & 0xFFFFFFFF);
    cmd.dwords[11]    = (uint32_t)(interval.start >> 32);

    wz_cdw12_t cdw12{};
    cdw12.nlb = (uint16_t)(count - 1);
    // DEAC=0（写零，不 deallocate）
    // NSZ=0, STC=0, PRCHK=0（默认零值）
    cmd.dwords[12] = cdw12.raw;

    KURD_t kurd;
    NVMe::command::complete_command_common cqe =
        submit_to_io_queue(ctrl, cmd, kurd);

    return fdw_result_kurd(cqe, DEVICES_locs::NVMe_events::wz);
}

// ============================================================
// NVMe_Controller::discard (static)
//
// 通过 Dataset Management (opcode 09h) 的 Deallocate 属性释放 LBA。
//
// 参数:
//   dev      — BlockDevice
//   interval — 要释放的 LBA 区间
//   flags    — 控制标志（当前未使用）
//
// 流程:
//   1. 分配 1 页物理内存作为 Range Definition 列表
//   2. 填充 dsm_range_t（SLBA, LLB, CATTR=0）
//   3. 构建 PRP 指向该页
//   4. 发送 DSM 命令 (AD=1, NR=0)
//   5. 等待完成 → 清理 PRP + 释放 Range 页
// ============================================================
KURD_t NVMe_Controller::discard(BlockDevice* dev,
                                 LBA_interval_t interval,
                                 uint64_t flags)
{
    (void)flags;

    auto* priv = static_cast<NVMe_device_private*>(dev->private_data);
    if (!priv) return param_error_discard(0x1);

    uint32_t controller_id = priv->controller_id;
    uint32_t nsid          = priv->nsid;

    if (controller_id >= controllers_count || !node_array[controller_id].controller)
        return param_error_discard(0x2);

    NVMe_Controller* ctrl = node_array[controller_id].controller;

    uint64_t count = interval.LBA_count;
    if (count == 0) {
        return KURD_t(result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::discard,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    // LBA 范围
    if (interval.start + count > dev->sector_count)
        return param_error_discard(0x3);

    // ---- 分配 Range 列表缓冲区（1 页 = 最多 256 个 range，此处只用一个） ----
    KURD_t kurd;
    void* range_va = __wrapped_pgs_valloc(&kurd, 1,
                                           page_state_t::kernel_pinned, 12);
    if (!range_va || error_kurd(kurd))
        return param_error_discard(0x4);
    ksetmem_8(range_va, 0, 4096);

    phyaddr_t range_pa = 0;
    kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)range_va, range_pa);
    if (error_kurd(kurd) || range_pa == 0) {
        __wrapped_pgs_vfree(range_va, 1);
        return param_error_discard(0x5);
    }

    // ---- 填充 Range 0 ----
    auto* range = static_cast<dsm_range_t*>(range_va);
    range->starting_lba        = interval.start;
    range->length_in_lbs       = (uint32_t)count;  // 1's based
    range->context_attributes  = 0;

    // ---- MPS (Memory Page Size) ----
    uint32_t cc       = ctrl->head_regs->controller_configuration;
    uint32_t mps_shift = 12 + ((cc >> 7) & 0xF);

    // ---- 构建 PRP（1 个 MPS 页） ----
    prp_root_t prp_root{};
    kurd = build_PRP_root(range_pa, 1, mps_shift, &prp_root, kurd);
    if (error_kurd(kurd)) {
        __wrapped_pgs_vfree(range_va, 1);
        return param_error_discard(0x6);
    }

    // ---- 构造 DSM 命令 ----
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::io_opcode::DATASET_MANAGEMENT;
    cmd.fiedls.nsid   = nsid;
    cmd.fiedls.DPTR1  = prp_root.prp1;
    cmd.fiedls.DPTR2  = prp_root.prp2;

    dsm_cdw10_t cdw10{};
    cdw10.nr = 0;  // 1 range (0's based)
    cmd.dwords[10] = cdw10.raw;

    dsm_cdw11_t cdw11{};
    cdw11.ad = 1;  // Deallocate attribute
    cmd.dwords[11] = cdw11.raw;

    // ---- 提交 ----
    NVMe::command::complete_command_common cqe =
        submit_to_io_queue(ctrl, cmd, kurd);

    // ---- 清理 ----
    {
        KURD_t dk;
        destroy_PRP_root(prp_root, mps_shift, dk);
    }
    __wrapped_pgs_vfree(range_va, 1);

    return fdw_result_kurd(cqe, DEVICES_locs::NVMe_events::discard);
}
