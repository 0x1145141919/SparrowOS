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
enum task_type_t:uint8_t{
    kthreadm,
    userthread,
    vCPU
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
constexpr uint8_t DEFAULT_STACK_PG_COUNT=7;
constexpr uint32_t DEFAULT_STACK_SIZE=DEFAULT_STACK_PG_COUNT*4096;
struct kthread_context{
    x64_standard_context regs;
    vaddr_t stack_bottom;
    uint16_t stacksize;
};
struct userthread_context{
    x64_standard_context regs;
    //预留给cr3,pcid
    //预留给XSAVE等等
};
struct vCPU_context{
    //预留给VMCS等等
};
constexpr miusecond_time_stamp_t DEFALUT_TIMER_SPAN_MIUS=20000;
constexpr uint64_t INIT_DEFAULT_RFLAGS=0x202;
extern "C" void secure_hlt();
class task{
    private:
    task_state_t task_state;
    task_type_t task_type;
    uint32_t belonged_processor_id;
    uint64_t tid;
    public:
    task(task_type_t task_type,void*context);//原子性初始化一个task
    void atomic_load();//原子性状态切换到running并且加载对应的上下文（必然会切换到上下文制定的rsp与rip）
    bool set_ready();//成功返回true,但是必须从init/blocked切换到才合法/成功，非法不会改状态字段
    bool set_blocked();//成功返回true,但是必须从running切换到才合法/成功，非法不会改状态字段
    bool set_dead();//成功返回true,只能由zombie切换到才合法/成功，非法不会改状态字段
    bool set_zombie();//合法前驱仅限running,blocked,ready
    bool set_running();
    reentrant_spinlock_cpp_t task_lock;
    ~task();//析构函数,原本是想只有dead才能运行，但是受abi制约，行为是无条件折构，只不过非dead状态会warning记录到日志
    void assign_valid_tid(uint64_t tid);
    static constexpr uint8_t task_not_in_term=0;
    static constexpr uint8_t task_in_term=1;
    u8ka task_in_term_flag;//标记任务是否在被迁移的途中
    static constexpr uint8_t task_not_on_queue=0;
    static constexpr uint8_t task_on_queue=1;
    task_blocked_reason_t blocked_reason;
    miusecond_time_stamp_t accumulated_time;
    miusecond_time_stamp_t lastest_run_stamp;
    miusecond_time_stamp_t lastest_span_length;
    miusecond_time_stamp_t sleep_wakeup_stamp;//若状态为blocked,且blocked_reason为sleeping,则sleep_wakeup_stamp有效,系统时间戳大于等于sleep_wakeup_stamp,才允许解除阻塞
    uint32_t get_belonged_processor_id();
    void set_belonged_processor_id(uint32_t pid);
    union{
        kthread_context*kthread;
        userthread_context*userthread;
        vCPU_context*vCPU;
    }context;
    Ktemplats::list_doubly<uint64_t> waiters;//在锁下的，exit的时候都唤醒
    task_state_t get_state();
    uint64_t get_tid();
    task_type_t get_task_type();
};
struct task_in_pool{
    task*task_ptr;
    uint32_t slot_version;
};
class tid_wait_queue:Ktemplats::list_doubly<uint64_t>{
    
    public:
    using list_doubly::push_back;
    using list_doubly::pop_front_value;
    void wakeup_all();//假定外部有锁，调用此接口唤醒所有等待者
    spinlock_cpp_t lock;
};
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
        huge_bitmap used_bitmap;
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
class alignas(64) per_processor_scheduler { 
    private:
    KURD_t default_kurd();
    KURD_t default_success();
    KURD_t default_fail();
    KURD_t default_fatal();
    task* idle;
    public:
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
    KURD_t insert_ready_task(task*task_ptr);
    void sleep_tasks_wake();
    per_processor_scheduler();
};
extern per_processor_scheduler global_schedulers[MAX_PROCESSORS_COUNT];
constexpr uint32_t INVALID_NODE_INDEX=~0;
extern "C"{
    uint64_t create_kthread(void*(*entry)(void*),void*arg,KURD_t*out_kurd);
    void kthread_yield_true_enter(x64_standard_context* context);
    void kthread_yield();
    uint64_t* get_scheduler_private_stack_top();
    void kthread_exit(uint64_t will);
    uint64_t kthread_wait(uint64_t tid);//注意，若对应的tid不存在返回~0ull
    void kthread_wait_cppenter(x64_standard_context*context);
    void kthread_exit_cppenter(x64_standard_context*context);
    void kthread_self_blocked(task_blocked_reason_t reason);
    void kthread_sleep(miusecond_time_stamp_t offset);
    void kthread_sleep_cppenter(x64_standard_context* context);
    void kthread_self_blocked_cppenter(x64_standard_context* context);
    uint64_t wakeup_thread(uint64_t tid);//返回的是KURD但是受限于abi，需要分析
    void block_queue(tid_wait_queue* block_queue);
    void block_queue_cppenter(x64_standard_context* context);
    void block_if_equal(tid_wait_queue* block_queue,uint64_t*checker,uint64_t block_token);
    void block_if_equal_cppenter(x64_standard_context* context);
}
/**
 * 内核线程接口里面锁顺序纪律：
 * 1.task锁永远比scheduler的锁先锁
 * 2.wait/exit接口对中waited锁的临界区覆盖waiters锁的临界区
 * 3.block_queue和block_if_equal_cppenter的block_queue锁临界区覆盖放弃执行流的线程的锁的临界区
 * 4.task锁临界区内可以调用task_pool相关接口，只在其内部有锁
 */