#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "arch/x86_64/Interrupt_system/Interrupt.h"
#include "util/lock.h"
#include "Interrupt_errors.h"
#include "util/Ktemplats.h"
namespace gdtentry
{
    constexpr uint8_t execute_only_type = 0b1001;
    constexpr uint8_t read_write_type = 0b0011;
}
namespace INTERRUPT_SUB_MODULES_LOCATIONS{
    namespace PROCESSORS_EVENT_CODE{
        constexpr uint8_t EVENT_CODE_RUNTIME_RESGIS=1;//主要对应的是x64_local_processor::x64_local_processor构造函数
        constexpr uint8_t EVENT_CODE_APS_INIT = 2;
        namespace APS_INIT_RESULTS_CODE{
            namespace RETRY_REASON_CODE{
                constexpr uint16_t RETRY_REASON_CODE_DEPENDIES_NOT_INITIALIZED = 1;
            }
            namespace PARTIAL_SUCCESS_CODE{
                constexpr uint8_t PARTIAL_SUCCESS_CODE_SOME_APS_IPI_TIME_OUT = 1;
            }
            namespace FATAL_REASON{
                constexpr uint8_t AP_STAGE_FAIL = 1;
            }
        }
    }
}
typedef uint32_t x2apicid_t;
struct x64_gdtentry {
	uint16_t	limit0;
	uint16_t	base0;
	uint16_t	base1: 8, type: 4, s: 1, dpl: 2, p: 1;
	uint16_t	limit1: 4, avl: 1, l: 1, d: 1, g: 1, base2: 8;
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
    .base2 = 0x00
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
    .base2 = 0x00
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
    .base2 = 0x00
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
    .base2 = 0x00
};
struct IDTEntry {
    uint16_t offset_low;      // 中断处理程序地址的低16位 (位 15-0)
    uint16_t segment_selector;// 代码段选择子 (位 15-0)
    // 属性字段 (位 31-16)
    union {
        struct {
            uint8_t ist_index : 3;   // 中断栈表索引 (位 2-0)
            uint8_t reserved1 : 5;   // 保留位 (位 7-3)
            uint8_t type : 4;        // 门类型 (位 11-8)
            uint8_t reserved2 : 1;   // 保留位 (位 12)
            uint8_t dpl : 2;         // 描述符特权级 (位 14-13)
            uint8_t present : 1;     // 存在标志 (位 15)
        } __attribute__((packed));
        uint16_t attributes;
    };
    uint16_t offset_mid;      // 中断处理程序地址的中16位 (位 31-16)
    uint32_t offset_high;     // 中断处理程序地址的高32位 (位 63-32)
    uint32_t reserved3;       // 保留位 (位 95-64/31-0)
}__attribute__((packed));
struct logical_idt{
    void*handler;
    uint8_t type;
    uint8_t ist_index;
    uint8_t dpl;
};

struct TSSentry
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t ist[8];//根据文档ist[0]不使用,必须分配为NULL
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base_offset;
    //简化设计，不配置io_map，用户态不准用io_map只允许内核态使用
} __attribute__((packed));
static constexpr uint64_t base0_mask = 0xFFFF;
static constexpr uint64_t base1_mask = 0xFFULL;
static constexpr uint64_t base2_mask = 0xFFULL;
static constexpr uint64_t base3_mask = 0xFFFFFFFFULL;
struct TSSDescriptorEntry
{
    uint16_t limit;
    uint16_t base0;
    uint8_t base1;
    uint8_t type : 4, zero : 1, dpl : 2, p : 1;
    uint8_t limit1 : 4, avl : 1, reserved : 2, g : 1;
    uint8_t base2;
    uint32_t base3;
    uint32_t reserved2;
}__attribute__((packed));

struct GDTR
{
    uint16_t limit;
    uint64_t base;
}__attribute__((packed));
struct IDTR
{
    uint16_t limit;
    uint64_t base;
}__attribute__((packed));
static constexpr uint8_t gdt_headcount = 0x6;
struct x64GDT
{
    x64_gdtentry entries[gdt_headcount];
    TSSDescriptorEntry tss_entry;
}__attribute__((packed));
constexpr uint32_t  GS_SLOT_MAX_ENTRY_COUNT = 0x40;
typedef uint64_t GS_struct[GS_SLOT_MAX_ENTRY_COUNT];
typedef uint64_t FS_struct[6];
constexpr uint8_t  STACK_PROTECTOR_CANARY_IDX = 0x5;
constexpr uint8_t K_cs_idx = 0x1;
constexpr uint8_t K_ds_ss_idx = 0x2;
constexpr uint8_t U_cs_idx = 0x3;
constexpr uint8_t U_ds_ss_idx = 0x4;
constexpr uint32_t  RSP0_STACKSIZE= 0x8000;
constexpr uint32_t  DF_STACKSIZE= 0x8000;
constexpr uint32_t  MC_STACKSIZE= 0x8000;
constexpr uint32_t  NMI_STACKSIZE= 0x8000;
constexpr uint32_t  IDLE_TASK_STACKSIZE= 0x8000;
constexpr uint32_t  total_stack_size= IDLE_TASK_STACKSIZE+RSP0_STACKSIZE+DF_STACKSIZE+MC_STACKSIZE+NMI_STACKSIZE;
constexpr uint32_t  L_PROCESSOR_GS_IDX= 0;
extern "C" KURD_t ap_init_one_by_one();
/* ── x2APIC ID → gs_complex_t 映射表 ─────────────────────────────
 * 由 APs_bringup.cpp 的两遍扫描分配填写，ap_init (kinit.cpp) 查表
 * 注意不可直接引用 GS_complex.h（circular dep），仅作指针 */
struct gs_complex_t;
extern gs_complex_t** g_gs_by_apicid;
