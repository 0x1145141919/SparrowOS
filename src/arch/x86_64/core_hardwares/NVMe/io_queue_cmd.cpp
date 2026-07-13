#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include "Scheduler/kthread_abi.h"

extern uint32_t logical_processor_count;

// ============================================================
// 常量 & 辅助
// ============================================================
static constexpr uint32_t CQ_ENTRY_SIZE = 16;
static constexpr uint32_t SQ_ENTRY_SIZE = 64;

// ============================================================
// queue_mgmt_cmd：底层 Admin 命令发送（Create/Delete）
// ============================================================
NVMe::command_result_t
NVMe_Controller::queue_mgmt_cmd(uint8_t opcode, uint16_t qid,
                                 uint16_t qsize, uint32_t cdw11,
                                 phyaddr_t prp1)
{
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = opcode;
    cmd.fiedls.nsid   = 0;
    cmd.dwords[10]    = (uint32_t(qsize) << 16) | qid;
    cmd.dwords[11]    = cdw11;

    if (opcode == NVMe::command::admin_opcode::CREATE_IO_SUBMISSION_QUEUE ||
        opcode == NVMe::command::admin_opcode::CREATE_IO_COMPLETION_QUEUE) {
        cmd.fiedls.DPTR1 = prp1;
    }

    return cmd_submit_and_process(0, cmd);
}

// ============================================================
// create_io_cq：完整创建 I/O Completion Queue
//
// 1. 分配 ring buffer
// 2. msix_vec_alloc 分配中断向量
// 3. 初始化 cqs[] 状态
// 4. 发送 Create I/O CQ 命令
// 5. 失败 → 回滚释放所有资源
// ============================================================
NVMe::command_result_t NVMe_Controller::create_io_cq(uint16_t qid, uint16_t qsize,
                                                      bool ien)
{
    KURD_t kurd;

    // ---- 1. 分配 ring buffer ----
    uint32_t cq_bytes = align_up(qsize * CQ_ENTRY_SIZE, 4096);
    void* cq_ring_va = __wrapped_pgs_valloc(
        &kurd, cq_bytes / 4096, page_state_t::kernel_pinned, 12);
    if (error_kurd(kurd) || !cq_ring_va) return NVMe::make_not_success_kurd(kurd);
    ksetmem_8(cq_ring_va, 0, cq_bytes);

    phyaddr_t cq_ring_pa = 0;
    kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)cq_ring_va, cq_ring_pa);
    if (error_kurd(kurd)) {
        __wrapped_pgs_vfree(cq_ring_va, cq_bytes / 4096);
        return NVMe::make_not_success_kurd(kurd);
    }

    // ---- 2. 分配 MSI-X vector（先 Mask，create 成功后 Unmask）----
    KURD_t msix_kurd;
    msix_kurd = msix_vec_alloc(qid - 1, qid);
    if (error_kurd(msix_kurd)) {
        __wrapped_pgs_vfree(cq_ring_va, cq_bytes / 4096);
        return NVMe::make_not_success_kurd(msix_kurd);
    }

    // ---- 3. 预填 cqs[] 状态 ----
    cqs[qid].num_of_entries  = qsize;
    cqs[qid].is_first_time   = true;
    cqs[qid].head_idx        = 0;

    // ---- 4. 发送 Create 命令 ----
    NVMe::io_queue::create_cq_cdw11_t cdw11;
    cdw11.raw  = 0;
    cdw11.pc   = 1;
    cdw11.ien  = ien ? 1 : 0;
    cdw11.iv   = qid;

    NVMe::command_result_t r =
        queue_mgmt_cmd(NVMe::command::admin_opcode::CREATE_IO_COMPLETION_QUEUE,
                        qid, qsize, cdw11.raw, cq_ring_pa);
    if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
        // 失败回滚
        msix_vec_free(qid);
        __wrapped_pgs_vfree(cq_ring_va, cq_bytes / 4096);
        cqs[qid] = cq_complex{};  // zero
        return r;
    }

    // ---- 5. 成功：记录 ring 信息 ----
    cqs[qid].block_queue_id = bq_alloc(&cqs[qid].wait_queue);
    cqs[qid].cq_ring = {
        .vpn  = reinterpret_cast<vaddr_t>(cq_ring_va) >> 12,
        .ppn  = cq_ring_pa >> 12,
        .npages   = cq_bytes >> 12,
        .access = KSPACE_RW_UC_ACCESS };

    return NVMe::command_result_t{};
}

// ============================================================
// create_io_sq：完整创建 I/O Submission Queue
//
// 1. 分配 ring buffer
// 2. 构造 sq_bitmap
// 3. 发送 Create I/O SQ 命令
// 4. 失败 → 回滚释放所有资源
// ============================================================
NVMe::command_result_t NVMe_Controller::create_io_sq(uint16_t qid, uint16_t qsize,
                                                      uint16_t cqid, uint8_t qprio)
{
    KURD_t kurd;

    // ---- 1. 分配 ring buffer ----
    uint32_t sq_bytes = align_up(qsize * SQ_ENTRY_SIZE, 4096);
    void* sq_ring_va = __wrapped_pgs_valloc(
        &kurd, sq_bytes / 4096, page_state_t::kernel_pinned, 12);
    if (error_kurd(kurd) || !sq_ring_va) return NVMe::make_not_success_kurd(kurd);
    ksetmem_8(sq_ring_va, 0, sq_bytes);

    phyaddr_t sq_ring_pa = 0;
    kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)sq_ring_va, sq_ring_pa);
    if (error_kurd(kurd)) {
        __wrapped_pgs_vfree(sq_ring_va, sq_bytes / 4096);
        return NVMe::make_not_success_kurd(kurd);
    }

    // ---- 2. 预填 sqs[] 状态（内嵌数据，无需额外分配）----
    sqs[qid].flying_slots.enable(sqs[qid].flying_slots_raw_map, qsize);
    sqs[qid].sqid           = qid;
    sqs[qid].num_of_entries = qsize;
    sqs[qid].belonged_cqid  = cqid;
    sqs[qid].tail_idx       = 0;

    // ---- 4. 发送 Create 命令 ----
    NVMe::io_queue::create_sq_cdw11_t cdw11;
    cdw11.raw   = 0;
    cdw11.pc    = 1;
    cdw11.qprio = qprio;
    cdw11.cqid  = cqid;

    NVMe::command_result_t r =
        queue_mgmt_cmd(NVMe::command::admin_opcode::CREATE_IO_SUBMISSION_QUEUE,
                        qid, qsize, cdw11.raw, sq_ring_pa);
    if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
        // 失败回滚（内嵌数据无需释放，只回滚外部资源）
        __wrapped_pgs_vfree(sq_ring_va, sq_bytes / 4096);
        sqs[qid] = sq_complex{};  // zero
        return r;
    }

    // ---- 5. 成功：记录 ring 信息 ----
    sqs[qid].sq_ring = {
        .vpn  = reinterpret_cast<vaddr_t>(sq_ring_va) >> 12,
        .ppn  = sq_ring_pa >> 12,
        .npages   = sq_bytes >> 12,
        .access = KSPACE_RW_UC_ACCESS };

    return NVMe::command_result_t{};
}

// ============================================================
// delete_io_sq：完整释放 I/O Submission Queue
//
// 1. 逐 bit 轮询等待释放（每 bit 500ms）
// 2. 发送 Delete I/O SQ 命令
// 3. 释放 ring / bitmap
// ============================================================
NVMe::command_result_t NVMe_Controller::delete_io_sq(uint16_t qid)
{
    constexpr uint32_t DRAIN_PER_BIT_MS = 500;

    if (sqs[qid].sq_ring.vpn == 0)
        return NVMe::command_result_t{};

    // ---- 1. 逐 bit 等待排空 ----
    for (uint32_t i = 0; i < (uint32_t)sqs[qid].num_of_entries; i++) {
        if (!sqs[qid].flying_slots[i]) continue;

        uint32_t waited = 0;
        while (waited < DRAIN_PER_BIT_MS) {
            if (!sqs[qid].flying_slots[i]) break;
            kthread_sleep(1 * 1000);
            waited++;
        }

        if (sqs[qid].flying_slots[i]) {
            bsp_kout << "[NVMe] SQ " << (uint32_t)qid
                     << " slot " << i << " drain timeout" << kendl;
            return NVMe::command_result_t{ .fields = { .result_type = NVMe::command_result_types::timeout } };
        }
    }

    // ---- 2. 发送 Delete 命令 ----
    NVMe::command_result_t r =
        queue_mgmt_cmd(NVMe::command::admin_opcode::DELETE_IO_SUBMISSION_QUEUE,
                        qid, 0, 0, 0);

    // ---- 3. 释放资源（内嵌数据无需释放，只释放外部 ring）----
    if (sqs[qid].sq_ring.vpn != 0) {
        __wrapped_pgs_vfree(reinterpret_cast<void*>(sqs[qid].sq_ring.vbase()),
                             sqs[qid].sq_ring.npages);
        sqs[qid].sq_ring.vpn = 0;
    }
    sqs[qid] = sq_complex{};

    return r;
}

// ============================================================
// delete_io_cq：完整释放 I/O Completion Queue
//
// 1. 发送 Delete I/O CQ 命令
// 2. 释放 ring buffer
// 3. 释放 MSI-X 向量
// ============================================================
NVMe::command_result_t NVMe_Controller::delete_io_cq(uint16_t qid)
{
    if (cqs[qid].cq_ring.vpn == 0)
        return NVMe::command_result_t{};

    // ---- 1. 发送 Delete 命令 ----
    NVMe::command_result_t r =
        queue_mgmt_cmd(NVMe::command::admin_opcode::DELETE_IO_COMPLETION_QUEUE,
                        qid, 0, 0, 0);

    // ---- 2. 释放 ring ----
    if (cqs[qid].cq_ring.vpn != 0) {
        __wrapped_pgs_vfree(reinterpret_cast<void*>(cqs[qid].cq_ring.vbase()),
                             cqs[qid].cq_ring.npages);
        cqs[qid].cq_ring.vpn = 0;
    }

    // ---- 3. 释放 MSI-X 向量 ----
    msix_vec_free(qid);

    // ---- 4. 释放 block queue ----
    bq_free(cqs[qid].block_queue_id);

    cqs[qid] = cq_complex{};  // zero

    return r;
}

// ============================================================
// io_queue_init：批量 I/O 队列初始化
// ============================================================
NVMe::command_result_t NVMe_Controller::io_queue_init(uint16_t iosq_count,
                                                       uint16_t iocq_count)
{
    bsp_kout << "[NVMe] queue negotiation: req iosq="
             << (uint32_t)iosq_count
             << " iocq=" << (uint32_t)iocq_count << kendl;

    // ---- 1. Set Features Number of Queues ----
    NVMe::command_result_t r = set_features_num_queues(iosq_count, iocq_count);
    if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
        bsp_kout << "[NVMe] set_features_num_queues FAILED" << kendl;
        return r;
    }

    // ---- 2. 创建 I/O CQ (qid 1..iocq_count) ----
    for (uint16_t qid = 1; qid <= iocq_count; qid++) {
        r = create_io_cq(qid, IO_CQ_ENTRY_COUNT, true);
        if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
            bsp_kout << "[NVMe] create_io_cq qid="
                     << (uint32_t)qid << " failed" << kendl;
            continue;
        }
        bsp_kout << "[NVMe] CQ " << (uint32_t)qid << " created" << kendl;
    }

    // ---- 3. 创建 I/O SQ (sqid 1..iosq_count), round-robin → CQ ----
    for (uint16_t sqid = 1; sqid <= iosq_count; sqid++) {
        uint16_t cqid = ((sqid - 1) % iocq_count) + 1;

        r = create_io_sq(sqid, IO_SQ_ENTRY_COUNT, cqid,
                          NVMe::io_queue::SQ_PRIO_HIGH);
        if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
            bsp_kout << "[NVMe] create_io_sq sqid="
                     << (uint32_t)sqid << " failed" << kendl;
            continue;
        }
        bsp_kout << "[NVMe] SQ " << (uint32_t)sqid
                 << " (→CQ " << (uint32_t)cqid << ") created" << kendl;
    }

    bsp_kout << "[NVMe] io_queue_init done" << kendl;
    return NVMe::command_result_t{};
}

// ============================================================
// io_queue_free：批量 I/O 队列释放
// ============================================================
NVMe::command_result_t NVMe_Controller::io_queue_free()
{
    // ---- 1. 删除所有 I/O SQ ----
    for (uint16_t qid = 1; qid < sq_count; qid++) {
        if (sqs[qid].sq_ring.vpn == 0) continue;
        NVMe::command_result_t r = delete_io_sq(qid);
        if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
            bsp_kout << "[NVMe] delete_io_sq qid="
                     << (uint32_t)qid << " failed" << kendl;
        }
    }

    // ---- 2. 删除所有 I/O CQ ----
    for (uint16_t qid = 1; qid < cq_count; qid++) {
        if (cqs[qid].cq_ring.vpn == 0) continue;
        NVMe::command_result_t r = delete_io_cq(qid);
        if (r.fields.result_type != NVMe::command_result_types::command_executed || NVMe::status::is_error(r.fields.status)) {
            bsp_kout << "[NVMe] delete_io_cq qid="
                     << (uint32_t)qid << " failed" << kendl;
        }
    }

    return NVMe::command_result_t{};
}
