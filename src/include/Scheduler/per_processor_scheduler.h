#pragma once
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "arch/x86_64/abi/base.h"
#include "abi/os_error_definitions.h"
#include "util/Ktemplats.h"
#include "util/lock.h"
#include "ktime.h"
#include "memory/AddresSpace.h"
namespace Scheduler{
    constexpr uint8_t self_scheduler=1;
    constexpr uint8_t scheduler_task_pool=2;
    namespace task_pool_events{
        constexpr uint8_t init=0;
        constexpr uint8_t slot_alloc=1;
        namespace alloc_results{
            namespace fail_reasons{
                constexpr uint16_t not_found=1;
            }
        }
        constexpr uint8_t slot_free=2;
        namespace free_results{ 
            namespace fail_reasons{
                constexpr uint16_t index_out_of_range=1;
                constexpr uint16_t not_allocated=2;
                constexpr uint16_t bad_tid=3;
                constexpr uint16_t sub_table_not_exist=4;
            }
        }
        namespace try_free_subtable{
            namespace fail_reasons{
                constexpr uint16_t null_pool=1;
                constexpr uint16_t not_all_empty=2;
            }
        }
    }
    namespace self_scheduler_events{
        constexpr uint8_t kthread_yield_enter=1;
        namespace kthread_yield_enter_results{
            namespace fatal_reasons{

            }
        }
        constexpr uint8_t insert_ready_task=3;
        namespace insert_ready_task_results{ 
            namespace fail_reasons{
                constexpr uint16_t null_task_ptr=1;
                constexpr uint16_t bad_task_type=2;
                constexpr uint16_t insert_fail=3;
            }
        }
        constexpr uint8_t wake_up_kthread=4;
        namespace wake_up_kthread_results{
            namespace success_reasons{
                constexpr uint16_t other_entity_wakeup=1;
                constexpr uint16_t already_wakeup_or_running=2;
            }
            namespace fail_reasons{
                constexpr uint16_t bad_task_state=1;
                constexpr uint16_t kthread_cant_wake_for_bad_block_reason=2;
            }
        }
        constexpr uint8_t kthread_block=5;
        namespace kthread_block_results{
            namespace fatal_reasons{
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t sleep_task_insert=7;
        namespace sleep_task_insert_results{
            namespace fail_reasons{
                constexpr uint16_t insert_fail=3;
            }
        }
        constexpr uint8_t kthread_sleep=6;
        namespace kthread_sleep_results{ 
            namespace fatal_reasons{
                constexpr uint16_t bad_task_state=1;
                 
                 
                 
                constexpr uint16_t illeage_state=5;
            }
        }
        // kthread_wait=8 removed — 2026-07-02 (waiters 链表一并清除)
        constexpr uint8_t kthread_exit=9;
        namespace kthread_exit_results{
            namespace fatal_reasons{
                constexpr uint16_t bad_task_state=1;
                 
            }
        }
        constexpr uint8_t kthread_block_queue=10;
        namespace kthread_block_queue_results{
            namespace fatal_reasons{
                 
                 
                 
                 
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t kthread_block_queue_if_equal=11;
        namespace kthread_block_queue_if_equal_results{
            namespace fatal_reasons{
                 
                 
                 
                 
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t kthread_common_save=12;
        // kthread_wait/waiters 已移除 — 2026-07-02。见 task_v3设计.md。
        namespace kthread_common_save_results{
            namespace fatal_reasons{
                 
                 
                constexpr uint16_t bad_task_state=3;
                 
            }
            namespace fail_reasons{
                constexpr uint16_t nullptr_param=1;
            }
        }
    }
};
enum task_state_t:uint8_t{
    init=0,
    ready,
    running,
    blocked,
    zombie,
    dead
};
// wait_other_kthread 已移除 — 2026-07-02，kthread_wait 及 waiters 链表已清除。
enum task_blocked_reason_t:uint8_t{
    invalid,
    sleeping,
    mutex,
    no_job
};
namespace kthread_call_num{
    constexpr uint64_t exit=0;
    constexpr uint64_t sleep=1;
    constexpr uint64_t yield=2;
    // 3 was kthread_wait — removed 2026-07-02
    constexpr uint64_t block=4;
    constexpr uint64_t block_to_queue=5;
    constexpr uint64_t block_to_queue_if_equal=6;
};
constexpr uint64_t INVALID_TID=~0ull;
constexpr miusecond_time_stamp_t DEFALUT_TIMER_SPAN_MIUS=20000;
constexpr uint64_t INIT_DEFAULT_RFLAGS=0x202;
extern "C" void secure_hlt();
struct u_ctx_t{
    x64_standard_context_v2 xtd_ctx;
    AddressSpace*as;
    uint32_t xcr0_mask;
    uint64_t cr4_mask;
    uint64_t drs[8];
    vaddr_t gs_base;
    vaddr_t fs_base;
    void*xsave_area;//从extend_ctx开始的4k空间，头部挖去后若剩下的足够，则align_up(this+sizoef(extend_ctx),64)后作为FPU通过XSAVEC保存，不够则走__wrapped_pgs_alloc分配页框存放
    uint64_t xsave_size;
};
class task{
    public:
    enum event_type_t{
        init,
        run_kthread,
        run_uthread,
        run_vCPU,
        offline,
        sleep,
        wait_io,
        wait_other,
        wait_mutex,
        event_type_COUNT
    };
    private:
    event_type_t current_event;
    task_state_t task_state;
    uint32_t belonged_processor_id;
    uint64_t tid;
    miusecond_time_stamp_t current_event_start_stamp;
    miusecond_time_stamp_t accumulates_bank[event_type_COUNT];
    
    public:
    task();
    bool usage_of_search_set_tid( uint64_t new_tid);//如其名字，只能在那个task_pool搜索的特殊场景用以用
    uint64_t get_tid();
    static  task* basic_constructor();
    static void placement_constructor(task*task_ptr);
    void atomic_load();   // 根据 ctx_choose 无脑加载对应上下文
    bool set_ready();//成功返回true,但是必须从init/blocked切换到才合法/成功，非法不会改状态字段
    bool set_blocked();//成功返回true,但是必须从running切换到才合法/成功，非法不会改状态字段
    bool set_dead();//成功返回true,只能由zombie切换到才合法/成功，非法不会改状态字段
    bool set_zombie();//合法前驱仅限running,blocked,ready
    bool set_running();
    reentrant_spinlock_cpp_t task_lock;
    void task_event_shift(event_type_t new_event);
    miusecond_time_stamp_t min_wakeup_stamp;
    uint32_t get_belonged_processor_id();
    void set_belonged_processor_id(uint32_t pid);
    x64_standard_context_v2 priv_ctx;
    vaddr_t priv_stack_base;        // 栈顶（4K对齐），guard page 在 [base-4K, base)
    uint32_t priv_stack_pages;      // 可用页数（不含 guard page）
    // 栈布局（高位→低位）：
    //   priv_stack_base + 4K * priv_stack_pages - 64B    — 初始RSP
    //   [priv_stack_base, priv_stack_base + 4K * pages)  — 可用栈空间（向下增长）
    //   priv_stack_base                                    — 栈顶
    //   [priv_stack_base - 4K, priv_stack_base)            — guard page（未映射，#PF not-present）
    u_ctx_t*uctx;//指向一个页
    //还有vCPU ctx指针
    enum ctx_choose{
        priv,
        u_ctx,
        vCPU
    };
    task_state_t get_state();
    ctx_choose choose;
};


// ── wq 句柄系统 ────────────────────────────────────────
// 全局 wait_queue 表，句柄（wq_id_t）代替指针：防伪、防 UAF、可跨进程传递
// 用户态可通过 syscall 使用相同句柄
struct  blocked_tasks_clamps_t{
    uint8_t batch_count;
    bool is_queue_empty;
    bool is_timeout_mov_early;
    task* arr[64];
};
typedef uint64_t bq_id_t;
constexpr bq_id_t  BQ_ID_INVALID = ~0u;
// 内部队列（对外不暴露）
class block_queue{
    enum state_t { ready, running } state = ready;
    task::event_type_t queue_event;
    Ktemplats::list_doubly<task*> inner_queue;
    public:
    spinlock_cpp_t qlock;
    block_queue(): state(ready), qlock{}, queue_event{}, inner_queue{} 
  {};
    KURD_t enable_queue(task::event_type_t type);
    KURD_t disable_queue();
    bool is_queue_ready();
    task*pop_head();
    void pop_timeouts(blocked_tasks_clamps_t*batch);
    void pop_all(blocked_tasks_clamps_t*batch);
};

/**
 * 
 */
// task_tid_compare — 按 tid 大小排序 task*
static int task_tid_compare(task* const& a ,task* const& b) {
    return a->get_tid()-b->get_tid();
}

class task_pool{
    static spinrwlock_cpp_t lock;
    static Ktemplats::RBTree<task*, task_tid_compare> m_tree;
public:
    static task* get_by_tid(uint64_t tid, KURD_t& kurd);
    static KURD_t insert(task* t);
    static KURD_t release(uint64_t tid);
    static int Init();
};
// ── DTS Gantt 日志条目标准格式 ─────────────────────────────────
struct dts_gantt_entry {
    uint64_t tsc;                // rdtsc 时间戳
    uint32_t tid;                // 被调度到的任务
    uint16_t dts_timeslice_us;   // 分配的时间片(μs)
    uint8_t  reason : 4;         // 调度原因
    uint8_t  preempt_cnt : 2;    // 当时的 dts_preempt_cnt
    uint8_t  voluntary_cnt : 2;  // 当时的 dts_voluntary_cnt
    uint8_t  io_urgency : 2;     // 引起调度的中断紧迫度
} __attribute__((packed));
static_assert(sizeof(dts_gantt_entry) == 16, "dts_gantt_entry size mismatch");

class alignas(64) per_processor_scheduler { 
    private:
    KURD_t default_kurd();
    KURD_t default_success();
    KURD_t default_fail();
    KURD_t default_fatal();
    task idle;
    void sleep_tasks_wake();
    void sched();//会内部修改ready_queue数据结构用ready_queues_lock保护，然后对应的task也会用锁保护其状态改变
    public:
    static constexpr uint32_t GANTT_CAPACITY = 4096;
    dts_gantt_entry* dts_gantt = nullptr;    // 按需分配，NULL=关闭
    uint32_t         dts_gantt_head = 0;
    Ktemplats::list_doubly<task*> ready_queue;//FIFO,除了从睡眠队列拿出来的是push_head,其他都是push_tail，但是运行一个任务都是在队列上pop_head
    class sleep_queue_t:Ktemplats::list_doubly<task*>
    {
        //继承自list_doubly的类，插入的时候要按照唤醒时间戳升序
        private:
        public:
        using list_doubly<task*>::empty;
        using list_doubly<task*>::front;
        using list_doubly<task*>::back;
        using list_doubly<task*>::pop_front_value;
        using list_doubly<task*>::size;
        using list_doubly<task*>::pop_front;
        sleep_queue_t()=default;
        KURD_t insert(task*task_ptr);//由于期望出队列的时候用pop_head_value的时候是时间戳最低的，因此这个insert要注意排序
    };
    sleep_queue_t sleep_queue;
    reentrant_spinlock_cpp_t sched_lock;//调度器数据结构锁，保护running tid,sleep_queue_t
    bool is_idle;
    void next_task_with_routine();//会交出执行流的一个函数，会先bq_id_t翻牌子，唤醒一批超时的，再sleep_tasks_wake唤醒所有睡眠超时的，最后再sched()调度
    KURD_t insert_ready_task(task*task_ptr, bool front=false);
    
    
    // ── DTS Gantt 接口 ──
    KURD_t dts_gantt_enable();   // 按需分配 Gantt 缓冲区
    void   dts_gantt_disable();  // 释放缓冲区，置 NULL
    void   dts_gantt_write(task* to_run, uint8_t reason, uint8_t io_urgency);
    friend task;
    friend class task_pool;
    static void placed_init();//以后这个调度器塞入每个CPU的gs_complex_t里面内嵌
};
per_processor_scheduler* get_self_scheduler();
per_processor_scheduler& get_other_scheduler(uint32_t pid);
per_processor_scheduler* get_other_scheduler(uint32_t pid);
constexpr uint32_t INVALID_NODE_INDEX=~0;

extern "C"{
    ckurd kthread_init(task*t,void*entry,void*arg1,void*arg2,uint8_t priv_pages);
    KURD_t task_start(task*t,uint32_t pid);//指定处理器上把对应的没有运行过（run_kthread积累为0的任务）从init转入ready后放入ready_queue
    [[noreturn]] void kthread_yield_true_enter(x64_standard_context_v2* context);
    void kthread_yield();
    uint64_t* get_scheduler_private_stack_top();
    void kthread_exit(uint64_t will);
    [[noreturn]] void kthread_exit_cppenter(x64_standard_context_v2*context);
    void kthread_self_blocked(task_blocked_reason_t reason);
    void kthread_sleep(miusecond_time_stamp_t offset);
    [[noreturn]] void kthread_sleep_cppenter(x64_standard_context_v2* context);
    [[noreturn]] void kthread_self_blocked_cppenter(x64_standard_context_v2* context);
    ckurd wakeup_thread(uint64_t tid, bool front_insert=false);
    [[noreturn]] void block_queue_cppenter(x64_standard_context_v2* context);
    void block_if_equal(bq_id_t qid, uint64_t* checker, uint64_t block_token);
    void block_if_equal_cppenter(x64_standard_context_v2* context);
    ckurd release_kthread(uint64_t tid);
    bq_id_t  bq_alloc(block_queue*q);                         // 分配一个新 block_queue，返回句柄,处于ready态
    ckurd bq_free(bq_id_t qid);               // 释放，返回 ckurd（KURD raw）
    //上面三个的返回值是唤醒个数
    void bq_flush_pending(blocked_tasks_clamps_t* clamp); // 处理 pending_wake 中所有弹出的 task（调用方已释放 bq_lock）
    void common_idle();
    char allkthread_true_enter;
    void resched(x64_standard_context_v2 *frame);
}
/**
 * 内核线程接口里面锁顺序纪律：
 * 1.task锁永远比scheduler的锁先锁
 * 2.block_queue / block_if_equal_cppenter 的 wq 锁临界区覆盖放弃执行流的线程锁
 * 3.task锁临界区内可以调用task_pool相关接口，只在其内部有锁
 * 4.wait/exit 及 waiters 已移除 — 2026-07-02
 */