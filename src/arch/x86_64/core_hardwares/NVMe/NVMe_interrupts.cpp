#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>

// ============================================================
// 通用 SQ 提交 + 阻塞等待完成（任意队列）
// 返回完整 CQE
// ============================================================
NVMe::command::complete_command_common
NVMe_Controller::cmd_submit_and_process(
    uint16_t qid,
    NVMe::command::submit_command_common cmd,
    KURD_t& kurd)
{
    sq_complex& sq = sqs[qid];
    cq_complex& cq = cqs[sq.belonged_cqid];
    auto* sq_ring = (NVMe::command::submit_command_common*)sq.sq_ring.vbase();
    spinlock_cpp_t& lock = cq.wait_queue.lock;

    uint16_t cid;
    {
        interrupt_guard g;
        spinlock_interrupt_about_guard l(lock);

        if (sq.sq_bitmap->bit_get(sq.tail_idx)) {
            // TODO: queue full handling
        }

        cid = sq.tail_idx;
        sq.sq_bitmap->bit_set(cid, true);
        sq_ring[cid] = cmd;
        sq_ring[cid].fiedls.cid = cid;
        sq.block_tokens[cid].block_token = NVMe::entry_block_token;
        sq.tail_idx = (sq.tail_idx + 1) % sq.num_of_entries;
    }

    sq_dorbell_write(qid, sq.tail_idx);

    block_if_equal(&cq.wait_queue,
                   &sq.block_tokens[cid].block_token,
                   NVMe::entry_block_token);

    {
        interrupt_guard g;
        spinlock_interrupt_about_guard l(lock);
        sq.sq_bitmap->bit_set(cid, false);
        // Return the full CQE
        return sq.block_tokens[cid].sq_entry;
    }
}

// ============================================================
// 通用 CQ 中断处理
// ============================================================
void NVMe_Controller::cq_interrupt_handler(uint16_t qid)
{
    cq_complex& cq = cqs[qid];
    spinlock_interrupt_about_guard l(cq.wait_queue.lock);

    if (cq.is_first_time) {
        cq.unprocessed_entry_expect = true;
        cq.is_first_time = false;
    }

    auto* cq_ring = (NVMe::command::complete_command_common*)cq.cq_ring.vbase();
    uint16_t processed = 0;
    uint16_t cursor = cq.head_idx;

    while (processed < cq.num_of_entries) {
        auto& entry = cq_ring[cursor];
        if (entry.fields.phase != cq.unprocessed_entry_expect) break;

        uint16_t sq_id  = entry.fields.sq_id;
        uint16_t cmd_id = entry.fields.cmd_id;

        // Detect AER completions (CID >= AER_base_cid)
        if (cmd_id >= AER_base_cid) {
            // AER completion - store the CQE in block_tokens and signal
            sqs[sq_id].block_tokens[cmd_id].sq_entry    = entry;
            sqs[sq_id].block_tokens[cmd_id].block_token = entry.fields.status;
        } else {
            // Normal completion: store full CQE + status
            sqs[sq_id].block_tokens[cmd_id].sq_entry    = entry;
            sqs[sq_id].block_tokens[cmd_id].block_token = entry.fields.status;
        }

        cursor = (cursor + 1) % cq.num_of_entries;
        processed++;
        if (cursor == 0) cq.unprocessed_entry_expect ^= 1;
    }

    if (processed > 0) {
        cq.head_idx = cursor;
        cq_dorbell_write(qid, cursor);
        cq.wait_queue.wakeup_all();
    }
}

// ============================================================
// Admin 包装
// ============================================================
NVMe::command::complete_command_common
NVMe_Controller::ADMIN_cmd_submit_and_process(
    NVMe::command::submit_command_common cmd, KURD_t& kurd)
{
    return cmd_submit_and_process(0, cmd, kurd);
}

void NVMe_Controller::ADMIN_CQ_interrupt_handler()
{
    cq_interrupt_handler(0);
}

void NVMe_Controller::ADMIN_CQ_handler(void* ctx, uint8_t vec,
                                        uint32_t proc_id)
{
    uint16_t i = 0;
    for (; i < controllers_count; i++) {
        if (!node_array[i].controller) continue;
        if (node_array[i].controller->ADmin_queue_belonged_processor == proc_id &&
            node_array[i].controller->ADmin_queue_vec == vec) {
            node_array[i].controller->ADMIN_CQ_interrupt_handler();
            break;
        }
    }
    if (i == controllers_count) {
        bsp_kout << "[ERROR] ADMIN_CQ_handler: No matching NVMe controller for vec=";
        bsp_kout.shift_hex();
        bsp_kout << (uint32_t)vec;
        bsp_kout.shift_dec();
        bsp_kout << ", proc_id=" << proc_id << kendl;
    }
}

void NVMe_Controller::IO_CQ_handler(void* ctx, uint8_t vec,
                                     uint32_t proc_id)
{
    uint16_t i = 0;
    for (; i < controllers_count; i++) {
        if (!node_array[i].controller) continue;
        if (node_array[i].controller->IO_CQ_vecs[proc_id] == vec) {
            node_array[i].controller->IO_CQ_interrupt_handler(proc_id);
            break;
        }
    }
    if (i == controllers_count) {
        bsp_kout << "[ERROR] IO_CQ_handler: No matching NVMe controller for vec=";
        bsp_kout.shift_hex();
        bsp_kout << (uint32_t)vec;
        bsp_kout.shift_dec();
        bsp_kout << ", proc_id=" << proc_id << kendl;
    }
}

void NVMe_Controller::IO_CQ_interrupt_handler(uint32_t proc_id)
{
    cq_interrupt_handler(proc_id + 1);
}

// ============================================================
// AER 提交（非阻塞）
// ============================================================
void NVMe_Controller::aer_submit(uint16_t aer_index, KURD_t& kurd)
{
    uint16_t cid = AER_base_cid + aer_index;

    // Allocate the AER CID slot in the bitmap
    sqs[0].sq_bitmap->bit_set(cid, true);
    sqs[0].block_tokens[cid].block_token = NVMe::entry_block_token;

    // Build AER command
    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::ASYNCHRONOUS_EVENT_REQUEST;
    cmd.fiedls.cid    = cid;

    auto* sq_ring = (NVMe::command::submit_command_common*)sqs[0].sq_ring.vbase();
    sq_ring[cid] = cmd;
    sq_ring[cid].fiedls.cid = cid;

    // Ring doorbell
    sq_dorbell_write(0, sqs[0].tail_idx);
}

// ============================================================
// AER event handlers（stub）
// ============================================================
void NVMe_Controller::aer_handle_error(uint32_t info)
{
    bsp_kout << "[NVMe] AER: Error Status event, info=0x";
    bsp_kout.shift_hex();
    bsp_kout << info;
    bsp_kout.shift_dec();
    bsp_kout << kendl;
}

void NVMe_Controller::aer_handle_smart_health(uint32_t info)
{
    bsp_kout << "[NVMe] AER: SMART/Health event, info=0x";
    bsp_kout.shift_hex();
    bsp_kout << info;
    bsp_kout.shift_dec();
    bsp_kout << kendl;
}

void NVMe_Controller::aer_handle_notice(uint32_t info)
{
    bsp_kout << "[NVMe] AER: Notice event, info=0x";
    bsp_kout.shift_hex();
    bsp_kout << info;
    bsp_kout.shift_dec();
    bsp_kout << kendl;
}

void NVMe_Controller::aer_handle_io_cmd_specific(uint32_t info)
{
    bsp_kout << "[NVMe] AER: I/O Command Specific event, info=0x";
    bsp_kout.shift_hex();
    bsp_kout << info;
    bsp_kout.shift_dec();
    bsp_kout << kendl;
}

void NVMe_Controller::aer_handle_one_shot(uint32_t info)
{
    bsp_kout << "[NVMe] AER: One Shot event, info=0x";
    bsp_kout.shift_hex();
    bsp_kout << info;
    bsp_kout.shift_dec();
    bsp_kout << kendl;
}

// ============================================================
// Poll thread start
// ============================================================
void NVMe_Controller::start_poll_thread(uint16_t qid, KURD_t& kurd)
{
    bsp_kout << "[NVMe] Starting poll thread for queue " << qid << kendl;
    // Poll thread implementation would go here
    // For now, this is a stub that logs the intent
}
