#include "arch/x86_64/Interrupt_system/loacl_processor.h"
extern "C" const x64_gdtentry bsp_init_gdt_entries[] = {
    {0},                                    // Null descriptor (index 0)
    kspace_CS_entry,                        // 内核代码段 (index 1, selector = 0x08)
    kspace_DS_SS_entry,                     // 内核数据段 (index 2, selector = 0x10)
    userspace_CS_entry,                     // 用户代码段 (index 3, selector = 0x18)
    userspace_DS_SS_entry,                  // 用户数据段 (index 4, selector = 0x20)
    {0}                                     // 预留条目，供TSS使用 (index 5)
};
extern "C" const GDTR bsp_init_gdt_descriptor = {
    .limit = sizeof(bsp_init_gdt_entries) - 1,
    .base = (uint64_t)bsp_init_gdt_entries
};

// 定义前21个IDT项的数组
extern "C" const IDTEntry bsp_init_idt_entries[21] = {
    // ivec::DIVIDE_ERROR (0) - 除零异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&div_by_zero_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&div_by_zero_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&div_by_zero_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::DEBUG (1) - 调试异常
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::NMI (2) - NMI异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&nmi_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&nmi_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&nmi_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::BREAKPOINT (3) - 断点异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&breakpoint_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (3 << 13) |    // dpl: 3 (特权级3)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&breakpoint_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&breakpoint_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::OVERFLOW (4) - 溢出异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&overflow_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (3 << 13) |    // dpl: 3 (特权级3)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&overflow_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&overflow_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::BOUND_RANGE_EXCEEDED (5) - 越界异常
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::INVALID_OPCODE (6) - 无效操作码异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&invalid_opcode_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&invalid_opcode_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&invalid_opcode_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::DEVICE_NOT_AVAILABLE (7) - 设备不可用异常
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::DOUBLE_FAULT (8) - 双重故障异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&double_fault_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&double_fault_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&double_fault_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // 向量9 - 未使用
    {
        .offset_low = 0,
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,
        .offset_high = 0,
        .reserved3 = 0
    },
    // ivec::INVALID_TSS (10) - 无效TSS异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&invalid_tss_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&invalid_tss_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&invalid_tss_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::SEGMENT_NOT_PRESENT (11) - 段不存在异常
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::STACK_SEGMENT_FAULT (12) - 栈段错误异常
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::GENERAL_PROTECTION_FAULT (13) - 一般保护错误异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&general_protection_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&general_protection_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&general_protection_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::PAGE_FAULT (14) - 页面错误异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&page_fault_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&page_fault_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&page_fault_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // 向量15 - 未使用
    {
        .offset_low = 0,
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,
        .offset_high = 0,
        .reserved3 = 0
    },
    // ivec::x87_FPU_FLOATING_POINT_ERROR (16) - x87 FPU 浮点错误
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::ALIGNMENT_CHECK (17) - 对齐检查异常
    {
        .offset_low = 0,   // 没有特定处理函数，置零
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xF << 8) |   // type: 陷阱门 (0b1111 = 15 = 0xF，有错误码)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (0 << 15),     // present: 0 (不存在，置为0)
        .offset_mid = 0,  // 没有特定处理函数，置零
        .offset_high = 0, // 没有特定处理函数，置零
        .reserved3 = 0
    },
    // ivec::MACHINE_CHECK (18) - 机器检查异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&machine_check_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&machine_check_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&machine_check_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::SIMD_FLOATING_POINT_EXCEPTION (19) - SIMD浮点异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&simd_floating_point_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&simd_floating_point_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&simd_floating_point_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    },
    // ivec::VIRTUALIZATION_EXCEPTION (20) - 虚拟化异常
    {
        .offset_low = static_cast<uint16_t>(reinterpret_cast<uint64_t>(&virtualization_bare_enter) & 0xFFFF),
        .segment_selector = x64_local_processor::K_cs_idx << 3,
        .attributes = (0 << 0) |     // ist_index: 0
                      (0 << 3) |     // reserved1: 0
                      (0xE << 8) |   // type: 中断门 (0b1110 = 14 = 0xE)
                      (0 << 12) |    // reserved2: 0
                      (0 << 13) |    // dpl: 0 (特权级0)
                      (1 << 15),     // present: 1
        .offset_mid = static_cast<uint16_t>((reinterpret_cast<uint64_t>(&virtualization_bare_enter) >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((reinterpret_cast<uint64_t>(&virtualization_bare_enter) >> 32) & 0xFFFFFFFF),
        .reserved3 = 0
    }
};

//extern "C" const IDTEntry bsp_init_idt_entries[21]={0};
extern "C" IDTR const bsp_init_idtr={
    .limit = sizeof(bsp_init_idt_entries) - 1,
    .base = (uint64_t)bsp_init_idt_entries
};