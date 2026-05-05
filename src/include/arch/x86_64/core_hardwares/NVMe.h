#pragma once
#include <block_device.h>
#include <util/Ktemplats.h>
#include <arch/x86_64/PCIe/base.h>
#include <memory/AddresSpace.h>
#include <Scheduler/per_processor_scheduler.h>
struct NVMe_device_private{
    uint32_t controller_id;//node_array中的引索
    uint32_t nsid;
};
namespace NVMe{
    constexpr uint64_t entry_block_token=~0ull;
    enum ctrl_state_t : uint8_t {                                                                                             
      CTRL_UNINIT,                                                                                                          
      CTRL_ENABLING,                                                                                                        
      CTRL_READY,                                                                                                           
      CTRL_FAULT,                                                                                                           
  };
    union cap_reg_t{
        struct{
            uint64_t mqes:16;
            uint64_t cqr:1;
            uint64_t ams:2;
            uint64_t reserved0:5;
            uint64_t to:8;
            uint64_t DSTRD:4;
            uint64_t nssrs:1;
            uint64_t css:8;
            uint64_t bps:1;
            uint64_t cps:2;
            uint64_t mps_min:4;
            uint64_t mqs_max:4;
            uint64_t PMRS:1;
            uint64_t CMBS:1;
            uint64_t NSSS:1;
            uint64_t CRMS:2;
            uint64_t NSSES:1;
            uint64_t reserved1:2;
        }__attribute__((packed)) field;
        static_assert(sizeof(field)==8, "not 8 bytes");
        uint64_t value;
    };
    union controler_ctrl_t{
        uint32_t value;
        struct{
            uint32_t EN:1;
            uint32_t reserved0:3;
            uint32_t CSS:3;
            uint32_t MPS:4;
            uint32_t AMS:3;
            uint32_t SHN:2;
            uint32_t IOSQES:4;
            uint32_t IOCQES:4;
            uint32_t CRIME:1;
            uint32_t reserved1:7;
        }__attribute__((packed)) field;
    };
    namespace command{
        union submit_command_common{ 
            struct{
            uint8_t opcode;
            uint8_t fuse:2;
            uint8_t reserved0:4;
            uint8_t psdt:2;
            uint16_t cid;
            uint32_t nsid;
            uint64_t qword1;
            uint64_t MPTR;
            uint64_t DPTR1;
            uint64_t DPTR2;
            uint64_t left[3];
            }__attribute__((packed)) fiedls;
            static_assert(sizeof(fiedls)==64, "not 64 bytes");
            uint32_t dwords[16];
        };
        union complete_command_common{
            struct{
                uint64_t cmd_spcify;
                uint16_t sq_head_ptr;
                uint16_t sq_id;
                uint16_t cmd_id;
                uint16_t phase:1;
                uint16_t status:15;
            }__attribute__((packed)) fields;
            static_assert(sizeof(fields)==16, "not 8 bytes");  
            uint32_t dwords[4];
        };
    };
    namespace status {
            // Success
            constexpr uint16_t SUCCESSFUL_COMPLETION = 0x00;
            
            // Generic Command Status (00h - 7Fh)
            constexpr uint16_t INVALID_COMMAND_OPCODE = 0x01;
            constexpr uint16_t INVALID_FIELD_IN_COMMAND = 0x02;
            constexpr uint16_t COMMAND_ID_CONFLICT = 0x03;
            constexpr uint16_t DATA_TRANSFER_ERROR = 0x04;
            constexpr uint16_t COMMANDS_ABORTED_DUE_TO_POWER_LOSS_NOTIFICATION = 0x05;
            constexpr uint16_t INTERNAL_ERROR = 0x06;
            constexpr uint16_t COMMAND_ABORT_REQUESTED = 0x07;
            constexpr uint16_t COMMAND_ABORTED_DUE_TO_SQ_DELETION = 0x08;
            constexpr uint16_t COMMAND_ABORTED_DUE_TO_FAILED_FUSED_COMMAND = 0x09;
            constexpr uint16_t COMMAND_ABORTED_DUE_TO_MISSING_FUSED_COMMAND = 0x0A;
            constexpr uint16_t INVALID_NAMESPACE_OR_FORMAT = 0x0B;
            constexpr uint16_t COMMAND_SEQUENCE_ERROR = 0x0C;
            constexpr uint16_t INVALID_SGL_SEGMENT_DESCRIPTOR = 0x0D;
            constexpr uint16_t INVALID_NUMBER_OF_SGL_DESCRIPTORS = 0x0E;
            constexpr uint16_t DATA_SGL_LENGTH_INVALID = 0x0F;
            constexpr uint16_t METADATA_SGL_LENGTH_INVALID = 0x10;
            constexpr uint16_t SGL_DESCRIPTOR_TYPE_INVALID = 0x11;
            constexpr uint16_t INVALID_USE_OF_CONTROLLER_MEMORY_BUFFER = 0x12;
            constexpr uint16_t PRP_OFFSET_INVALID = 0x13;
            constexpr uint16_t ATOMIC_WRITE_UNIT_EXCEEDED = 0x14;
            constexpr uint16_t OPERATION_DENIED = 0x15;
            constexpr uint16_t SGL_OFFSET_INVALID = 0x16;
            // 0x17 Reserved
            constexpr uint16_t HOST_IDENTIFIER_INCONSISTENT_FORMAT = 0x18;
            constexpr uint16_t KEEP_ALIVE_TIMER_EXPIRED = 0x19;
            constexpr uint16_t KEEP_ALIVE_TIMEOUT_INVALID = 0x1A;
            constexpr uint16_t COMMAND_ABORTED_DUE_TO_PREEMPT_AND_ABORT = 0x1B;
            constexpr uint16_t SANITIZE_FAILED = 0x1C;
            constexpr uint16_t SANITIZE_IN_PROGRESS = 0x1D;
            constexpr uint16_t SGL_DATA_BLOCK_GRANULARITY_INVALID = 0x1E;
            constexpr uint16_t COMMAND_NOT_SUPPORTED_FOR_QUEUE_IN_CMB = 0x1F;
            constexpr uint16_t NAMESPACE_IS_WRITE_PROTECTED = 0x20;
            constexpr uint16_t COMMAND_INTERRUPTED = 0x21;
            constexpr uint16_t TRANSIENT_TRANSPORT_ERROR = 0x22;
            constexpr uint16_t COMMAND_PROHIBITED_BY_COMMAND_AND_FEATURE_LOCKDOWN = 0x23;
            constexpr uint16_t ADMIN_COMMAND_MEDIA_NOT_READY = 0x24;
            constexpr uint16_t INVALID_KEY_TAG = 0x25;
            constexpr uint16_t HOST_DISPERSED_NAMESPACE_SUPPORT_NOT_ENABLED = 0x26;
            constexpr uint16_t HOST_IDENTIFIER_NOT_INITIALIZED = 0x27;
            constexpr uint16_t INCORRECT_KEY = 0x28;
            constexpr uint16_t FDP_DISABLED = 0x29;
            constexpr uint16_t INVALID_PLACEMENT_HANDLE_LIST = 0x2A;
            // 0x2B to 0x7F Reserved
            
            // I/O Command Set Specific Status (80h - BFh)
            constexpr uint16_t LBA_OUT_OF_RANGE = 0x80;
            constexpr uint16_t CAPACITY_EXCEEDED = 0x81;
            constexpr uint16_t NAMESPACE_NOT_READY = 0x82;
            constexpr uint16_t RESERVATION_CONFLICT = 0x83;
            constexpr uint16_t FORMAT_IN_PROGRESS = 0x84;
            constexpr uint16_t INVALID_VALUE_SIZE = 0x85;   // KV command set
            constexpr uint16_t INVALID_KEY_SIZE = 0x86;     // KV command set
            
            // Helper: Check if status indicates error
            constexpr bool is_error(uint16_t status) {
                return status != SUCCESSFUL_COMPLETION;
            }
            
            // Helper: Check if retry might succeed (Do Not Retry bit is cleared)
            // Note: Do Not Retry bit is bit 0 of status code (not included in these values)
            // Actual retry logic needs to check the Status Field in CQE
    }
        namespace command {
        // Admin Command Opcodes (Figure 142)
        namespace admin_opcode {
            // 00h - 0Fh
            constexpr uint8_t DELETE_IO_SUBMISSION_QUEUE   = 0x00;
            constexpr uint8_t CREATE_IO_SUBMISSION_QUEUE   = 0x01;
            constexpr uint8_t GET_LOG_PAGE                  = 0x02;
            constexpr uint8_t DELETE_IO_COMPLETION_QUEUE    = 0x04;
            constexpr uint8_t CREATE_IO_COMPLETION_QUEUE    = 0x05;
            constexpr uint8_t IDENTIFY                      = 0x06;
            constexpr uint8_t ABORT                         = 0x08;
            constexpr uint8_t SET_FEATURES                  = 0x09;
            constexpr uint8_t GET_FEATURES                  = 0x0A;
            constexpr uint8_t ASYNCHRONOUS_EVENT_REQUEST    = 0x0C;
            constexpr uint8_t NAMESPACE_MANAGEMENT          = 0x0D;
            
            // 10h - 1Fh
            constexpr uint8_t FIRMWARE_COMMIT               = 0x10;
            constexpr uint8_t FIRMWARE_IMAGE_DOWNLOAD       = 0x11;
            constexpr uint8_t DEVICE_SELF_TEST              = 0x14;
            constexpr uint8_t NAMESPACE_ATTACHMENT          = 0x15;
            constexpr uint8_t KEEP_ALIVE                    = 0x18;
            constexpr uint8_t DIRECTIVE_SEND                = 0x19;
            constexpr uint8_t DIRECTIVE_RECEIVE             = 0x1A;
            constexpr uint8_t VIRTUALIZATION_MANAGEMENT     = 0x1C;
            constexpr uint8_t NVME_MI_SEND                  = 0x1D;
            constexpr uint8_t NVME_MI_RECEIVE               = 0x1E;
            
            // 20h - 2Fh
            constexpr uint8_t CAPACITY_MANAGEMENT            = 0x20;
            constexpr uint8_t DISCOVERY_INFORMATION_MANAGEMENT = 0x21;
            constexpr uint8_t FABRIC_ZONING_RECEIVE          = 0x22;
            constexpr uint8_t LOCKDOWN                       = 0x24;
            constexpr uint8_t FABRIC_ZONING_LOOKUP           = 0x25;
            constexpr uint8_t CLEAR_EXPORTED_NVM_RESOURCE_CONFIG = 0x28;
            constexpr uint8_t FABRIC_ZONING_SEND             = 0x29;
            constexpr uint8_t CREATE_EXPORTED_NVM_SUBSYSTEM  = 0x2A;
            constexpr uint8_t MANAGE_EXPORTED_NVM_SUBSYSTEM  = 0x2D;
            
            // 30h - 3Fh
            constexpr uint8_t MANAGE_EXPORTED_NAMESPACE      = 0x31;
            constexpr uint8_t MANAGE_EXPORTED_PORT           = 0x35;
            constexpr uint8_t SEND_DISCOVERY_LOG_PAGE        = 0x39;
            constexpr uint8_t TRACK_SEND                     = 0x3D;
            
            // 40h - 4Fh
            constexpr uint8_t MIGRATION_SEND                 = 0x41;
            constexpr uint8_t MIGRATION_RECEIVE              = 0x42;
            
            // 7Ch - 7Fh
            constexpr uint8_t CONTROLLER_DATA_QUEUE          = 0x45;
            constexpr uint8_t DOORBELL_BUFFER_CONFIG         = 0x7C;
            constexpr uint8_t FABRICS_COMMANDS               = 0x7F;  // Note: Subcommand in CDW10
            
            // 80h - 8Fh
            constexpr uint8_t FORMAT_NVM                     = 0x80;
            constexpr uint8_t SECURITY_SEND                  = 0x81;
            constexpr uint8_t SECURITY_RECEIVE               = 0x82;
            constexpr uint8_t SANITIZE                       = 0x84;
            constexpr uint8_t LOAD_PROGRAM                   = 0x85;
            constexpr uint8_t GET_LBA_STATUS                 = 0x86;
            constexpr uint8_t PROGRAM_ACTIVATION_MANAGEMENT  = 0x88;
            constexpr uint8_t MEMORY_RANGE_SET_MANAGEMENT    = 0x89;
            
            // Vendor Specific range (C0h - FFh)
            constexpr uint8_t VENDOR_SPECIFIC_BASE           = 0xC0;
            
            // Helper: Check if opcode is vendor specific
            constexpr bool is_vendor_specific(uint8_t opcode) {
                return opcode >= VENDOR_SPECIFIC_BASE;
            }
            
            // Helper: Check if opcode is valid (not reserved)
            constexpr bool is_valid(uint8_t opcode) {
                // 0x03, 0x07, 0x0B, 0x0E-0x0F, 0x12-0x13, 0x16-0x17, etc are reserved
                switch (opcode) {
                    case 0x03: case 0x07: case 0x0B: 
                    case 0x0E: case 0x0F: 
                    case 0x12: case 0x13:
                    case 0x16: case 0x17:
                    case 0x1B: case 0x1F:
                    case 0x23: case 0x26: case 0x27:
                    case 0x2B: case 0x2C: case 0x2E: case 0x2F:
                    case 0x32: case 0x33: case 0x34: 
                    case 0x37: case 0x38: 
                    case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:
                    case 0x43: case 0x44: 
                    case 0x46 ... 0x7B:
                    case 0x7D: case 0x7E:
                    case 0x83: case 0x87:
                    case 0x8A ... 0xBF:
                        return false;
                    default:
                        return true;
                }
            }
        }
         // I/O Command Opcodes
        // Figure 21 (NVM Commands) + Figure 556 (I/O Commands)
        namespace io_opcode {
            // NVM Command Set (Figure 21) - 基础 I/O 命令
            constexpr uint8_t FLUSH                    = 0x00;
            constexpr uint8_t WRITE                    = 0x01;
            constexpr uint8_t READ                     = 0x02;
            constexpr uint8_t WRITE_UNCORRECTABLE      = 0x04;
            constexpr uint8_t COMPARE                  = 0x05;
            constexpr uint8_t WRITE_ZEROES             = 0x08;
            constexpr uint8_t DATASET_MANAGEMENT       = 0x09;
            constexpr uint8_t VERIFY                   = 0x0C;
            
            // Reservation Commands (from Figure 556 and Figure 21)
            constexpr uint8_t RESERVATION_REGISTER     = 0x0D;
            constexpr uint8_t RESERVATION_REPORT       = 0x0E;
            constexpr uint8_t RESERVATION_ACQUIRE      = 0x11;
            constexpr uint8_t RESERVATION_RELEASE      = 0x15;
            
            // I/O Management Commands (Figure 556)
            constexpr uint8_t IO_MANAGEMENT_RECEIVE    = 0x12;
            constexpr uint8_t IO_MANAGEMENT_SEND       = 0x19;  // Check: Figure 556 says 0x19?
            
            // Cancel command (Figure 556)
            constexpr uint8_t CANCEL                   = 0x18;
            
            // Copy command (Figure 21)
            constexpr uint8_t COPY                     = 0x19;  // Figure 21 says 0x19
            
            // Fabric Commands (Figure 556)
            constexpr uint8_t FABRIC_COMMANDS          = 0x7F;
            
            // Vendor Specific range (80h - FFh)
            constexpr uint8_t VENDOR_SPECIFIC_BASE     = 0x80;
            
            // Data transfer direction (bits 1:0 of combo opcode)
            // 00b = no data transfer
            // 01b = host to controller (Write)
            // 10b = controller to host (Read)
            // 11b = bidirectional
            
            // Helper: Check if opcode is vendor specific
            constexpr bool is_vendor_specific(uint8_t opcode) {
                return opcode >= VENDOR_SPECIFIC_BASE;
            }
            
            // Helper: Get data transfer direction from opcode
            // Note: This is based on the combined opcode bits 1:0
            constexpr uint8_t get_transfer_direction(uint8_t opcode) {
                return opcode & 0x03;
            }
            
            constexpr uint8_t DIR_NO_DATA      = 0x00;
            constexpr uint8_t DIR_HOST_TO_CTRL = 0x01;  // Write-like
            constexpr uint8_t DIR_CTRL_TO_HOST = 0x02;  // Read-like
            constexpr uint8_t DIR_BIDIRECTIONAL = 0x03;
            
            // Helper: Check if opcode is valid (not reserved)
            constexpr bool is_valid(uint8_t opcode) {
                // Reserved opcodes from Figure 21: 03h, 06h-07h, 0Ah-0Bh, 0Fh-10h, 13h-14h, 16h-17h, 1Ah-7Eh
                switch (opcode) {
                    case 0x03: case 0x06: case 0x07:
                    case 0x0A: case 0x0B:
                    case 0x0F: case 0x10:
                    case 0x13: case 0x14:
                    case 0x16: case 0x17:
                    case 0x1A ... 0x7E:
                        return false;
                    default:
                        return true;
                }
            }
            
            // Helper: Get command name (for debugging)
        }
    }
};
namespace DEVICES_locs{
    constexpr uint8_t NVMe=0x1;
    namespace NVMe_events{
        constexpr uint8_t Init=0; 
        namespace init_results{
            namespace fail_reasons{
                constexpr uint16_t already_init=0;
                constexpr uint16_t ctrl_disable_timeout=1;
                constexpr uint16_t ctrl_enable_timeout=2;
                constexpr uint16_t identify_ctrl_fail=3;
            }
        };
        constexpr uint8_t submit_command=1;
        namespace submit_command_results{ 
            namespace fail_results{ 
                constexpr uint16_t tail_increas_fail_dueto_unrelased_cmd=1;
            };
        };
        constexpr uint8_t Read=3;
        constexpr uint8_t Write=4;
        constexpr uint8_t flush=5;
        constexpr uint8_t discard=6;
        constexpr uint8_t wz=7;
        constexpr uint8_t cmp=8;
        constexpr uint8_t pre_init=9;
        namespace pre_init_results{ 
            namespace fail_reasons{ 
                constexpr uint16_t this_is_nullptr=0;
                constexpr uint16_t dev_not_found=1;
                constexpr uint16_t bar0_is_io=2;
            };
        };
        constexpr uint8_t msix_vec_alloc=10;
        namespace msix_vec_alloc_results{ 
            namespace fail_reasons{ 
                constexpr uint16_t param_out_of_range=1;
            };
        };
        constexpr uint8_t msix_vec_dealloc=11;
        namespace msix_vec_dealloc_results{ 
            namespace fail_reasons{ 
                constexpr uint16_t processor_not_found=1;
            };
        };
        constexpr uint8_t Offline=0xff;
    };
};
constexpr uint64_t soon_ring_bell_mask=1;//此mask置1时候马上doorbell按下尝试阻塞，反之则内部提交压力到一定程度后再发
class NVMe_Controller{
    public:
    struct node{
        uint16_t pcie_seg;
        uint8_t pcie_bus;
        uint8_t pcie_dev:5;
        uint8_t pcie_func:3;
        vaddr_t ecam_va;
        NVMe_Controller* controller;
    };
    private:
    NVMe::ctrl_state_t state;
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_failure();
    static KURD_t default_fatal();
    uint32_t ADmin_queue_belonged_processor;//getter
    uint8_t ADmin_queue_vec;//getter
    uint8_t* IO_CQ_vecs;//getter，长度为本机器逻辑处理器数
    static constexpr uint16_t DEFAULT_IO_SQ_ENTRY_COUNT=256;
    static constexpr uint16_t DEFAULT_IO_CQ_ENTRY_COUNT=1024;
    uint16_t IO_SQ_ENTRY_COUNT;
    uint16_t IO_CQ_ENTRY_COUNT;
    vm_interval bar_intervals[6];
    vaddr_t ecam;
    struct head_regs_t{
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
    }__attribute__((packed));
    static_assert(sizeof(head_regs_t) == 0x1000,"head_regs size mismatch");
    struct sq_complex{
        uint16_t sqid;
        uint16_t num_of_entries;
        uint16_t belonged_cqid;
        uint16_t tail_idx;//to_wirte
        Ktemplats::kernel_bitmap* sq_bitmap;
        vm_interval sq_ring;//sq里面的命令的CID严格为引索号
        uint64_t*block_tokens;//sq里面的命令的CID严格为引索号，block_tokens[CID]的值是CID,block_tokens[CID]在
        //void block_if_equal(tid_wait_queue* block_queue,uint64_t*checker,uint64_t block_token);接口中block_token为～0，非~0的情况是KURD的uint64_traw,自行提取
        //线程上下文会用这个接口进行阻塞唤醒
    };
    //sq的提交，阻塞，返回释放
    struct cq_complex{
        uint16_t cqid;
        uint16_t num_of_entries;
        uint16_t cq_head_ptr;
        uint16_t head_idx;
        vm_interval cq_ring; 
        bool is_first_time;
        bool unprocessed_entry_expect;
        //NVMe规范中的CQ每个队列的每个回合新提交的命令的phase_tag期望的bool值
        tid_wait_queue wait_queue;//这个队列的锁锁住的是cq以及下辖所有sq的写
    };
    uint16_t sq_count;
    sq_complex*sqs;//对应引索号即队列号
    uint16_t cq_count;
    cq_complex*cqs;//对应引索号即队列号
    head_regs_t* head_regs;
    uint32_t* doorbells;
    uint32_t doorbell_stride_u32;  // (4 << CAP.DSTRD) / 4, 缓存避免重复读 CAP
    BlockDevice*NSs;
    uint32_t NS_count;
    void cq_dorbell_write(uint32_t qid,uint32_t value);
    void sq_dorbell_write(uint32_t qid,uint32_t value);
    MSIX_entry_t* msix_table;
    uint16_t max_msix_vectors;
    uint64_t*msix_pending_bits_array;
    uint32_t cq_dorbell_read(uint32_t qid);
    uint32_t sq_dorbell_read(uint32_t qid);
    static void ADMIN_CQ_handler(void*ctx,uint8_t vec,uint32_t proc_id);
    static void IO_CQ_handler(void*ctx,uint8_t vec,uint32_t proc_id);
    static KURD_t Init(uint64_t flags);//这个接口做成这个样子是刻意让其能够被create_kthread的
    bool wait_for_ready(head_regs_t *regs, bool target, uint32_t timeout_500ms);
    KURD_t second_stage_init();//这里才是实质性的初始化
    KURD_t pre_init();//ecam相关预初始化
    KURD_t msix_vec_alloc(uint32_t processor_id,uint16_t msix_vec);
    KURD_t msix_vec_free(uint16_t msix_vec);
    public:
    static node* node_array;
    static uint32_t controllers_count;
    NVMe_Controller(vaddr_t ecam);
    static KURD_t device_init(NVMe_Controller* dev);
    KURD_t offline(uint64_t flags);
    // 通用接口：任意队列
    uint64_t cmd_submit_and_process(uint16_t qid,
    NVMe::command::submit_command_common cmd, KURD_t& kurd);
    void cq_interrupt_handler(uint16_t qid);

    // Admin 队列专用包装
    void ADMIN_CQ_interrupt_handler();
    void IO_CQ_interrupt_handler(uint32_t proc_id);
    uint64_t ADMIN_cmd_submit_and_process(NVMe::command::submit_command_common cmd,KURD_t&kurd);//提交，返回时的uint64_t是status
    KURD_t IO_cmd_submit(NVMe::command::submit_command_common cmd,uint32_t proc_id,bool soon_ring_bell);
    static KURD_t read(BlockDevice* dev,uint64_t sector,uint32_t count,void* buf,uint64_t flags);
    static KURD_t write(BlockDevice* dev,uint64_t sector,uint32_t count,void* buf,uint64_t flags);
    static KURD_t flush(BlockDevice* dev,uint64_t flags);
    static KURD_t discard(BlockDevice* dev,uint64_t sector,uint32_t count,uint64_t flags);
    static KURD_t write_zero(BlockDevice* dev,uint64_t sector,uint32_t count,uint64_t flags);
    static KURD_t compare(BlockDevice* dev,uint64_t sector,uint32_t count,void* buf,uint64_t flags);
};

extern void register_nvme_kshell_commands();
