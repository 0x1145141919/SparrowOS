#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include <util/arch/x86-64/cpuid_intel.h>

// ============================================================
// I/O 命令 CDW12 — NVM Command Set 通用字段
//   Read  (opcode 02h): Section 3.3.4, Figure 52-53
//   Write (opcode 01h): Section 3.3.6, Figure 69-70
//   Cmp   (opcode 05h): Section 3.3.1, Figure 25-26
// ============================================================
union io_cdw12_t {
    uint32_t raw;
    struct {
        uint32_t nlb     : 16;  // Number of Logical Blocks (0's based)
        uint32_t cetype  :  4;  // Command Extension Type
        uint32_t dtype   :  4;  // Directive Type (Write 有; Read/Cmp reserved)
        uint32_t stc     :  1;  // Storage Tag Check
        uint32_t rsvd25  :  1;
        uint32_t prinfo  :  4;  // Protection Information Action + Check (bits 29:26)
        uint32_t fua     :  1;  // Force Unit Access
        uint32_t lr      :  1;  // Limited Retry
    } __attribute__((packed));
};


// ============================================================
// read_param_error: 参数校验失败时的 KURD
// ============================================================
static KURD_t read_param_error(uint16_t reason)
{
    return KURD_t(
        result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::Read,
        level_code::ERROR, err_domain::CORE_MODULE);
}

// ============================================================
// NVMe_Controller::read (static)
//
// BlockDevice 回调，通过 NVMe READ 命令从指定 Namespace 读取数据。
//
// 调用者保证：
//   - buf.pbase 指向**物理连续**的缓冲区
//   - buf.size  描述缓冲区字节容量
//
// 函数内校验：
//   1. interval.LBA_count 非零检查
//   2. LBA 范围是否超出命名空间容量
//   3. 缓冲区是否足以容纳 interval.LBA_count * sector_size 字节
//   4. pbase 对齐是否符合 PRP 要求（MPS 对齐）
//   5. 单次传输大小是否超出 one_time_read_limit
//
// 参数:
//   dev      — BlockDevice（对应一个 namespace），private_data 为 NVMe_device_private
//   buf      — 物理连续缓冲区的基址和大小
//   interval — LBA 区间（起始 + 数量）
//   flags    — 控制标志（当前未使用）
// ============================================================
KURD_t NVMe_Controller::read(BlockDevice* dev, pbuf_t buf,LBA_interval_t interval, uint64_t flags)
{
    (void)flags;

    // ---- 1. 提取 controller & nsid ----
    auto* priv = reinterpret_cast<NVMe_device_private_v2*>(&dev->private_data);
    NVMe_Controller* ctrl = priv->controller;
    uint32_t nsid          = priv->nsid;

    // ---- 2. 零长度检查 ----
    uint64_t count = interval.LBA_count;
    if (count == 0) {
        return empty_kurd;
    }

    uint32_t sector_size = dev->sector_size;
    uint64_t bytes       = count * sector_size;

    // ---- 3. LBA 范围检查 ----
    if (interval.start + count > dev->sector_count) {
        return read_param_error(0x3);
    }

    // ---- 4. 缓冲区容量检查 ----
    if (bytes > buf.size) {
        return read_param_error(0x4);
    }

    // ---- 5. MPS 与对齐检查 ----
    uint32_t cc       = ctrl->head_regs->controller_configuration;
    uint32_t mps_shift = 12 + ((cc >> 7) & 0xF);
    uint64_t mps      = 1ull << mps_shift;

    if (buf.pbase & (mps - 1)) {
        // pbase 未对齐到 MPS — PRP List 要求页对齐
        return read_param_error(0x5);
    }

    // ---- 6. 单次传输大小限制 ----
    if (dev->one_time_read_limit > 0 && bytes > dev->one_time_read_limit) {
        return read_param_error(0x6);
    }

    // ---- 7. 计算 MPS 页数并构建 PRP ----
    uint32_t mps_page_count = (uint32_t)((bytes + mps - 1) >> mps_shift);
    // mps_page_count 不得超出 buf.size 按 MPS 折算的页数上限
    {
        uint64_t buf_mps_pages = (buf.size + mps - 1) >> mps_shift;
        if ((uint64_t)mps_page_count > buf_mps_pages) {
            return read_param_error(0x7);
        }
    }

    // NLB 0's based, 必须 ≤ 16-bit
    if (count - 1 > 0xFFFF) {
        return read_param_error(0x8);
    }

    prp_root_t prp_root{};
    KURD_t kurd;
    kurd = build_PRP_root(buf.pbase, mps_page_count, mps_shift, &prp_root, kurd);
    if (error_kurd(kurd)) {
        return kurd;
    }

    // ---- 8. 构造 READ 命令 ----
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::io_opcode::READ;
    cmd.fiedls.nsid   = nsid;
    cmd.fiedls.DPTR1  = prp_root.prp1;
    cmd.fiedls.DPTR2  = prp_root.prp2;

    // SLBA = interval.start (64-bit), CDW10 = 低 32, CDW11 = 高 32
    cmd.dwords[10] = (uint32_t)(interval.start & 0xFFFFFFFF);
    cmd.dwords[11] = (uint32_t)(interval.start >> 32);

    // CDW12
    io_cdw12_t cdw12{};
    cdw12.nlb = (uint16_t)(count - 1);
    cmd.dwords[12] = cdw12.raw;

    // ---- 9. 提交到 I/O 队列 ----
    uint32_t cpu_id = fast_get_processor_id();
    uint16_t qid    = cpu_id + 1;
    ctrl->cmd_submit_and_process(qid, cmd);

    // ---- 10. 清理 PRP ----
    {
        KURD_t destroy_kurd;
        destroy_PRP_root(prp_root, mps_shift, destroy_kurd);
    }

    // ---- 11. 返回 ----
    return empty_kurd;
}

// ============================================================
// write_param_error / cmp_param_error
// ============================================================
static KURD_t write_param_error(uint16_t reason)
{
    return KURD_t(
        result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::Write,
        level_code::ERROR, err_domain::CORE_MODULE);
}

static KURD_t cmp_param_error(uint16_t reason)
{
    return KURD_t(
        result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::cmp,
        level_code::ERROR, err_domain::CORE_MODULE);
}

// ============================================================
// NVMe_Controller::write (static)
//
// BlockDevice 回调—物理连续缓冲区写入。
// ============================================================
KURD_t NVMe_Controller::write(BlockDevice* dev, pbuf_t buf,LBA_interval_t interval, uint64_t flags)
{
    (void)flags;

    // ---- 1. 提取 controller & nsid ----
    auto* priv = reinterpret_cast<NVMe_device_private_v2*>(&dev->private_data);
    NVMe_Controller* ctrl = priv->controller;
    uint32_t nsid          = priv->nsid;

    // ---- 2. 零长度检查 ----
    uint64_t count = interval.LBA_count;
    if (count == 0) {
        return empty_kurd;
    }

    uint32_t sector_size = dev->sector_size;
    uint64_t bytes       = count * sector_size;

    // ---- 3. LBA 范围 ----
    if (interval.start + count > dev->sector_count)
        return write_param_error(0x3);

    // ---- 4. 缓冲区容量 ----
    if (bytes > buf.size)
        return write_param_error(0x4);

    // ---- 5. MPS & 对齐 ----
    uint32_t cc       = ctrl->head_regs->controller_configuration;
    uint32_t mps_shift = 12 + ((cc >> 7) & 0xF);
    uint64_t mps      = 1ull << mps_shift;

    if (buf.pbase & (mps - 1))
        return write_param_error(0x5);

    // ---- 6. 单次传输大小限制 ----
    if (dev->one_time_write_limit > 0 && bytes > dev->one_time_write_limit)
        return write_param_error(0x6);

    // ---- 7. MPS 页数 & PRP ----
    uint32_t mps_page_count = (uint32_t)((bytes + mps - 1) >> mps_shift);
    if (count - 1 > 0xFFFF)
        return write_param_error(0x8);

    prp_root_t prp_root{};
    KURD_t kurd;
    kurd = build_PRP_root(buf.pbase, mps_page_count, mps_shift, &prp_root, kurd);
    if (error_kurd(kurd)) return kurd;

    // ---- 8. 构造 WRITE 命令 ----
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::io_opcode::WRITE;
    cmd.fiedls.nsid   = nsid;
    cmd.fiedls.DPTR1  = prp_root.prp1;
    cmd.fiedls.DPTR2  = prp_root.prp2;
    cmd.dwords[10]    = (uint32_t)(interval.start & 0xFFFFFFFF);
    cmd.dwords[11]    = (uint32_t)(interval.start >> 32);

    io_cdw12_t cdw12{};
    cdw12.nlb = (uint16_t)(count - 1);
    cmd.dwords[12] = cdw12.raw;

    // ---- 9. 提交 ----
    uint32_t cpu_id = fast_get_processor_id();
    uint16_t qid    = cpu_id + 1;

    ctrl->cmd_submit_and_process(qid, cmd);

    // ---- 10. 清理 ----
    { KURD_t dk; destroy_PRP_root(prp_root, mps_shift, dk); }

    return empty_kurd;
}

// ============================================================
// NVMe_Controller::compare (static)
//
// BlockDevice 回调—物理连续比较缓冲区。
// Compare 约束：PRINFO.PRACT=0
// ============================================================
KURD_t NVMe_Controller::compare(BlockDevice* dev, pbuf_t buf,LBA_interval_t interval, uint64_t flags)
{
    (void)flags;

    auto* priv = reinterpret_cast<NVMe_device_private_v2*>(&dev->private_data);
    NVMe_Controller* ctrl = priv->controller;
    uint32_t nsid          = priv->nsid;

    uint64_t count = interval.LBA_count;
    if (count == 0) {
        return empty_kurd;
    }

    uint32_t sector_size = dev->sector_size;
    uint64_t bytes       = count * sector_size;

    if (interval.start + count > dev->sector_count)
        return cmp_param_error(0x3);

    if (bytes > buf.size)
        return cmp_param_error(0x4);

    uint32_t cc       = ctrl->head_regs->controller_configuration;
    uint32_t mps_shift = 12 + ((cc >> 7) & 0xF);
    uint64_t mps      = 1ull << mps_shift;

    if (buf.pbase & (mps - 1))
        return cmp_param_error(0x5);

    uint32_t mps_page_count = (uint32_t)((bytes + mps - 1) >> mps_shift);
    if (count - 1 > 0xFFFF)
        return cmp_param_error(0x8);

    prp_root_t prp_root{};
    KURD_t kurd;
    kurd = build_PRP_root(buf.pbase, mps_page_count, mps_shift, &prp_root, kurd);
    if (error_kurd(kurd)) return kurd;

    // ---- 构造 COMPARE 命令 ----
    // Compare 使用与 Read 相同的 CDW12 布局；PRINFO.PRACT 必须为 0
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::io_opcode::COMPARE;
    cmd.fiedls.nsid   = nsid;
    cmd.fiedls.DPTR1  = prp_root.prp1;
    cmd.fiedls.DPTR2  = prp_root.prp2;
    cmd.dwords[10]    = (uint32_t)(interval.start & 0xFFFFFFFF);
    cmd.dwords[11]    = (uint32_t)(interval.start >> 32);

    io_cdw12_t cdw12{};
    cdw12.nlb = (uint16_t)(count - 1);
    cmd.dwords[12] = cdw12.raw;

    uint32_t cpu_id = fast_get_processor_id();
    uint16_t qid    = cpu_id + 1;

    ctrl->cmd_submit_and_process(qid, cmd);

    { KURD_t dk; destroy_PRP_root(prp_root, mps_shift, dk); }

    return empty_kurd;
}
