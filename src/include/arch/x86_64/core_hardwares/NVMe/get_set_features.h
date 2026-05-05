#pragma once
#include <stdint.h>

namespace NVMe {
    namespace features {
        // Feature Identifiers (FID)
        constexpr uint8_t FID_NUMBER_OF_QUEUES            = 0x07;
        constexpr uint8_t FID_INTERRUPT_COALESCING        = 0x08;
        constexpr uint8_t FID_INTERRUPT_VECTOR_CONFIG     = 0x09;
        constexpr uint8_t FID_ASYNC_EVENT_CONFIG          = 0x0B;
        constexpr uint8_t FID_HOST_CTRL_THERMAL_MGMT      = 0x10;
        constexpr uint8_t FID_HOST_MEMORY_BUFFER          = 0x0D;

        // Get Features Select (SEL)
        constexpr uint8_t GET_SEL_CURRENT     = 0;
        constexpr uint8_t GET_SEL_DEFAULT     = 1;
        constexpr uint8_t GET_SEL_SAVED       = 2;
        constexpr uint8_t GET_SEL_SUPPORTED_CAPS = 3;
    }

    namespace features_detail {
        // Number of Queues (FID 07h)
        union nq_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t nsqr : 16;  // Number of I/O Submission Queues Requested
                uint32_t ncqr : 16;  // Number of I/O Completion Queues Requested
            } __attribute__((packed));
        };

        union nq_cqe0_t {
            uint32_t raw;
            struct {
                uint32_t nsqr_minus_1 : 16;  // (NSQR - 1) allocated
                uint32_t ncqr_minus_1 : 16;  // (NCQR - 1) allocated
            } __attribute__((packed));
        };

        // Interrupt Coalescing (FID 08h)
        union int_coal_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t thr : 8;   // Aggregation Threshold
                uint32_t time : 8;  // Aggregation Time (in 100us units)
                uint32_t : 16;
            } __attribute__((packed));
        };

        // Interrupt Vector Config (FID 09h)
        union iv_config_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t iv : 16;   // Interrupt Vector
                uint32_t cd : 1;    // Coalescing Disable
                uint32_t : 15;
            } __attribute__((packed));
        };

        // Async Event Config (FID 0Bh)
        union aer_cfg_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t sm : 1;    // SMART / Health
                uint32_t err : 1;   // Error Log
                uint32_t ns : 1;    // Namespace Attribute Notices
                uint32_t fw : 1;    // Firmware Activation Notices
                uint32_t tel : 1;   // Telemetry Log Notices
                uint32_t : 27;
            } __attribute__((packed));
        };

        // Host Memory Buffer (FID 0Dh)
        // CDW11: bits[31:16]=reserved, bits[3]=CTZ, bits[2]=HMNARE,
        //        bits[1]=MR, bits[0]=EHM (Figure 448)
        union hmb_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t ehm    : 1;  // bit 0  Enable Host Memory
                uint32_t mr     : 1;  // bit 1  Memory Return
                uint32_t hmnare : 1;  // bit 2  Host Mem Non-op Access Restrict
                uint32_t ctz    : 1;  // bit 3  Cleared to Zero
                uint32_t        : 28; // bits 31:04 reserved
            } __attribute__((packed));
        };

        // HMB Descriptor Entry (Figure 454): 16 bytes, physically contiguous
        struct __attribute__((packed)) hmb_descriptor_t {
            uint64_t baddr;   // Buffer Address (0-7), MPS-aligned
            uint32_t bsize;   // Buffer Size (8-11), in MPS units
            uint32_t rsvd;    // Reserved (12-15)
        };
        static_assert(sizeof(hmb_descriptor_t) == 16,
                      "hmb_descriptor_t must be 16 bytes");

        // Host Controller Thermal Management (FID 10h)
        // CDW11: bits[31:16] = TMT2, bits[15:0] = TMT1 (Figure 399)
        union hctm_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t tmt1 : 16;  // bits[15:0] = Thermal Management Temperature 1
                uint32_t tmt2 : 16;  // bits[31:16] = Thermal Management Temperature 2
            } __attribute__((packed));
        };
    }
}
