#pragma once

#include <stdint.h>

namespace gdtentry {
constexpr uint8_t execute_only_type = 0b1001;
constexpr uint8_t read_write_type = 0b0011;
}

struct x64_gdtentry {
    uint16_t limit0;
    uint16_t base0;
    uint16_t base1 : 8, type : 4, s : 1, dpl : 2, p : 1;
    uint16_t limit1 : 4, avl : 1, l : 1, d : 1, g : 1, base2 : 8;
} __attribute__((packed));

static constexpr x64_gdtentry kspace_DS_SS_entry = {
    .limit0 = 0xFFFF,
    .base0 = 0x0000,
    .base1 = 0x00,
    .type = gdtentry::read_write_type,
    .s = 1,
    .dpl = 0,
    .p = 1,
    .limit1 = 0b1111,
    .avl = 0,
    .l = 0,
    .d = 1,
    .g = 1,
    .base2 = 0x00,
};

static constexpr x64_gdtentry kspace_CS_entry = {
    .limit0 = 0xFFFF,
    .base0 = 0x0000,
    .base1 = 0x00,
    .type = gdtentry::execute_only_type,
    .s = 1,
    .dpl = 0,
    .p = 1,
    .limit1 = 0b1111,
    .avl = 0,
    .l = 1,
    .d = 0,
    .g = 1,
    .base2 = 0x00,
};

static constexpr x64_gdtentry userspace_DS_SS_entry = {
    .limit0 = 0xFFFF,
    .base0 = 0x0000,
    .base1 = 0x00,
    .type = gdtentry::read_write_type,
    .s = 1,
    .dpl = 3,
    .p = 1,
    .limit1 = 0b1111,
    .avl = 0,
    .l = 0,
    .d = 1,
    .g = 1,
    .base2 = 0x00,
};

static constexpr x64_gdtentry userspace_CS_entry = {
    .limit0 = 0xFFFF,
    .base0 = 0x0000,
    .base1 = 0x00,
    .type = gdtentry::execute_only_type,
    .s = 1,
    .dpl = 3,
    .p = 1,
    .limit1 = 0b1111,
    .avl = 0,
    .l = 1,
    .d = 0,
    .g = 1,
    .base2 = 0x00,
};

struct IDTEntry {
    uint16_t offset_low;
    uint16_t segment_selector;
    union {
        struct {
            uint8_t ist_index : 3;
            uint8_t reserved1 : 5;
            uint8_t type : 4;
            uint8_t reserved2 : 1;
            uint8_t dpl : 2;
            uint8_t present : 1;
        } __attribute__((packed));
        uint16_t attributes;
    };
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved3;
} __attribute__((packed));

struct GDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct IDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class x64_local_processor {
  public:
    static constexpr uint8_t K_cs_idx = 0x1;
};

extern "C" char div_by_zero_bare_enter;
extern "C" char breakpoint_bare_enter;
extern "C" char nmi_bare_enter;
extern "C" char overflow_bare_enter;
extern "C" char invalid_opcode_bare_enter;
extern "C" char general_protection_bare_enter;
extern "C" char double_fault_bare_enter;
extern "C" char page_fault_bare_enter;
extern "C" char machine_check_bare_enter;
extern "C" char invalid_tss_bare_enter;
extern "C" char simd_floating_point_bare_enter;
extern "C" char virtualization_bare_enter;
