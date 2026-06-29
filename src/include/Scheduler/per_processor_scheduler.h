#pragma once
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "arch/x86_64/abi/base.h"
#include "abi/os_error_definitions.h"
#include "util/Ktemplats.h"
#include "util/huge_bitmap.h"
#include "util/lock.h"
#include "ktime.h"
#include "memory/all_pages_arr.h"
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
        constexpr uint8_t timer_cpp_enter=0;
        namespace timer_cpp_enter_results{
            namespace fatal_reasons{
                constexpr uint16_t null_scheduler=1;
                constexpr uint16_t invalid_running_task_id=2;
                constexpr uint16_t null_task_ptr=3;
                constexpr uint16_t invalid_cs=4;
                constexpr uint16_t null_kthread_context=5;
                constexpr uint16_t invalid_stack_size=6;
                constexpr uint16_t rsp_out_of_range=7;
                constexpr uint16_t null_userthread_context=8;
                constexpr uint16_t invalid_task_type=9;
            }
        }
        constexpr uint8_t kthread_yield_enter=1;
        namespace kthread_yield_enter_results{
            namespace fatal_reasons{
                constexpr uint16_t null_scheduler=1;
                constexpr uint16_t invalid_running_task_id=2;
                constexpr uint16_t null_task_ptr=3;
                constexpr uint16_t null_kthread_context=4;
                constexpr uint16_t invalid_stack_size=5;
                constexpr uint16_t rsp_out_of_range=6;
                constexpr uint16_t invalid_task_type=7;
            }
        }
        constexpr uint8_t schedule_and_switch=2;
        namespace schedule_and_switch_results{
            namespace fatal_reasons{
                constexpr uint16_t empty_no_idle=1;
                constexpr uint16_t null_task_ptr=2;
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
                constexpr uint16_t bad_task_type=1;
                constexpr uint16_t context_nullptr=2;
                constexpr uint16_t context_null_stack_size=3;
                constexpr uint16_t context_stackptr_out_of_range=4;
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t sleep_task_insert=7;
        namespace sleep_task_insert_results{
            namespace fail_reasons{
                constexpr uint16_t null_task_ptr=1;
                constexpr uint16_t bad_task_type=2;
                constexpr uint16_t insert_fail=3;
            }
        }
        constexpr uint8_t kthread_sleep=6;
        namespace kthread_sleep_results{ 
            namespace fatal_reasons{
                constexpr uint16_t bad_task_state=1;
                constexpr uint16_t context_nullptr=2;
                constexpr uint16_t context_null_stack_size=3;
                constexpr uint16_t context_stackptr_out_of_range=4;
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t kthread_wait=8;
        namespace kthread_wait_results{
            namespace fatal_reasons{
                constexpr uint16_t bad_task_state=1;
                constexpr uint16_t context_stackptr_out_of_range=4;
            } 
        }
        constexpr uint8_t kthread_exit=9;
        namespace kthread_exit_results{
            namespace fatal_reasons{
                constexpr uint16_t bad_task_state=1;
                constexpr uint16_t waiter_bad_block_reason=2;
                constexpr uint16_t context_stackptr_out_of_range=4;
            }
        }
        constexpr uint8_t kthread_block_queue=10;
        namespace kthread_block_queue_results{
            namespace fatal_reasons{
                constexpr uint16_t bad_task_type=1;
                constexpr uint16_t context_nullptr=2;
                constexpr uint16_t context_null_stack_size=3;
                constexpr uint16_t context_stackptr_out_of_range=4;
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t kthread_block_queue_if_equal=11;
        namespace kthread_block_queue_if_equal_results{
            namespace fatal_reasons{
                constexpr uint16_t bad_task_type=1;
                constexpr uint16_t context_nullptr=2;
                constexpr uint16_t context_null_stack_size=3;
                constexpr uint16_t context_stackptr_out_of_range=4;
                constexpr uint16_t illeage_state=5;
            }
        }
        constexpr uint8_t kthread_common_save=12;
        namespace kthread_common_save_results{
            namespace fatal_reasons{
                constexpr uint16_t bad_task_type=1;
                constexpr uint16_t context_nullptr=2;
                constexpr uint16_t bad_task_state=3;
                constexpr uint16_t context_stackptr_out_of_range=4;
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
enum task_blocked_reason_t:uint8_t{
    invalid,
    sleeping,
    mutex,
    no_job,
    wait_other_kthread
};
namespace kthread_call_num{
    constexpr uint64_t exit=0;
    constexpr uint64_t sleep=1;
    constexpr uint64_t yield=2;
    constexpr uint64_t wait=3;
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
    private:
    task_state_t task_state;
    uint32_t belonged_processor_id;
    uint64_t tid;
    public:
    static  task* basic_constructor();
    void launch();   // 纯机械：从 priv_ctx 加载上下文开始执行
    void resume();   // 根据 ctx_choose 无脑加载对应上下文
    bool set_ready();//成功返回true,但是必须从init/blocked切换到才合法/成功，非法不会改状态字段
    bool set_blocked();//成功返回true,但是必须从running切换到才合法/成功，非法不会改状态字段
    bool set_dead();//成功返回true,只能由zombie切换到才合法/成功，非法不会改状态字段
    bool set_zombie();//合法前驱仅限running,blocked,ready
    bool set_running();
    reentrant_spinlock_cpp_t task_lock;
    void assign_valid_tid(uint64_t tid);
    task_blocked_reason_t blocked_reason;
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
    miusecond_time_stamp_t accumulates_bank[event_type_COUNT];
    void task_event_shift(event_type_t new_event);
    event_type_t current_event;
    miusecond_time_stamp_t current_event_start_stamp;
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
    Ktemplats::list_doubly<task*> waiters;//在锁下的，exit的时候都唤醒
    u_ctx_t*uctx;//指向一个页
    //还有vCPU ctx指针
    enum ctx_choose{
        priv,
        u_ctx,
        vCPU
    };
    ctx_choose choose;
};
struct task_in_pool{
    task*task_ptr;
    uint32_t slot_version;
};
// 专用 huge_bitmap 派生：固定 0=空闲, 1=已占用 语义 + 计数跟踪
// 用于 subtable 的槽位分配跟踪，替代已被移除的 bitmap_t 线性扫描语义
class used_slot_bitmap : public huge_bitmap {
    uint64_t m_used_count = 0;
    spinlock_cpp_t m_count_lock;
public:
    used_slot_bitmap(uint64_t bits) : huge_bitmap(bits) {}

    void used_bit_count_add(uint64_t n) {
        m_count_lock.lock();
        m_used_count += n;
        m_count_lock.unlock();
    }

    void used_bit_count_sub(uint64_t n) {
        m_count_lock.lock();
        if (n >= m_used_count) m_used_count = 0;
        else m_used_count -= n;
        m_count_lock.unlock();
    }

    uint64_t used_count() const { return m_used_count; }
};

// ── wq 句柄系统 ────────────────────────────────────────
// 全局 wait_queue 表，句柄（wq_id_t）代替指针：防伪、防 UAF、可跨进程传递
// 用户态可通过 syscall 使用相同句柄

typedef uint64_t wq_id_t;
constexpr wq_id_t  WQ_ID_INVALID = ~0u;
constexpr uint32_t WQ_TABLE_SIZE = 256;

// 内部队列（对外不暴露）
class block_queue{
    spinlock_cpp_t qlock;
    task::event_type_t queue_event;
    Ktemplats::list_doubly<task*> inner_queue;
};

wq_id_t  wq_alloc();
void     wq_free(wq_id_t qid);
void     wq_wake_one(wq_id_t qid, uint64_t wake_val);
void     wq_wake_all(wq_id_t qid, uint64_t wake_val);
/**
 * 
 */
uint32_t tid_to_idx(uint64_t tid);
class task_pool{
    private:
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();
    static spinrwlock_cpp_t lock;//技术债之写饥饿，后续考虑无锁操作
    static constexpr uint32_t root_table_entry_count=1<<16;
    static constexpr uint32_t sub_table_entry_count=1<<16;
    struct subtable{
        used_slot_bitmap used_bitmap;
        task_in_pool task_table[sub_table_entry_count];
    };
    struct root_entry
    {
        subtable*sub;
        uint32_t last_max_slot_version;
    };
    static root_entry root_table[root_table_entry_count];
    static KURD_t enable_subtable(uint32_t high_idx);
    static KURD_t try_disable_subtable(uint32_t high_idx);
    static uint32_t last_alloc_index;
    /*
    alloc_tid逻辑为从last_alloc_index开始扫描位图，若找到空闲槽位则返回index,并且更新last_alloc_index为index+1，途中若遇到空subtable则创建
    根据index返回(index<<32)|table[index].slot_version(示意写法)为tid
    需要发明kurd错误语义
    */
    static uint64_t alloc_tid(KURD_t&kurd);
    public:
    static task* get_by_tid(uint64_t tid,KURD_t &kurd);
    static uint64_t alloc(
        task* task_ptr,
        KURD_t&kurd
    );//行为为若alloc_tid成功则在对应的tid的槽位填写task_ptr,并返回tid,否则返回～0ll并且传递kurd错误语义
    static KURD_t release_tid(uint64_t tid);
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
    task* idle;
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
    void sched();//会内部修改ready_queue数据结构用ready_queues_lock保护，然后对应的task也会用锁保护其状态改变
    KURD_t insert_ready_task(task*task_ptr, bool front=false);
    void sleep_tasks_wake();

    // ── DTS Gantt 接口 ──
    KURD_t dts_gantt_enable();   // 按需分配 Gantt 缓冲区
    void   dts_gantt_disable();  // 释放缓冲区，置 NULL
    void   dts_gantt_write(task* to_run, uint8_t reason, uint8_t io_urgency);

    per_processor_scheduler();
    ~per_processor_scheduler(); // 析构时确保 gantt 释放
};
extern per_processor_scheduler global_schedulers[MAX_PROCESSORS_COUNT];
constexpr uint32_t INVALID_NODE_INDEX=~0;
extern "C"{
    task* task_spawn(task_type_t type);
    KURD_t task_start(uint64_t insert_pid);
    [[noreturn]] void kthread_yield_true_enter(x64_standard_context* context);
    void kthread_yield();
    uint64_t* get_scheduler_private_stack_top();
    void kthread_exit(uint64_t will);
    uint64_t kthread_wait(uint64_t tid);//注意，若对应的tid不存在返回~0ull
    void kthread_wait_cppenter(x64_standard_context*context);
    [[noreturn]] void kthread_exit_cppenter(x64_standard_context*context);
    void kthread_self_blocked(task_blocked_reason_t reason);
    void kthread_sleep(miusecond_time_stamp_t offset);
    [[noreturn]] void kthread_sleep_cppenter(x64_standard_context* context);
    [[noreturn]] void kthread_self_blocked_cppenter(x64_standard_context* context);
    uint64_t wakeup_thread(uint64_t tid, bool front_insert=false);//返回的是KURD但是受限于abi，需要分析
    void block_queue(wq_id_t qid);
    [[noreturn]] void block_queue_cppenter(x64_standard_context* context);
    void block_if_equal(wq_id_t qid, uint64_t* checker, uint64_t block_token);
    void block_if_equal_cppenter(x64_standard_context* context);
    uint64_t release_kthread(uint64_t tid);
}
/**
 * 内核线程接口里面锁顺序纪律：
 * 1.task锁永远比scheduler的锁先锁
 * 2.wait/exit接口对中waited锁的临界区覆盖waiters锁的临界区
 * 3.block_queue / block_if_equal_cppenter 的 wq 锁临界区覆盖放弃执行流的线程锁
 * 4.task锁临界区内可以调用task_pool相关接口，只在其内部有锁
 */