#pragma once
#include <stdint.h>

namespace NVMe {
    namespace io_queue {
        constexpr uint8_t SQ_PRIO_URGENT = 0;
        constexpr uint8_t SQ_PRIO_HIGH   = 1;
        constexpr uint8_t SQ_PRIO_MEDIUM = 2;
        constexpr uint8_t SQ_PRIO_LOW    = 3;

        union create_cq_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t pc : 1;    // Physically Contiguous
                uint32_t ien : 1;   // Interrupts Enabled
                uint32_t : 14;
                uint32_t iv : 16;   // Interrupt Vector
            } __attribute__((packed));
        };

        union create_sq_cdw11_t {
            uint32_t raw;
            struct {
                uint32_t pc : 1;    // Physically Contiguous
                uint32_t qprio : 2; // Queue Priority
                uint32_t : 13;
                uint32_t cqid : 16; // Completion Queue ID
            } __attribute__((packed));
        };
    }
}
