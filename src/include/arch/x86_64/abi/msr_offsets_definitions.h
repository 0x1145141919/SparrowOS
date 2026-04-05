#pragma once
#include <stdint.h>

// Intel CPU MSR (Model Specific Registers) 偏移值定义
// 来源: Intel® 64 and IA-32 Architectures Software Developer's Manual Volume 4

namespace msr {
    constexpr uint32_t IA32_EFER = 0xC0000080;  
    // APIC相关寄存器
    namespace apic {
        constexpr uint32_t IA32_APIC_BASE = 0x1B;           // APIC Base Address Register // APIC基地址寄存器
        
        // x2APIC寄存器 (MSR地址范围: 0x800-0x8FF)
        // 基本寄存器
        constexpr uint32_t IA32_X2APIC_ID = 0x802;                  // x2APIC ID Register // x2APIC ID寄存器
        constexpr uint32_t IA32_X2APIC_VERSION = 0x803;             // x2APIC Version Register // x2APIC版本寄存器
        constexpr uint32_t IA32_X2APIC_TPR = 0x808;                 // Task Priority Register // 任务优先级寄存器
        constexpr uint32_t IA32_X2APIC_PPR = 0x80A;                 // Processor Priority Register // 处理器优先级寄存器
        constexpr uint32_t IA32_X2APIC_EOI = 0x80B;                 // EOI Register // EOI寄存器(中断结束)
        constexpr uint32_t IA32_X2APIC_LDR = 0x80D;                 // Logical Destination Register // 逻辑目标寄存器
        constexpr uint32_t IA32_X2APIC_SVR = 0x80F;                 // Spurious Interrupt Vector Register // 虚假中断向量寄存器
        constexpr uint32_t IA32_X2APIC_ISR0 = 0x810;                // In-Service Register Bits 31:0 // 服务中寄存器位31:0
        constexpr uint32_t IA32_X2APIC_ISR1 = 0x811;                // In-Service Register Bits 63:32 // 服务中寄存器位63:32
        constexpr uint32_t IA32_X2APIC_ISR2 = 0x812;                // In-Service Register Bits 95:64 // 服务中寄存器位95:64
        constexpr uint32_t IA32_X2APIC_ISR3 = 0x813;                // In-Service Register Bits 127:96 // 服务中寄存器位127:96
        constexpr uint32_t IA32_X2APIC_ISR4 = 0x814;                // In-Service Register Bits 159:128 // 服务中寄存器位159:128
        constexpr uint32_t IA32_X2APIC_ISR5 = 0x815;                // In-Service Register Bits 191:160 // 服务中寄存器位191:160
        constexpr uint32_t IA32_X2APIC_ISR6 = 0x816;                // In-Service Register Bits 223:192 // 服务中寄存器位223:192
        constexpr uint32_t IA32_X2APIC_ISR7 = 0x817;                // In-Service Register Bits 255:224 // 服务中寄存器位255:224
        constexpr uint32_t IA32_X2APIC_TMR0 = 0x818;                // Trigger Mode Register Bits 31:0 // 触发模式寄存器位31:0
        constexpr uint32_t IA32_X2APIC_TMR1 = 0x819;                // Trigger Mode Register Bits 63:32 // 触发模式寄存器位63:32
        constexpr uint32_t IA32_X2APIC_TMR2 = 0x81A;                // Trigger Mode Register Bits 95:64 // 触发模式寄存器位95:64
        constexpr uint32_t IA32_X2APIC_TMR3 = 0x81B;                // Trigger Mode Register Bits 127:96 // 触发模式寄存器位127:96
        constexpr uint32_t IA32_X2APIC_TMR4 = 0x81C;                // Trigger Mode Register Bits 159:128 // 触发模式寄存器位159:128
        constexpr uint32_t IA32_X2APIC_TMR5 = 0x81D;                // Trigger Mode Register Bits 191:160 // 触发模式寄存器位191:160
        constexpr uint32_t IA32_X2APIC_TMR6 = 0x81E;                // Trigger Mode Register Bits 223:192 // 触发模式寄存器位223:192
        constexpr uint32_t IA32_X2APIC_TMR7 = 0x81F;                // Trigger Mode Register Bits 255:224 // 触发模式寄存器位255:224
        constexpr uint32_t IA32_X2APIC_IRR0 = 0x820;                // Interrupt Request Register Bits 31:0 // 中断请求寄存器位31:0
        constexpr uint32_t IA32_X2APIC_IRR1 = 0x821;                // Interrupt Request Register Bits 63:32 // 中断请求寄存器位63:32
        constexpr uint32_t IA32_X2APIC_IRR2 = 0x822;                // Interrupt Request Register Bits 95:64 // 中断请求寄存器位95:64
        constexpr uint32_t IA32_X2APIC_IRR3 = 0x823;                // Interrupt Request Register Bits 127:96 // 中断请求寄存器位127:96
        constexpr uint32_t IA32_X2APIC_IRR4 = 0x824;                // Interrupt Request Register Bits 159:128 // 中断请求寄存器位159:128
        constexpr uint32_t IA32_X2APIC_IRR5 = 0x825;                // Interrupt Request Register Bits 191:160 // 中断请求寄存器位191:160
        constexpr uint32_t IA32_X2APIC_IRR6 = 0x826;                // Interrupt Request Register Bits 223:192 // 中断请求寄存器位223:192
        constexpr uint32_t IA32_X2APIC_IRR7 = 0x827;                // Interrupt Request Register Bits 255:224 // 中断请求寄存器位255:224
        constexpr uint32_t IA32_X2APIC_ESR = 0x828;                 // Error Status Register // 错误状态寄存器
        constexpr uint32_t IA32_X2APIC_LVT_CMCI = 0x82F;            // LVT Corrected Machine Check Interrupt Register // LVT校正机器检查中断寄存器
        constexpr uint32_t IA32_X2APIC_ICR = 0x830;                 // Interrupt Command Register // 中断命令寄存器
        constexpr uint32_t IA32_X2APIC_LVT_TIMER = 0x832;           // LVT Timer Interrupt Register // LVT定时器中断寄存器
        constexpr uint32_t IA32_X2APIC_LVT_THERMAL = 0x833;         // LVT Thermal Sensor Interrupt Register // LVT热传感器中断寄存器
        constexpr uint32_t IA32_X2APIC_LVT_PMI = 0x834;             // LVT Performance Monitor Interrupt Register // LVT性能监控中断寄存器
        constexpr uint32_t IA32_X2APIC_LVT_LINT0 = 0x835;           // LVT LINT0 Interrupt Register // LVT LINT0中断寄存器
        constexpr uint32_t IA32_X2APIC_LVT_LINT1 = 0x836;           // LVT LINT1 Interrupt Register // LVT LINT1中断寄存器
        constexpr uint32_t IA32_X2APIC_LVT_ERROR = 0x837;           // LVT Error Interrupt Register // LVT错误中断寄存器
        constexpr uint32_t IA32_X2APIC_TIMER_INITIAL_COUNT = 0x838; // Initial Count Register (for Timer) // 定时器初始计数寄存器
        constexpr uint32_t IA32_X2APIC_TIMER_CURRENT_COUNT = 0x839; // Current Count Register (for Timer) // 定时器当前计数寄存器
        constexpr uint32_t IA32_X2APIC_TIMER_DIVIDE_CONFIG = 0x83E; // Divide Configuration Register (for Timer) // 定时器分频配置寄存器
        constexpr uint32_t IA32_X2APIC_SELF_IPI = 0x83F;            // Self IPI Register // 自IPI寄存器
    }
    
    // 系统相关信息寄存器
    namespace system {
        constexpr uint32_t IA32_FEATURE_CONTROL = 0x3A;     // Feature Control Register // 特性控制寄存器
        constexpr uint32_t IA32_TSC = 0x10;                 // Time-Stamp Counter // 时间戳计数器
        constexpr uint32_t IA32_PLATFORM_ID = 0x17;         // Platform ID // 平台ID
        constexpr uint32_t IA32_EBL_CR_POWERON = 0x2A;      // Processor Control and Status // 处理器控制和状态
        constexpr uint32_t IA32_EBC_FREQUENCY_ID = 0x2C;    // Processor Frequency Information // 处理器频率信息
    }
    
    // 调试相关寄存器
    namespace debug {
        constexpr uint32_t IA32_DEBUGCTL = 0x1D9;           // Debug Control Register // 调试控制寄存器
        constexpr uint32_t IA32_LASTBRANCHFROMIP = 0x1DB;   // Last Branch From IP // 最后分支来源IP
        constexpr uint32_t IA32_LASTBRANCHTOIP = 0x1DC;     // Last Branch To IP // 最后分支目标IP
        constexpr uint32_t IA32_LASTINTFROMIP = 0x1DD;      // Last Interrupt From IP // 最后中断来源IP
        constexpr uint32_t IA32_LASTINTTOIP = 0x1DE;        // Last Interrupt To IP // 最后中断目标IP
    }
    
    // 性能监控相关寄存器
    namespace performance {
        constexpr uint32_t IA32_MPERF = 0xE7;               // Max Performance Frequency Clock Count // 最大性能频率时钟计数
        constexpr uint32_t IA32_APERF = 0xE8;               // Actual Performance Frequency Clock Count // 实际性能频率时钟计数
        constexpr uint32_t IA32_PERF_STATUS = 0x198;        // Current Performance Status // 当前性能状态
        constexpr uint32_t IA32_PERF_CTL = 0x199;           // Performance Control // 性能控制
    }
    
    // 电源管理相关寄存器
    namespace power_management {
        constexpr uint32_t IA32_CLOCK_MODULATION = 0x19A;   // Clock Modulation Control // 时钟调制控制
        constexpr uint32_t IA32_THERM_INTERRUPT = 0x19B;    // Thermal Interrupt Control // 热中断控制
        constexpr uint32_t IA32_THERM_STATUS = 0x19C;       // Thermal Monitor Status // 热监控状态
        constexpr uint32_t IA32_MISC_ENABLE = 0x1A0;        // Enable Miscellaneous Processor Features // 启用杂项处理器特性
    }
    
    // 内存类型范围寄存器(MTRR)
    namespace mtrr {
        constexpr uint32_t IA32_MTRR_CAP = 0xFE;            // MTRR Capabilities // MTRR能力
        constexpr uint32_t IA32_MTRR_DEF_TYPE = 0x2FF;      // MTRR Default Type Register // MTRR默认类型寄存器
        
        // 物理MTRR寄存器
        constexpr uint32_t IA32_MTRR_PHYSBASE0 = 0x200;     // MTRR Base Register 0 // MTRR基址寄存器0
        constexpr uint32_t IA32_MTRR_PHYSMASK0 = 0x201;     // MTRR Mask Register 0 // MTRR掩码寄存器0
        constexpr uint32_t IA32_MTRR_PHYSBASE1 = 0x202;     // MTRR Base Register 1 // MTRR基址寄存器1
        constexpr uint32_t IA32_MTRR_PHYSMASK1 = 0x203;     // MTRR Mask Register 1 // MTRR掩码寄存器1
        constexpr uint32_t IA32_MTRR_PHYSBASE2 = 0x204;     // MTRR Base Register 2 // MTRR基址寄存器2
        constexpr uint32_t IA32_MTRR_PHYSMASK2 = 0x205;     // MTRR Mask Register 2 // MTRR掩码寄存器2
        constexpr uint32_t IA32_MTRR_PHYSBASE3 = 0x206;     // MTRR Base Register 3 // MTRR基址寄存器3
        constexpr uint32_t IA32_MTRR_PHYSMASK3 = 0x207;     // MTRR Mask Register 3 // MTRR掩码寄存器3
        constexpr uint32_t IA32_MTRR_PHYSBASE4 = 0x208;     // MTRR Base Register 4 // MTRR基址寄存器4
        constexpr uint32_t IA32_MTRR_PHYSMASK4 = 0x209;     // MTRR Mask Register 4 // MTRR掩码寄存器4
        constexpr uint32_t IA32_MTRR_PHYSBASE5 = 0x20A;     // MTRR Base Register 5 // MTRR基址寄存器5
        constexpr uint32_t IA32_MTRR_PHYSMASK5 = 0x20B;     // MTRR Mask Register 5 // MTRR掩码寄存器5
        constexpr uint32_t IA32_MTRR_PHYSBASE6 = 0x20C;     // MTRR Base Register 6 // MTRR基址寄存器6
        constexpr uint32_t IA32_MTRR_PHYSMASK6 = 0x20D;     // MTRR Mask Register 6 // MTRR掩码寄存器6
        constexpr uint32_t IA32_MTRR_PHYSBASE7 = 0x20E;     // MTRR Base Register 7 // MTRR基址寄存器7
        constexpr uint32_t IA32_MTRR_PHYSMASK7 = 0x20F;     // MTRR Mask Register 7 // MTRR掩码寄存器7
        constexpr uint32_t IA32_MTRR_PHYSBASE8 = 0x210;     // MTRR Base Register 8 // MTRR基址寄存器8
        constexpr uint32_t IA32_MTRR_PHYSMASK8 = 0x211;     // MTRR Mask Register 8 // MTRR掩码寄存器8
        constexpr uint32_t IA32_MTRR_PHYSBASE9 = 0x212;     // MTRR Base Register 9 // MTRR基址寄存器9
        constexpr uint32_t IA32_MTRR_PHYSMASK9 = 0x213;     // MTRR Mask Register 9 // MTRR掩码寄存器9
        
        // 固定范围MTRR寄存器
        constexpr uint32_t IA32_MTRR_FIX64K_00000 = 0x250;  // Fixed Range MTRR for 64K range at 00000H // 固定范围MTRR(64K, 起始于00000H)
        constexpr uint32_t IA32_MTRR_FIX16K_80000 = 0x258;  // Fixed Range MTRR for 16K range at 80000H // 固定范围MTRR(16K, 起始于80000H)
        constexpr uint32_t IA32_MTRR_FIX16K_A0000 = 0x259;  // Fixed Range MTRR for 16K range at A0000H // 固定范围MTRR(16K, 起始于A0000H)
        constexpr uint32_t IA32_MTRR_FIX4K_C0000 = 0x268;   // Fixed Range MTRR for 4K ranges at C0000H // 固定范围MTRR(4K, 起始于C0000H)
        constexpr uint32_t IA32_MTRR_FIX4K_C8000 = 0x269;   // Fixed Range MTRR for 4K ranges at C8000H // 固定范围MTRR(4K, 起始于C8000H)
        constexpr uint32_t IA32_MTRR_FIX4K_D0000 = 0x26A;   // Fixed Range MTRR for 4K ranges at D0000H // 固定范围MTRR(4K, 起始于D0000H)
        constexpr uint32_t IA32_MTRR_FIX4K_D8000 = 0x26B;   // Fixed Range MTRR for 4K ranges at D8000H // 固定范围MTRR(4K, 起始于D8000H)
        constexpr uint32_t IA32_MTRR_FIX4K_E0000 = 0x26C;   // Fixed Range MTRR for 4K ranges at E0000H // 固定范围MTRR(4K, 起始于E0000H)
        constexpr uint32_t IA32_MTRR_FIX4K_E8000 = 0x26D;   // Fixed Range MTRR for 4K ranges at E8000H // 固定范围MTRR(4K, 起始于E8000H)
        constexpr uint32_t IA32_MTRR_FIX4K_F0000 = 0x26E;   // Fixed Range MTRR for 4K ranges at F0000H // 固定范围MTRR(4K, 起始于F0000H)
        constexpr uint32_t IA32_MTRR_FIX4K_F8000 = 0x26F;   // Fixed Range MTRR for 4K ranges at F8000H // 固定范围MTRR(4K, 起始于F8000H)
        constexpr uint32_t IA32_PAT = 0x277;
    }
    
    // 虚拟化技术相关寄存器(VTX)
    namespace vmx {
        constexpr uint32_t IA32_VMX_BASIC = 0x480;          // VMX Basic Information // VMX基本信息
        constexpr uint32_t IA32_VMX_PINBASED_CTLS = 0x481;  // VMX Pin-Based Controls // VMX基于引脚的控制
        constexpr uint32_t IA32_VMX_PROCBASED_CTLS = 0x482; // VMX Primary Processor-Based Controls // VMX主处理器控制
        constexpr uint32_t IA32_VMX_EXIT_CTLS = 0x483;      // VMX Exit Controls // VMX退出控制
        constexpr uint32_t IA32_VMX_ENTRY_CTLS = 0x484;     // VMX Entry Controls // VMX进入控制
        constexpr uint32_t IA32_VMX_MISC = 0x485;           // VMX Miscellaneous Information // VMX杂项信息
        constexpr uint32_t IA32_VMX_CR0_FIXED0 = 0x486;     // VMX CR0 Fixed Bits 0 // VMX CR0固定位0
        constexpr uint32_t IA32_VMX_CR0_FIXED1 = 0x487;     // VMX CR0 Fixed Bits 1 // VMX CR0固定位1
        constexpr uint32_t IA32_VMX_CR4_FIXED0 = 0x488;     // VMX CR4 Fixed Bits 0 // VMX CR4固定位0
        constexpr uint32_t IA32_VMX_CR4_FIXED1 = 0x489;     // VMX CR4 Fixed Bits 1 // VMX CR4固定位1
        constexpr uint32_t IA32_VMX_VMCS_ENUM = 0x48A;      // VMX VMCS Enumeration // VMX VMCS枚举
        constexpr uint32_t IA32_VMX_PROCBASED_CTLS2 = 0x48B; // VMX Secondary Processor-Based Controls // VMX次处理器控制
        constexpr uint32_t IA32_VMX_EPT_VPID_CAP = 0x48C;   // VMX EPT/VPID Capabilities // VMX EPT/VPID能力
        constexpr uint32_t IA32_VMX_TRUE_PINBASED_CTLS = 0x48D;  // VMX True Pin-Based Controls // VMX真实基于引脚的控制
        constexpr uint32_t IA32_VMX_TRUE_PROCBASED_CTLS = 0x48E; // VMX True Primary Processor-Based Controls // VMX真实主处理器控制
        constexpr uint32_t IA32_VMX_TRUE_EXIT_CTLS = 0x48F;      // VMX True Exit Controls // VMX真实退出控制
        constexpr uint32_t IA32_VMX_TRUE_ENTRY_CTLS = 0x490;     // VMX True Entry Controls // VMX真实进入控制
        constexpr uint32_t IA32_VMX_VMFUNC = 0x491;         // VMX VM-Function Controls // VMX虚拟机功能控制
    }
    
    // SGX相关寄存器
    namespace sgx {
        constexpr uint32_t IA32_SGXLEPUBKEYHASH0 = 0x8C;    // SGX Launch Enclave Public Key Hash 0 // SGX启动飞地公钥哈希0
        constexpr uint32_t IA32_SGXLEPUBKEYHASH1 = 0x8D;    // SGX Launch Enclave Public Key Hash 1 // SGX启动飞地公钥哈希1
        constexpr uint32_t IA32_SGXLEPUBKEYHASH2 = 0x8E;    // SGX Launch Enclave Public Key Hash 2 // SGX启动飞地公钥哈希2
        constexpr uint32_t IA32_SGXLEPUBKEYHASH3 = 0x8F;    // SGX Launch Enclave Public Key Hash 3 // SGX启动飞地公钥哈希3
    }
    namespace timer{
        constexpr uint32_t IA32_TSC_DEADLINE = 0x6E0; 
    };
    // 系统调用相关寄存器
    namespace syscall {
        constexpr uint32_t IA32_SYSENTER_CS = 0x174;        // SYSENTER_CS_MSR // SYSENTER代码段MSR
        constexpr uint32_t IA32_SYSENTER_ESP = 0x175;       // SYSENTER_ESP_MSR // SYSENTER栈指针MSR
        constexpr uint32_t IA32_SYSENTER_EIP = 0x176;       // SYSENTER_EIP_MSR // SYSENTER指令指针MSR
        constexpr uint32_t IA32_STAR = 0xC0000081;          // Legacy Mode SYSCALL Target Address // 传统模式SYSCALL目标地址
        constexpr uint32_t IA32_LSTAR = 0xC0000082;         // Long Mode SYSCALL Target Address // 长模式SYSCALL目标地址
        constexpr uint32_t IA32_FMASK = 0xC0000084;         // SYSCALL Flag Mask // SYSCALL标志掩码
        constexpr uint32_t IA32_FS_BASE = 0xC0000100;       // Map of BASE Address of FS // FS段基地址映射
        constexpr uint32_t IA32_GS_BASE = 0xC0000101;       // Map of BASE Address of GS // GS段基地址映射
        constexpr uint32_t IA32_KERNEL_GS_BASE = 0xC0000102; // Swap Target of BASE Address of GS // GS基地址交换目标
        constexpr uint32_t IA32_TSC_AUX = 0xC0000103;       // Auxiliary TSC // 辅助时间戳计数器
    }
}