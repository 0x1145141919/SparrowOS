/* ═══════════════════════════════════════════════════════════════════════
 * BQ (Block Queue) 系统实现
 *
 * 全局 BQ 池 + 句柄分配/释放 + get_lock/wake 原语 + block_if_equal 阻塞入口
 *
 * 锁纪律：bq_lock > task_lock > sched_lock
 *
 * 当前阶段：空 KURD 占位。所有 ckurd 返回值为临时构造，未编排模块错误树。
 * ═══════════════════════════════════════════════════════════════════════ */

#include "Scheduler/per_processor_scheduler.h"
#include "ktime.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
class Bqs_container{
    
}