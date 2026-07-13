#pragma once
#include <stdint.h>
#include "abi/os_error_definitions.h"
#include "ktime.h"

namespace Scheduler {
    constexpr uint8_t SCHEDULER          = 1;
    constexpr uint8_t KTHREADS           = 2;
    constexpr uint8_t TASK_POOL          = 3;
    constexpr uint8_t BLOCK_QUEUE_SYSTEM = 4;

    namespace SCHEDULER_EVENTS {
        namespace COMMON_FATAL_REASONS {
            constexpr uint16_t STATE_TRANSITION_FAIL = 0;
        }
        namespace COMMON_FAIL_REASONS {
            constexpr uint16_t NULL_TASK_PTR = 0;
            constexpr uint16_t BAD_TASK_TYPE = 1;
            constexpr uint16_t INSERT_FAIL   = 2;
        }
        constexpr uint8_t EVENT_CODE_INSERT_READY_TASK = 1;
        constexpr uint8_t EVENT_CODE_SET_STATE         = 2;
        namespace insert_ready_task_results {}
    }

    namespace KTHREADS_EVENTS {
        namespace COMMON_FAIL_REASONS {
            constexpr uint16_t NULL_PARAM     = 0;
            constexpr uint16_t BAD_TASK_STATE = 1;
        }
        namespace COMMON_FATAL_REASONS {
            constexpr uint16_t BAD_TASK_STATE        = 0;
            constexpr uint16_t PRIVCTX_STACKPTR_OOR  = 1;
            constexpr uint16_t NOT_SUPPORTED_CTX     = 2;
            constexpr uint16_t NULL_RUNNING_TASK     = 3;
            constexpr uint16_t STATE_TRANSITION_FAIL = 4;
        }
        constexpr uint8_t EVENT_CODE_KTHREAD_INIT        = 1;
        namespace kthread_init_results {}
        constexpr uint8_t EVENT_CODE_TASK_LAUNCH         = 2;
        namespace task_launch_results {
            namespace FAIL_REASONS {
                constexpr uint16_t INVALID_TID            = 2;
                constexpr uint16_t TARGET_NOT_KERNEL_ADDR = 3;
                constexpr uint16_t NOT_PRIV_CTX           = 4;
                constexpr uint16_t STATE_TRANSITION_FAIL  = 5;
            }
        }
        constexpr uint8_t EVENT_CODE_RELEASE_KTHREAD     = 3;
        namespace release_kthread_results {
            namespace FAIL_REASONS {
                constexpr uint16_t TASK_NOT_ZOMBIE = 2;
            }
        }
        constexpr uint8_t EVENT_CODE_WAKEUP_THREAD       = 4;
        namespace wakeup_thread_results {
            namespace SUCCESS_REASONS {
                constexpr uint16_t OTHER_ENTITY_WAKEUP       = 1;
                constexpr uint16_t ALREADY_RUNNING_OR_WAKEUP = 2;
            }
            namespace FAIL_REASONS {
                constexpr uint16_t TASK_ON_BLOCK_QUEUE = 2;
                constexpr uint16_t BAD_TASK_STATE      = 3;
            }
        }
        constexpr uint8_t EVENT_CODE_KTHREAD_COMMON_SAVE = 5;
        namespace kthread_common_save_results {
            namespace FAIL_REASONS {
                constexpr uint16_t NULL_RUNNING_TASK = 2;
            }
        }
        constexpr uint8_t EVENT_CODE_SET_STATE = 6;
    }

    namespace TASK_POOL_EVENTS {
        namespace COMMON_FAIL_REASONS {
            constexpr uint16_t NOT_FOUND = 0;
            constexpr uint16_t BAD_TID   = 1;
        }
        constexpr uint8_t EVENT_CODE_GET_BY_TID = 1;
        namespace get_by_tid_results {
            namespace FAIL_REASONS {}
        }
        constexpr uint8_t EVENT_CODE_RELEASE    = 2;
        namespace release_results {
            namespace FAIL_REASONS {}
        }
    }

    namespace BLOCK_QUEUE_SYSTEM_EVENTS {
        namespace COMMON_FAIL_REASONS {
            constexpr uint16_t NULL_PARAM      = 0;
            constexpr uint16_t INVALID_STATE   = 1;
            constexpr uint16_t QUEUE_NOT_EMPTY = 2;
        }
        constexpr uint8_t EVENT_CODE_PUSH_TAIL    = 1;
        namespace push_tail_results {
            namespace FAIL_REASONS {}
        }
        constexpr uint8_t EVENT_CODE_ENABLE_QUEUE = 2;
        namespace enable_queue_results {
            namespace FAIL_REASONS {}
        }
        constexpr uint8_t EVENT_CODE_DISABLE_QUEUE = 3;
        namespace disable_queue_results {
            namespace FAIL_REASONS {}
        }
        constexpr uint8_t EVENT_CODE_BQ_FREE      = 4;
        namespace bq_free_results {
            namespace FAIL_REASONS {
                constexpr uint16_t QUEUE_NOT_FOUND = 3;
                constexpr uint16_t REMOVE_FAIL     = 4;
            }
        }
    }
};

enum task_state_t : uint8_t {
    init = 0,
    ready,
    running,
    blocked,
    zombie,
    dead
};

enum task_blocked_reason_t : uint8_t {
    invalid,
    sleeping,
    mutex,
    no_job
};

namespace kthread_call_num {
    constexpr uint64_t exit = 0;
    constexpr uint64_t sleep = 1;
    constexpr uint64_t yield = 2;
    constexpr uint64_t block = 4;
    constexpr uint64_t block_to_queue = 5;
    constexpr uint64_t block_to_queue_if_equal = 6;
};

constexpr uint8_t DEFAULT_PRIVSTACK_PGS_COUNT = 4;
constexpr uint64_t INVALID_TID = ~0ull;
constexpr miusecond_time_stamp_t DEFALUT_TIMER_SPAN_MIUS = 20000;
constexpr uint64_t INIT_DEFAULT_RFLAGS = 0x202;
constexpr uint32_t INVALID_NODE_INDEX = ~0;

typedef uint64_t bq_id_t;
constexpr bq_id_t BQ_ID_INVALID = ~0u;

enum zombie_observe_results_t {
    ZOMBIE_DEAD,
    ZOMBIE_ALIVE,
    ZOMBIE_TID_NOT_FOUND
};

struct dts_gantt_entry {
    uint64_t tsc;
    uint32_t tid;
    uint16_t dts_timeslice_us;
    uint8_t  reason : 4;
    uint8_t  preempt_cnt : 2;
    uint8_t  voluntary_cnt : 2;
    uint8_t  io_urgency : 2;
} __attribute__((packed));
static_assert(sizeof(dts_gantt_entry) == 16, "dts_gantt_entry size mismatch");


