#include "arch/x86_64/core_hardwares/NVMe.h"
#include "util/kout.h"
// ============================================================
// 通用 SQ 提交 + 阻塞等待完成（任意队列）
// ============================================================
uint64_t NVMe_Controller::cmd_submit_and_process(
    uint16_t qid,
    NVMe::command::submit_command_common cmd,
    KURD_t& kurd)
{
    sq_complex& sq = sqs[qid];
    cq_complex& cq = cqs[sq.belonged_cqid];
    auto* sq_ring = (NVMe::command::submit_command_common*)sq.sq_ring.vbase;
    spinlock_cpp_t& lock = cq.wait_queue.lock;

    uint16_t cid;
    {
        interrupt_guard g;
        spinlock_interrupt_about_guard l(lock);

        if (sq.sq_bitmap->bit_get(sq.tail_idx)) {
            // TODO: 队列满
        }

        cid = sq.tail_idx;
        sq.sq_bitmap->bit_set(cid, true);
        sq_ring[cid] = cmd;
        sq_ring[cid].fiedls.cid = cid;
        sq.block_tokens[cid] = NVMe::entry_block_token;
        sq.tail_idx = (sq.tail_idx + 1) % sq.num_of_entries;
    }

    sq_dorbell_write(qid, sq.tail_idx);

    block_if_equal(&cq.wait_queue, sq.block_tokens + cid, NVMe::entry_block_token);

    uint64_t ret;
    {
        interrupt_guard g;
        spinlock_interrupt_about_guard l(lock);
        sq.sq_bitmap->bit_set(cid, false);
        ret = sq.block_tokens[cid];
    }
    return ret;
}

// ============================================================
// 通用 CQ 中断处理（任意队列）
// ============================================================
void NVMe_Controller::cq_interrupt_handler(uint16_t qid)
{
    cq_complex& cq = cqs[qid];
    spinlock_interrupt_about_guard l(cq.wait_queue.lock);

    if (cq.is_first_time) {
        cq.unprocessed_entry_expect = true;
        cq.is_first_time = false;
    }

    auto* cq_ring = (NVMe::command::complete_command_common*)cq.cq_ring.vbase;
    uint16_t processed = 0;
    uint16_t cursor = cq.head_idx;

    while (processed < cq.num_of_entries) {
        auto& entry = cq_ring[cursor];
        if (entry.fields.phase != cq.unprocessed_entry_expect) break;

        sqs[entry.fields.sq_id].block_tokens[entry.fields.cmd_id] = entry.fields.status;

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
uint64_t NVMe_Controller::ADMIN_cmd_submit_and_process(
    NVMe::command::submit_command_common cmd, KURD_t& kurd)
{
    return cmd_submit_and_process(0, cmd, kurd);
}
void NVMe_Controller::ADMIN_CQ_interrupt_handler() { cq_interrupt_handler(0); }
void NVMe_Controller::ADMIN_CQ_handler(void *ctx, uint8_t vec, uint32_t proc_id)
{   
    uint16_t i=0;
    for(;i<controllers_count;i++){
        if(!node_array[i].controller)continue;
        if(node_array[i].controller->ADmin_queue_belonged_processor==proc_id&&
        node_array[i].controller->ADmin_queue_vec==vec){
            node_array[i].controller->ADMIN_CQ_interrupt_handler();
            break;
        }
    }
    if(i==controllers_count){
        // 打印报错：未找到匹配的NVMe控制器
        bsp_kout << "[ERROR] ADMIN_CQ_handler: No matching NVMe controller found for vec=";
        bsp_kout.shift_hex();
        bsp_kout << (uint32_t)vec;
        bsp_kout.shift_dec();
        bsp_kout << ", proc_id=" << proc_id << kendl;
    }

}

void NVMe_Controller::IO_CQ_handler(void *ctx, uint8_t vec, uint32_t proc_id)
{
    uint16_t i=0;
    for(;i<controllers_count;i++){
        if(!node_array[i].controller)continue;
        if(node_array[i].controller->IO_CQ_vecs[proc_id]==vec){
            node_array[i].controller->IO_CQ_interrupt_handler(proc_id);
            break;
        }
    }
    if(i==controllers_count){
        // 打印报错：未找到匹配的NVMe控制器
        bsp_kout << "[ERROR] IO_CQ_handler: No matching NVMe controller found for vec=";
        bsp_kout.shift_hex();
        bsp_kout << (uint32_t)vec;
        bsp_kout.shift_dec();
        bsp_kout << ", proc_id=" << proc_id << kendl;
    }
}
void NVMe_Controller::IO_CQ_interrupt_handler(uint32_t proc_id)
{
    cq_interrupt_handler(proc_id+1);
}