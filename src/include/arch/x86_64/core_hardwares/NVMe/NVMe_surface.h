#pragma once
#include <arch/x86_64/core_hardwares/NVMe/base.h>
#include <block_device.h>
#include <util/Ktemplats.h>
#include <arch/x86_64/PCIe/base.h>
#include <memory/AddresSpace.h>
#include <Scheduler/per_processor_scheduler.h>

struct NVMe_device_private {
    uint32_t controller_id;
    uint32_t nsid;
};

class NVMe_Controller {
public:
    struct node {
        uint16_t pcie_seg;
        uint8_t pcie_bus;
        uint8_t pcie_dev : 5;
        uint8_t pcie_func : 3;
        vaddr_t ecam_va;
        NVMe_Controller* controller;
    };

    struct sq_rq_t {
        NVMe::command::complete_command_common sq_entry;
        uint64_t block_token;
    };

private:
    NVMe::ctrl_state_t state;
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_failure();
    static KURD_t default_fatal();

    uint32_t ADmin_queue_belonged_processor;
    uint8_t ADmin_queue_vec;
    uint8_t* IO_CQ_vecs;

    static constexpr uint16_t DEFAULT_IO_SQ_ENTRY_COUNT = 256;
    static constexpr uint16_t DEFAULT_IO_CQ_ENTRY_COUNT = 1024;
    uint16_t IO_SQ_ENTRY_COUNT;
    uint16_t IO_CQ_ENTRY_COUNT;
    vm_interval bar_intervals[6];
    vaddr_t ecam;

    struct head_regs_t {
        uint64_t cap;
        uint32_t vs;
        uint32_t interrupt_mask_set;
        uint32_t interrupt_mask_clear;
        uint32_t controller_configuration;
        uint32_t reserved0;
        uint32_t controller_status;
        uint32_t nvm_subsystem_reset;
        uint32_t admin_queue_attributes;
        uint64_t admin_submission_queue_base;
        uint64_t admin_completion_queue_base;
        uint32_t controller_memory_buffer_location;
        uint32_t controller_memory_buffer_size;
        uint32_t boot_partion_info;
        uint32_t boot_partion_read_select;
        uint64_t boot_partion_memory_buffer_location;
        uint64_t controller_memory_buffer_space_control;
        uint32_t controller_memory_buffer_status;
        uint32_t controller_memoty_buffer_elastic_buffer_size;
        uint32_t controller_memory_buffer_sustained_write_throughput;
        uint32_t NVM_subsystem_shutdown;
        uint32_t controller_ready_timeouts;
        uint32_t reserved1[869];
        uint32_t PMRCAP;
        uint32_t PMRCTL;
        uint32_t PMRSTS;
        uint32_t PMREBS;
        uint32_t PMRSWTP;
        uint32_t PMRMSCL;
        uint32_t PMRMSCU;
        uint32_t reserved2[121];
    } __attribute__((packed));
    static_assert(sizeof(head_regs_t) == 0x1000, "head_regs size mismatch");

    struct sq_complex {
        uint16_t sqid;
        uint16_t num_of_entries;
        uint16_t belonged_cqid;
        uint16_t tail_idx;
        Ktemplats::kernel_bitmap* sq_bitmap;
        vm_interval sq_ring;
        sq_rq_t* block_tokens;
    };

    struct cq_complex {
        uint16_t cqid;
        uint16_t num_of_entries;
        uint16_t cq_head_ptr;
        uint16_t head_idx;
        vm_interval cq_ring;
        bool is_first_time;
        bool unprocessed_entry_expect;
        tid_wait_queue wait_queue;
    };

    uint16_t sq_count;
    sq_complex* sqs;
    uint16_t cq_count;
    cq_complex* cqs;
    head_regs_t* head_regs;
    uint32_t* doorbells;
    uint32_t doorbell_stride_u32;
    BlockDevice* NSs;
    uint32_t NS_count;

    void cq_dorbell_write(uint32_t qid, uint32_t value);
    void sq_dorbell_write(uint32_t qid, uint32_t value);
    MSIX_entry_t* msix_table;
    uint16_t max_msix_vectors;
    uint64_t* msix_pending_bits_array;
    uint32_t cq_dorbell_read(uint32_t qid);
    uint32_t sq_dorbell_read(uint32_t qid);

    static void ADMIN_CQ_handler(void* ctx, uint8_t vec, uint32_t proc_id);
    static void IO_CQ_handler(void* ctx, uint8_t vec, uint32_t proc_id);
    static KURD_t Init(uint64_t flags);
    bool wait_for_ready(head_regs_t* regs, bool target, uint32_t timeout_500ms);
    KURD_t second_stage_init();
    KURD_t pre_init();
    KURD_t msix_vec_alloc(uint32_t processor_id, uint16_t msix_vec);
    KURD_t msix_vec_free(uint16_t msix_vec);

    // AER
    static constexpr uint8_t AER_batch_size = 16;
    static constexpr uint16_t AER_base_cid = 0xf000;
    vm_interval admin_buffer;
    vm_interval hmb_buffer;
public:
    static node* node_array;
    static uint32_t controllers_count;

    NVMe_Controller(vaddr_t ecam);
    static KURD_t device_init(NVMe_Controller* dev);
    KURD_t offline(uint64_t flags);

    NVMe::command::complete_command_common cmd_submit_and_process(
        uint16_t qid,
        NVMe::command::submit_command_common cmd,
        KURD_t& kurd);

    void cq_interrupt_handler(uint16_t qid);
    void ADMIN_CQ_interrupt_handler();
    void IO_CQ_interrupt_handler(uint32_t proc_id);

    NVMe::command::complete_command_common ADMIN_cmd_submit_and_process(
        NVMe::command::submit_command_common cmd,
        KURD_t& kurd);

    KURD_t IO_cmd_submit(NVMe::command::submit_command_common cmd,
                         uint32_t proc_id,
                         bool soon_ring_bell);

    static KURD_t read(BlockDevice* dev, uint64_t sector, uint32_t count,void* buf, uint64_t flags);
    static KURD_t write(BlockDevice* dev, uint64_t sector, uint32_t count,
                         void* buf, uint64_t flags);
    static KURD_t flush(BlockDevice* dev, uint64_t flags);
    static KURD_t discard(BlockDevice* dev, uint64_t sector, uint32_t count,
                           uint64_t flags);
    static KURD_t write_zero(BlockDevice* dev, uint64_t sector, uint32_t count,
                              void* buf, uint64_t flags);
    static KURD_t compare(BlockDevice* dev, uint64_t sector, uint32_t count,
                           void* buf, uint64_t flags);

    // Identify wrappers
    KURD_t identify_ctrl(phyaddr_t buf_pa, KURD_t& kurd);
    KURD_t identify_ns(uint32_t nsid, phyaddr_t buf_pa, KURD_t& kurd);
    KURD_t identify_ns_list(uint32_t nsid, phyaddr_t buf_pa, KURD_t& kurd);
    KURD_t identify_ns_indep(uint32_t nsid, phyaddr_t buf_pa, KURD_t& kurd);
    KURD_t identify_primary_ctrl_caps(uint16_t cntid, phyaddr_t buf_pa, KURD_t& kurd);

    // AER methods
    void aer_submit(uint16_t aer_index, KURD_t& kurd);
    void aer_handle_error(uint32_t info);
    void aer_handle_smart_health(uint32_t info);
    void aer_handle_notice(uint32_t info);
    void aer_handle_io_cmd_specific(uint32_t info);
    void aer_handle_one_shot(uint32_t info);

    // Get/Set Features
    NVMe::command::complete_command_common get_features_cmd(
        uint8_t fid, uint8_t sel, KURD_t& kurd);
    NVMe::command::complete_command_common set_features_cmd(
        uint8_t fid, uint32_t cdw11, phyaddr_t buf_pa, KURD_t& kurd);
    KURD_t get_features_num_queues(KURD_t& kurd);
    KURD_t set_features_num_queues(uint16_t nsqr, uint16_t ncqr, KURD_t& kurd);
    KURD_t get_features_int_coalescing(KURD_t& kurd);
    KURD_t set_features_int_coalescing(uint8_t thr, uint8_t time, KURD_t& kurd);
    KURD_t get_features_int_vector_config(KURD_t& kurd);
    KURD_t set_features_int_vector_config(uint16_t iv, KURD_t& kurd);
    KURD_t get_features_async_event_config(KURD_t& kurd);
    KURD_t set_features_async_event_config(uint8_t sm, uint8_t err, uint8_t ns,
                                            uint8_t fw, uint8_t tel, KURD_t& kurd);
    KURD_t get_features_hctm(KURD_t& kurd);
    KURD_t set_features_hctm(uint16_t tmt2, uint16_t tmt1, KURD_t& kurd);

    // IO Queue management
    NVMe::command::complete_command_common queue_mgmt_cmd(
        uint8_t opcode, uint16_t qid, uint16_t qsize,
        uint32_t cdw11, phyaddr_t prp1, KURD_t& kurd);
    KURD_t create_io_cq(uint16_t qid, uint16_t qsize, bool ien, KURD_t& kurd);
    KURD_t create_io_sq(uint16_t qid, uint16_t qsize, uint16_t cqid,
                         uint8_t qprio, KURD_t& kurd);
    KURD_t delete_io_sq(uint16_t qid, KURD_t& kurd);
    KURD_t delete_io_cq(uint16_t qid, KURD_t& kurd);

    // HMB 分配（second_stage_init 调用）
    // 从 Identify Controller HMPRE 读取推荐大小，启用到主控
    KURD_t hmb_alloc(KURD_t& kurd);
    // HMB 释放（offline 调用）
    KURD_t hmb_free(KURD_t& kurd);

    // I/O 队列释放（offline 调用）：先删所有 SQ，再删所有 CQ
    KURD_t io_queue_free(KURD_t& kurd);

    // I/O 队列批量初始化
    // iosq_count = 要创建的 I/O SQ 数量（包含全部 SQID）
    // iocq_count = 要创建的 I/O CQ 数量（包含全部 CQID）
    // iocq_count <= iosq_count <= cap.mqes
    // CQID [1, iocq_count], 每个绑定到 CPU (qid-1), 使用 MSI-X vector qid
    // SQID [1, iosq_count], round-robin 匹配到 CQ
    KURD_t io_queue_init(uint16_t iosq_count, uint16_t iocq_count, KURD_t& kurd);

    // Poll thread
    void start_poll_thread(uint16_t qid, KURD_t& kurd);
};

extern void register_nvme_kshell_commands();
