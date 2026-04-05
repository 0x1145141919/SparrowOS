#pragma once
#include "stdint.h"
namespace ivec
{
static constexpr uint8_t DIVIDE_ERROR = 0;
static constexpr uint8_t DEBUG = 1;
static constexpr uint8_t NMI = 2;
static constexpr uint8_t BREAKPOINT = 3;
static constexpr uint8_t OVERFLOW = 4;
static constexpr uint8_t BOUND_RANGE_EXCEEDED = 5;
static constexpr uint8_t INVALID_OPCODE = 6;
static constexpr uint8_t DEVICE_NOT_AVAILABLE = 7;
static constexpr uint8_t DOUBLE_FAULT = 8;
static constexpr uint8_t COPROCESSOR_SEGMENT_OVERRUN = 9;
static constexpr uint8_t INVALID_TSS = 10;
static constexpr uint8_t SEGMENT_NOT_PRESENT = 11;
static constexpr uint8_t STACK_SEGMENT_FAULT = 12;
static constexpr uint8_t GENERAL_PROTECTION_FAULT = 13;
static constexpr uint8_t PAGE_FAULT = 14;
static constexpr uint8_t X87_FPU_ERROR = 16;
static constexpr uint8_t ALIGNMENT_CHECK = 17;
static constexpr uint8_t MACHINE_CHECK = 18;
static constexpr uint8_t SIMD_FLOATING_POINT_EXCEPTION = 19;
static constexpr uint8_t VIRTUALIZATION_EXCEPTION = 20;
static constexpr uint8_t CONTROL_PROTECTION_EXCEPTION = 21;
static constexpr uint8_t BOTTOM_FOR_SYSTEM_RESERVED_VECS = 224;
static constexpr uint8_t LAPIC_TIMER = 224;
static constexpr uint8_t ASM_PANIC = 225;//只推荐用int ASM_PANIC触发
static constexpr uint8_t KTHREAD_CALL = 226;
static constexpr uint8_t IPI = 240;
static constexpr uint8_t LAPIC_ERR=241;
};
/**
 * 0～31架构占用
 * 224～255系统占用
 * 中间32～223允许自由分配
 */
