/* ═══════════════════════════════════════════════════════════════════════
 * NVMe 中断与提交路径 — 锁纪律
 *
 * 三类锁，按 `cq_wq_lock > sq_lock` 方向拿取，允许嵌套，禁止逆向：
 *
 *    cq_wq_lock  (block_queue::qlock)
 *         │
 *         ├ 保护：wait_queue（block_queue）的 task* 链表出入（pop）
 *         ├ 仅中断路径持有(cq_interrupt_handler)
 *         ├ 提交/清理路径不碰此锁
 *         │
 *         ▼ 可在临界区内嵌套拿 sq_lock
 *
 *    sq_lock    (sq_complex::sq_lock)
 *         │
 *         ├ 保护：flying_slots、tail_idx、complete_commands_bank[]
 *         ├ 提交路径(asynchronized_cmd_submit)持有
 *         ├ 清理路径(release_cmd → sq_lock)持有
 *         └ 中断路径不会先拿 sq_lock→再拿 cq_wq_lock（禁止逆向）
 *
 * 路径拆解：
 *
 *   [提交] asynchronized_cmd_submit
 *     sq_lock → flying_slots.set + sq_ring[cid] + complete_commands_bank[cid].cmd_spcify = entry_block_token
 *             → sq_lock.unlock()
 *     sq_dorbell_write(qid, sq.tail_idx)
 *     返回 cid
 *
 *   [等待/释放] cmd_submit_and_process
 *     block_if_equal(cq.block_queue_id, &complete_commands_bank[cid].cmd_spcify, entry_block_token)
 *     release_cmd(qid,cid) → sq_lock → 清 flying_slots + 边界检查
 *     返回 command_result_t
 *
 *   [释放] release_cmd
 *     sq_lock → 清 flying_slots + 边界检查
 *
 *   [中断] cq_interrupt_handler
 *     spinlock_interrupt_about_guard(cq_wq_lock)
 *       读 CQ ring
 *       对每完成：写 complete_commands_bank[]（无需 sq_lock）
 *       pop_all()  → 放入 batch
 *     cq_wq_lock.unlock()
 *     bq_flush_pending(batch, false)
 *     写 doorbell
 *
 *   [清理]（超时后同上提交路径后半段）
 *     spinlock_interrupt_about_guard(sq_lock) → 读结果（不 release，flying_slot 保留用于排查）
 *
 * ═══════════════════════════════════════════════════════════════════════ */
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
NVMe::command_result_t NVMe_Controller::cmd_submit_and_process(uint16_t qid, NVMe::command::submit_command_common cmd)
{
    sq_complex& sq = sqs[qid];
    cq_complex& cq = cqs[sq.belonged_cqid];
    uint16_t cid= asynchronized_cmd_submit(qid,cmd);
    uint64_t res= block_if_equal(cq.block_queue_id,&sq.complete_commands_bank[cid].fields.cmd_spcify,NVMe::entry_block_token);
    NVMe::command_result_t r;
    if(res&2){
        r.fields.result_type_t = NVMe::command_result_types::timeout;
    }else{
        ksystemramcpy(&sq.complete_commands_bank[cid],&r,16);
        release_cmd(qid,cid);
        r.fields.result_type_t = NVMe::command_result_types::command_executed;
    }
    return r;
}
uint16_t NVMe_Controller::asynchronized_cmd_submit(uint16_t qid, NVMe::command::submit_command_common cmd)
{
    sq_complex& sq = sqs[qid];
    cq_complex& cq = cqs[sq.belonged_cqid];
    auto* sq_ring = (NVMe::command::submit_command_common*)sq.sq_ring.vbase();

    uint16_t cid;
    {
        interrupt_guard g;
        spinlock_interrupt_about_guard l(sq.sq_lock);

        if (sq.flying_slots[sq.tail_idx]) {
            // TODO: queue full handling
        }

        cid = sq.tail_idx;
        sq.flying_slots[cid] = true;
        sq_ring[cid] = cmd;
        sq_ring[cid].fiedls.cid = cid;
        sq.complete_commands_bank[cid].fields.cmd_spcify = NVMe::entry_block_token;
        sq.tail_idx = (sq.tail_idx + 1) % sq.num_of_entries;
    }

    sq_dorbell_write(qid, sq.tail_idx);
    return cid;
}
bool NVMe_Controller::release_cmd(uint16_t qid, uint64_t cid)
{
    
        if(qid>=this->sq_count)return false;
        sq_complex& sq = sqs[qid];
        if(cid>=sq.num_of_entries)return false;
        interrupt_guard g;
        spinlock_interrupt_about_guard l(sq.sq_lock);
        sq.flying_slots[cid] = false;
        // Return the full CQE
        return true;
    
}

uint64_t NVMe_Controller::interrupt_handle(interrupt_token_t *token)
{
    // token_private 的 [0:63] 是 NVMe_Controller* 指针，[64:79] 是 cq_id
    __uint128_t token_private = token->token_private;
    NVMe_Controller* dev = (NVMe_Controller*)token_private;
    uint16_t cqid = token->token_private >> 64;
    dev->cq_interrupt_handler(cqid);
    return 1; // TOKEN_FLAG_MASK_TOKEN_SCHEDULE
}

// ============================================================
// 通用 CQ 中断处理
// ============================================================
void NVMe_Controller::cq_interrupt_handler(uint16_t qid)
{
    cq_complex& cq = cqs[qid];
    blocked_tasks_clamps_t clamp;

    {
        spinlock_interrupt_about_guard l(cq.wait_queue.qlock);

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

            // 写入 CQE 结果 — 提交端的 block_if_equal 醒来后读取
            {
            spinlock_interrupt_about_guard g( sqs[sq_id].sq_lock);
            sqs[sq_id].complete_commands_bank[cmd_id] = entry;
            }

            cursor = (cursor + 1) % cq.num_of_entries;
            processed++;
            if (cursor == 0) cq.unprocessed_entry_expect ^= 1;
        }

        if (processed > 0) {
            cq.head_idx = cursor;
            cq_dorbell_write(qid, cursor);
            
            while(true)
            {
                cq.wait_queue.pop_all(&clamp);
                if(clamp.is_queue_empty)break;
                bq_flush_pending(&clamp,false);
            }
        }
    }

    if (clamp.batch_count > 0) {
        bq_flush_pending(&clamp, false);
    }
}

// ============================================================
// AER 提交（非阻塞）
//
// 注意：AER CID 使用高位范围（AER_base_cid + aer_index），
// 不经过常规 flying_slots 位图（位图仅覆盖 256 个 slot）。
// AER 在 Admin SQ 上提交，提交端不阻塞；其中断完成走 admin CQ 路径。
// ============================================================
void NVMe_Controller::aer_submit(uint16_t aer_index, KURD_t& kurd)
{
    uint16_t cid = AER_base_cid + aer_index;

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
