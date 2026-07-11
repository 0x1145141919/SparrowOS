#pragma once
#include <stdint.h>
#include <abi/os_error_definitions.h>

namespace NVMe {
    constexpr uint64_t entry_block_token = ~0ull;

    enum ctrl_state_t : uint8_t {
        CTRL_UNINIT,
        CTRL_ENABLING,
        CTRL_READY,
        CTRL_FAULT,
    };

    union cap_reg_t {
        struct {
            uint64_t mqes : 16;
            uint64_t cqr : 1;
            uint64_t ams : 2;
            uint64_t reserved0 : 5;
            uint64_t to : 8;
            uint64_t DSTRD : 4;
            uint64_t nssrs : 1;
            uint64_t css : 8;
            uint64_t bps : 1;
            uint64_t cps : 2;
            uint64_t mps_min : 4;
            uint64_t mqs_max : 4;
            uint64_t PMRS : 1;
            uint64_t CMBS : 1;
            uint64_t NSSS : 1;
            uint64_t CRMS : 2;
            uint64_t NSSES : 1;
            uint64_t reserved1 : 2;
        } __attribute__((packed)) field;
        static_assert(sizeof(field) == 8, "not 8 bytes");
        uint64_t value;
    };

    union controler_ctrl_t {
        uint32_t value;
        struct {
            uint32_t EN : 1;
            uint32_t reserved0 : 3;
            uint32_t CSS : 3;
            uint32_t MPS : 4;
            uint32_t AMS : 3;
            uint32_t SHN : 2;
            uint32_t IOSQES : 4;
            uint32_t IOCQES : 4;
            uint32_t CRIME : 1;
            uint32_t reserved1 : 7;
        } __attribute__((packed)) field;
    };

    namespace command {
        union submit_command_common {
            struct {
                uint8_t opcode;
                uint8_t fuse : 2;
                uint8_t reserved0 : 4;
                uint8_t psdt : 2;
                uint16_t cid;
                uint32_t nsid;
                uint64_t qword1;
                uint64_t MPTR;
                uint64_t DPTR1;
                uint64_t DPTR2;
                uint64_t left[3];
            } __attribute__((packed)) fiedls;
            static_assert(sizeof(fiedls) == 64, "not 64 bytes");
            uint32_t dwords[16];
        };

        union complete_command_common {
            struct {
                uint64_t cmd_spcify;
                uint16_t sq_head_ptr;
                uint16_t sq_id;
                uint16_t cmd_id;
                uint16_t phase : 1;
                uint16_t status : 15;
            } __attribute__((packed)) fields;
            static_assert(sizeof(fields) == 16, "not 8 bytes");
            uint32_t dwords[4];
        };
        
        
    };
    namespace command_result_types{
        constexpr uint8_t command_executed=0;
        constexpr uint8_t timeout=1;
        constexpr uint8_t not_success_kurd=2;

    }
    union command_result_t {
        struct {
                uint64_t cmd_spcify;
                uint64_t result_type:4;
                uint64_t reserved:45;
                uint64_t status : 15;
            } __attribute__((packed)) fields;
            static_assert(sizeof(fields) == 16, "not 8 bytes");
        uint32_t dwords[4];
    };
    inline command_result_t make_not_success_kurd(KURD_t kurd)
    {
        command_result_t r;
        r.fields.result_type = command_result_types::not_success_kurd;
        r.fields.cmd_spcify = kurd_get_raw(kurd);
        return r;
    }
    namespace status {
        constexpr uint16_t SUCCESSFUL_COMPLETION = 0x00;
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
        constexpr uint16_t LBA_OUT_OF_RANGE = 0x80;
        constexpr uint16_t CAPACITY_EXCEEDED = 0x81;
        constexpr uint16_t NAMESPACE_NOT_READY = 0x82;
        constexpr uint16_t RESERVATION_CONFLICT = 0x83;
        constexpr uint16_t FORMAT_IN_PROGRESS = 0x84;
        constexpr uint16_t INVALID_VALUE_SIZE = 0x85;
        constexpr uint16_t INVALID_KEY_SIZE = 0x86;

        constexpr bool is_error(uint16_t status) {
            return status != SUCCESSFUL_COMPLETION;
        }
    }

    namespace command {
        namespace admin_opcode {
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
            constexpr uint8_t CAPACITY_MANAGEMENT            = 0x20;
            constexpr uint8_t DISCOVERY_INFORMATION_MANAGEMENT = 0x21;
            constexpr uint8_t FABRIC_ZONING_RECEIVE          = 0x22;
            constexpr uint8_t LOCKDOWN                       = 0x24;
            constexpr uint8_t FABRIC_ZONING_LOOKUP           = 0x25;
            constexpr uint8_t CLEAR_EXPORTED_NVM_RESOURCE_CONFIG = 0x28;
            constexpr uint8_t FABRIC_ZONING_SEND             = 0x29;
            constexpr uint8_t CREATE_EXPORTED_NVM_SUBSYSTEM  = 0x2A;
            constexpr uint8_t MANAGE_EXPORTED_NVM_SUBSYSTEM  = 0x2D;
            constexpr uint8_t MANAGE_EXPORTED_NAMESPACE      = 0x31;
            constexpr uint8_t MANAGE_EXPORTED_PORT           = 0x35;
            constexpr uint8_t SEND_DISCOVERY_LOG_PAGE        = 0x39;
            constexpr uint8_t TRACK_SEND                     = 0x3D;
            constexpr uint8_t MIGRATION_SEND                 = 0x41;
            constexpr uint8_t MIGRATION_RECEIVE              = 0x42;
            constexpr uint8_t CONTROLLER_DATA_QUEUE          = 0x45;
            constexpr uint8_t DOORBELL_BUFFER_CONFIG         = 0x7C;
            constexpr uint8_t FABRICS_COMMANDS               = 0x7F;
            constexpr uint8_t FORMAT_NVM                     = 0x80;
            constexpr uint8_t SECURITY_SEND                  = 0x81;
            constexpr uint8_t SECURITY_RECEIVE               = 0x82;
            constexpr uint8_t SANITIZE                       = 0x84;
            constexpr uint8_t LOAD_PROGRAM                   = 0x85;
            constexpr uint8_t GET_LBA_STATUS                 = 0x86;
            constexpr uint8_t PROGRAM_ACTIVATION_MANAGEMENT  = 0x88;
            constexpr uint8_t MEMORY_RANGE_SET_MANAGEMENT    = 0x89;
            constexpr uint8_t VENDOR_SPECIFIC_BASE           = 0xC0;

            constexpr bool is_vendor_specific(uint8_t opcode) { return opcode >= VENDOR_SPECIFIC_BASE; }
        }

        namespace io_opcode {
            constexpr uint8_t FLUSH                    = 0x00;
            constexpr uint8_t WRITE                    = 0x01;
            constexpr uint8_t READ                     = 0x02;
            constexpr uint8_t WRITE_UNCORRECTABLE      = 0x04;
            constexpr uint8_t COMPARE                  = 0x05;
            constexpr uint8_t WRITE_ZEROES             = 0x08;
            constexpr uint8_t DATASET_MANAGEMENT       = 0x09;
            constexpr uint8_t VERIFY                   = 0x0C;
            constexpr uint8_t RESERVATION_REGISTER     = 0x0D;
            constexpr uint8_t RESERVATION_REPORT       = 0x0E;
            constexpr uint8_t RESERVATION_ACQUIRE      = 0x11;
            constexpr uint8_t RESERVATION_RELEASE      = 0x15;
            constexpr uint8_t IO_MANAGEMENT_RECEIVE    = 0x12;
            constexpr uint8_t CANCEL                   = 0x18;
            constexpr uint8_t COPY                     = 0x19;
            constexpr uint8_t FABRIC_COMMANDS          = 0x7F;
            constexpr uint8_t VENDOR_SPECIFIC_BASE     = 0x80;

            constexpr bool is_vendor_specific(uint8_t opcode) { return opcode >= VENDOR_SPECIFIC_BASE; }
            constexpr uint8_t get_transfer_direction(uint8_t opcode) { return opcode & 0x03; }
            constexpr uint8_t DIR_NO_DATA      = 0x00;
            constexpr uint8_t DIR_HOST_TO_CTRL = 0x01;
            constexpr uint8_t DIR_CTRL_TO_HOST = 0x02;
            constexpr uint8_t DIR_BIDIRECTIONAL = 0x03;
        }
    }
}

namespace DEVICES_locs {
    constexpr uint8_t NVMe = 0x1;

    namespace NVMe_events {
        constexpr uint8_t Init = 0;
        namespace init_results {
            namespace fail_reasons {
                constexpr uint16_t already_init = 0;
                constexpr uint16_t ctrl_disable_timeout = 1;
                constexpr uint16_t ctrl_enable_timeout = 2;
                constexpr uint16_t identify_ctrl_fail = 3;
            }
        }
        constexpr uint8_t submit_command = 1;
        namespace submit_command_results {
            namespace fail_results {
                constexpr uint16_t tail_increas_fail_dueto_unrelased_cmd = 1;
            }
        }
        constexpr uint8_t Read = 3;
        constexpr uint8_t Write = 4;
        constexpr uint8_t flush = 5;
        constexpr uint8_t discard = 6;
        constexpr uint8_t wz = 7;
        constexpr uint8_t cmp = 8;
        constexpr uint8_t pre_init = 9;
        namespace pre_init_results {
            namespace fail_reasons {
                constexpr uint16_t this_is_nullptr = 0;
                constexpr uint16_t dev_not_found = 1;
                constexpr uint16_t bar0_is_io = 2;
            }
        }
        constexpr uint8_t msix_vec_alloc = 10;
        namespace msix_vec_alloc_results {
            namespace fail_reasons {
                constexpr uint16_t param_out_of_range = 1;
            }
        }
        constexpr uint8_t msix_vec_dealloc = 11;
        namespace msix_vec_dealloc_results {
            namespace fail_reasons {
                constexpr uint16_t processor_not_found = 1;
            }
        }
        constexpr uint8_t set_features = 12;
        namespace set_features_results {
            namespace fail_reasons {
                constexpr uint16_t nvme_status_nonzero = 0;
            }
        }
        constexpr uint8_t io_queue_mgmt = 13;
        namespace io_queue_mgmt_results {
            namespace fail_reasons {
                constexpr uint16_t nvme_status_nonzero = 0;
            }
        }
        constexpr uint8_t Offline = 0xff;
    }
}

// AER event type constants
namespace aer_event_type {
    constexpr uint8_t ERROR_STATUS    = 0x01;
    constexpr uint8_t SMART_HEALTH    = 0x02;
    constexpr uint8_t NOTICE          = 0x07;
    constexpr uint8_t IO_CMD_SPECIFIC = 0x08;
    constexpr uint8_t ONE_SHOT        = 0x09;

    namespace error_info {
        // Error Information Log (Log Identifier 01h) entry types
        constexpr uint8_t WRITE_FAULT           = 0x00;
        constexpr uint8_t READ_FAULT            = 0x01;
        constexpr uint8_t UNRECOVERED_DATA_INTEGRITY_ERROR = 0x04;
        constexpr uint8_t PERSISTENT_MEDIA_REGION_WRITE_FAULT = 0x05;
        constexpr uint8_t PERSISTENT_MEDIA_REGION_READ_FAULT  = 0x06;
        constexpr uint8_t INVALID_DATA_SGL_SEGMENT            = 0x08;
        constexpr uint8_t MEMORY_SPACE_EXCEEDANCE            = 0x0E;
        constexpr uint8_t LBA_OUT_OF_RANGE                   = 0x0F;
        constexpr uint8_t NAMESPACE_IS_READ_ONLY             = 0x12;
        constexpr uint8_t INTERNAL_TARGET_ERROR              = 0xF0;
    }

    namespace smart_info {
        constexpr uint8_t TEMPERATURE_THRESHOLD    = 0x01;
        constexpr uint8_t SPARE_BELOW_THRESHOLD    = 0x02;
        constexpr uint8_t NVM_RELIABILITY_DEGRADED = 0x04;
        constexpr uint8_t MEDIA_READ_ONLY          = 0x08;
        constexpr uint8_t VOLATILE_MEMORY_BACKUP_DEVICE_FAILED = 0x10;
        constexpr uint8_t PMR_UNRELIABLE           = 0x20;
    }

    namespace notice_info {
        constexpr uint8_t FIRMWARE_ACTIVATION_STARTING     = 0x01;
        constexpr uint8_t NAMESPACE_ATTRIBUTE_CHANGED      = 0x02;
        constexpr uint8_t NVM_SUBSYSTEM_HARDWARE_CHANGED   = 0x03;
        constexpr uint8_t CHANGED_NAMESPACE_LIST           = 0x04;
        constexpr uint8_t TELEMETRY_LOG_CREATED            = 0x05;
        constexpr uint8_t ASYMMETRIC_NAMESPACE_ACCESS_CHANGED = 0x06;
        constexpr uint8_t PREDICTABLE_LATENCY_CHANGED      = 0x07;
        constexpr uint8_t LBA_STATUS_INFORMATION_ALERT     = 0x08;
        constexpr uint8_t ENDURANCE_GROUP_EVENT            = 0x09;
        constexpr uint8_t EXPORTED_NVM_RESOURCE_CONFIG_CHANGED = 0x0A;
        constexpr uint8_t DEALLOCATED_OR_UNWRITTEN_DATA_ERROR = 0x0B;
        constexpr uint8_t EXPORTED_NVM_RESOURCE_CONFIG_LOST    = 0x0C;
        constexpr uint8_t ZONE_EVENT_NOTIFICATION              = 0x0D;
    }

    namespace io_specific_info {
        constexpr uint8_t RESERVATION_PREEMPTED       = 0x01;
        constexpr uint8_t RESERVATION_RELEASED        = 0x02;
        constexpr uint8_t RESERVATION_PREEMPT_AND_ABORT = 0x03;
    }
}

constexpr uint64_t soon_ring_bell_mask = 1;
