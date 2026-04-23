#pragma once
#include <stdint.h>
// ========== 1. PCI通用头部 (0x00-0x3F) ==========
// 所有PCI/PCIe设备都有的前64字节


// ========== 2. Command寄存器位定义 ==========
typedef union {
    uint16_t value;
    struct {
        uint16_t io_space:1;           // bit0: I/O空间使能
        uint16_t memory_space:1;        // bit1: 内存空间使能
        uint16_t bus_master:1;          // bit2: 总线主控使能 (DMA)
        uint16_t special_cycles:1;       // bit3: 特殊周期
        uint16_t memory_write_inv:1;     // bit4: 内存写无效
        uint16_t vga_palette_snoop:1;    // bit5: VGA调色板监听
        uint16_t parity_error_response:1; // bit6: 奇偶错误响应
        uint16_t reserved1:1;            // bit7: 保留
        uint16_t serr_enable:1;          // bit8: SERR#使能
        uint16_t fast_back2back:1;       // bit9: 快速背靠背
        uint16_t interrupt_disable:1;     // bit10: 中断禁用
        uint16_t reserved2:5;            // bits11-15: 保留
    }fields;
} __attribute__((packed)) pci_command_t;

// ========== 3. Status寄存器位定义 ==========
typedef union {
    uint16_t value;
    struct {
        uint16_t immeadiate_readiness:1;
        uint16_t reserved1:2;            // bits0-2: 保留
        uint16_t interrupt_status:1;      // bit3: 中断状态
        uint16_t capabilities_list:1;     // bit4: 支持能力链表
        uint16_t mhz66_capable:1;         // bit5: 66MHz能力
        uint16_t reserved2:1;             // bit6: 保留
        uint16_t fast_back2back_capable:1; // bit7: 快速背靠背能力
        uint16_t master_data_parity_error:1; // bit8: 主数据奇偶错误
        uint16_t devsel_timing:2;         // bits9-10: DEVSEL时序
        uint16_t signaled_target_abort:1;  // bit11: 发送目标终止
        uint16_t received_target_abort:1;  // bit12: 收到目标终止
        uint16_t received_master_abort:1;  // bit13: 收到主终止
        uint16_t signaled_system_error:1;  // bit14: 发送系统错误
        uint16_t detected_parity_error:1;  // bit15: 检测到奇偶错误
    }fields;
} __attribute__((packed)) pci_status_t;

// ========== 4. Header Type 定义 ==========
constexpr uint8_t PCI_HEADER_TYPE_MASK = 0x7F;
constexpr uint8_t PCI_HEADER_TYPE_MULTIFUNC = 0x80;

constexpr uint8_t PCI_HEADER_TYPE_ENDPOINT = 0x00;    // Type 0 (普通设备)
constexpr uint8_t PCI_HEADER_TYPE_PCI_BRIDGE = 0x01;  // Type 1 (PCI-PCI 桥)
constexpr uint8_t PCI_HEADER_TYPE_CARDBUS = 0x02;     // Type 2 (CardBus 桥)
constexpr uint16_t ECAM_size=0x1000;
constexpr uint8_t func_bit_width=3;
constexpr uint8_t device_bit_num=5;
union bar_t{
    uint32_t value;
    struct bar_mem{
        uint32_t identifier:1;
        uint32_t mem_type:2;
        uint32_t mem_prefetchable:1;
        uint32_t base_address:28;
    }mem_field;
    struct bar_io{
        uint32_t identifier:1;
        uint32_t reserved:1;
        uint32_t size:30;
    }io_field;
};
typedef struct {
    // 0x00-0x03: 设备标识
    uint16_t vendor_id;           // 厂商ID (读)
    uint16_t device_id;           // 设备ID (读)
    
    // 0x04-0x07: 命令和状态
    uint16_t command;              // 命令寄存器 (读/写)
    uint16_t status;               // 状态寄存器 (读)
    
    // 0x08-0x0B: 版本和类别码
    uint8_t revision_id;           // 修订版本 (读)
    uint8_t prog_if;               // 编程接口 (读)
    uint8_t sub_class;             // 子类 (读)
    uint8_t base_class;            // 基类 (读)
    
    // 0x0C-0x0F: 头部信息
    uint8_t cache_line_size;       // 缓存行大小 (读/写)
    uint8_t latency_timer;         // 延迟定时器 (读/写)
    uint8_t header_type;           // 头部类型 (读)
    uint8_t bist;                   // 内建自测试 (读/写)
    
    uint32_t bars[6];
    
    // 0x28-0x2B: CardBus CIS指针 (很少用)
    uint32_t cardbus_cis_ptr;       // CardBus CIS指针 (读)
    
    // 0x2C-0x2F: 子系统标识
    uint16_t subsystem_vendor_id;   // 子系统厂商ID (读)
    uint16_t subsystem_id;          // 子系统ID (读)
    
    // 0x30-0x33: 扩展ROM
    uint32_t expansion_rom_base;    // 扩展ROM基地址 (读/写)
    
    // 0x34-0x3B: 能力指针和保留
    uint8_t capabilities_ptr;        // 能力链表指针 (读)
    uint8_t reserved1[7];            // 保留
    
    // 0x3C-0x3F: 中断和时序
    uint8_t interrupt_line;          // 中断线 (读/写)
    uint8_t interrupt_pin;           // 中断引脚 (读)
    uint8_t min_grant;               // 最小授权时间 (读)
    uint8_t max_latency;             // 最大延迟时间 (读)
} __attribute__((packed)) pci_header_endpoint_t;
typedef struct { 
    uint16_t vendor_id;           // 厂商ID (读)
    uint16_t device_id;           // 设备ID (读)
    uint16_t command;
    uint16_t status;
    uint32_t revision_id:8;
    uint8_t prog_if;
    uint8_t sub_class;
    uint8_t base_class;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bars[2];                // PCI桥通常只有2个BAR
    uint8_t primary_bus;             // 主总线号
    uint8_t secondary_bus;           // 次总线号
    uint8_t subordinate_bus;         // 从总线号
    uint8_t secondary_latency_timer; // 二级延迟定时器
     uint8_t io_base;                // I/O基地址 (高4位有效)
    uint8_t io_limit;               // I/O地址限制 (高4位有效)
    uint16_t secondary_status;       // 二级状态寄存器
    uint16_t memory_base;           // 内存基地址 (高12位有效)
    uint16_t memory_limit;          // 内存地址限制 (高12位有效)
    uint16_t prefetchable_memory_base; // 可预取内存基地址 (高12位有效)
    uint16_t prefetchable_memory_limit; // 可预取内u
    uint32_t prefetchable_memory_base_upper;
    uint32_t prefetchable_memory_limit_upper;
    uint16_t io_base_upper;         // I/O基地址高16位 (仅64位桥)
    uint16_t io_limit_upper;
    uint8_t capabilities_ptr;
    uint8_t reserved1[3];
    uint32_t expansion_rom_base;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint16_t bridge_control;
} __attribute__((packed)) pci_header_pci_bridge_t;
// 静态断言检查大小
static_assert(sizeof(pci_header_endpoint_t) == 64, "pci_header_t must be 64 bytes");
static_assert(sizeof(pci_header_pci_bridge_t) == 64, "pci_header_pci_bridge_t must be 64 bytes");

// BAR类型常量
constexpr uint8_t PCI_BAR_TYPE_32BIT = 0x0;
constexpr uint8_t PCI_BAR_TYPE_64BIT = 0x2;


// ========== 6. 能力链表通用头 ==========
typedef struct {
    uint8_t id;         // 能力ID
    uint8_t next;       // 下一个能力的偏移
} __attribute__((packed)) pci_cap_t;

// 能力 ID 定义
constexpr uint8_t PCI_CAP_ID_PM = 0x01;      // Power Management
constexpr uint8_t PCI_CAP_ID_MSI = 0x05;     // MSI
constexpr uint8_t PCI_CAP_ID_EXP = 0x10;     // PCI Express
constexpr uint8_t PCI_CAP_ID_MSIX = 0x11;    // MSI-X


// ========== 7. Power Management能力 ==========
typedef struct {
    uint8_t id;                     // 0x01
    uint8_t next;                   // 下一个能力偏移
    struct{
        uint16_t version:3;
        uint16_t pme_support:1;
        uint16_t Immediate_Readiness_on_Return_to_D0:1;
        uint16_t initialize_bit:1;
        uint16_t aux_current:3;
        uint16_t D1_support:1;
        uint16_t D2_support:1;
        uint16_t PME_Support:1;
    } cap;                    // PM能力寄存器
    struct{
        uint16_t power_state:2;
        uint16_t reserved0:1;
        uint16_t no_soft_reset:1;
        uint16_t reserved1:4;
        uint16_t PME_En:1;
        uint16_t Data_Select:4;
        uint16_t Data_Scale:2;
        uint16_t PME_Status:1;
        uint16_t reserved2:6;
        uint16_t undefined:2;
    } ctrl_status;            // PM控制/状态寄存器
    uint8_t data;                    // PM数据 (可选)
} __attribute__((packed)) pci_pm_cap_t;

// PM能力寄存器位
typedef union {
    uint16_t value;
    struct {
        uint16_t version:3;          // bits0-2: PM规范版本
        uint16_t pme_clock:1;         // bit3: PME时钟
        uint16_t reserved:1;          // bit4: 保留
        uint16_t device_specific_init:1; // bit5: 设备特定初始化
        uint16_t aux_current:3;       // bits6-8: 辅助电流
        uint16_t d1_support:1;        // bit9: 支持D1状态
        uint16_t d2_support:1;        // bit10: 支持D2状态
        uint16_t pme_support:5;       // bits11-15: PME支持
    };
} __attribute__((packed)) pmi_pm_cap_t;

typedef union {
    uint16_t value;
    struct {
        uint16_t msi_enable:1;
        uint16_t multi_message_capable:3;
        uint16_t multi_message_enable:3;
        uint16_t address_64_capable:1;
        uint16_t per_vector_mask_capable:1;
        uint16_t extended_message_data_capable:1;
        uint16_t extended_message_data_enable:1;
    };
} __attribute__((packed)) msi_ctl_t;
// ========== 8. MSI能力 ==========
typedef struct {
    uint8_t id;                     // 0x05
    uint8_t next;                   // 下一个能力偏移
    msi_ctl_t ctrl;                   // MSI控制寄存器
    uint32_t address;                // 消息地址 (32位)
    uint16_t data;                   // 消息数据
    uint16_t data_up16;           // 消息地址高32位 (64位模式)
    uint32_t mask;                   // 掩码位 (可选)
    uint32_t pending;                // 挂起位 (可选)
} __attribute__((packed)) pci_msi_32_cap_t;
typedef struct { 
    uint8_t id;                     // 0x05
    uint8_t next;                   // 下一个能力偏移
    msi_ctl_t ctrl;                   // MSI控制寄存器
    uint64_t address;                // 消息地址 (64位)
    uint16_t data;                   // 消息数据
    uint16_t data_up16;             
    uint32_t mask;                   // 掩码位 (可选)
    uint32_t pending;  
} __attribute__((packed)) pci_msi_64_cap_t;

// MSI控制寄存器位
typedef union {
    uint16_t value;
    struct {
        uint16_t enable:1;            // bit0: MSI使能
        uint16_t multi_msg_capable:3; // bits1-3: 多消息能力
        uint16_t multi_msg_enable:3;  // bits4-6: 多消息使能
        uint16_t capable_64bit:1;     // bit7: 64位地址能力
        uint16_t per_vector_mask:1;   // bit8: 每向量掩码能力
        uint16_t reserved:7;          // bits9-15: 保留
    };
} __attribute__((packed)) pci_msi_ctrl_t;


// ========== 9. MSI-X能力 ==========
typedef struct {
    uint8_t id;                     // 0x11
    uint8_t next;                   // 下一个能力偏移
    uint16_t ctrl;                   // MSI-X控制寄存器
    
    // Table和PBA信息 (一个dword编码BAR索引和偏移)
    uint32_t table_offset;           // Table offset/BIR
    uint32_t pba_offset;             // PBA offset/BIR
} __attribute__((packed)) pci_msix_cap_t;

// MSI-X控制寄存器位
typedef union {
    uint16_t value;
    struct {
        uint16_t table_size:11;      // bits0-10: Table大小 (N-1)
        uint16_t reserved:3;         // bits11-13: 保留
        uint16_t function_mask:1;    // bit14: 功能掩码
        uint16_t enable:1;           // bit15: MSI-X使能
    };
} __attribute__((packed)) pci_msix_ctrl_t;

// MSI-X Table entry (每个16字节)
typedef struct {
    uint32_t msg_addr_low;           // 消息地址低32位
    uint32_t msg_addr_high;          // 消息地址高32位
    uint32_t msg_data;               // 消息数据
    uint32_t vector_control;         // 向量控制
} __attribute__((packed)) pci_msix_table_entry_t;

// MSI-X向量控制位
typedef union {
    uint32_t value;
    struct {
        uint32_t mask:1;             // bit0: 掩码位
        uint32_t reserved:31;        // bits1-31: 保留
    };
} __attribute__((packed)) pci_msix_vector_ctrl_t;


// ========== 10. PCI Express能力 ==========
typedef struct {
    uint8_t id;                     // 0x10
    uint8_t next;                   // 下一个能力偏移
    struct{
        uint16_t cap_ver:4;
        uint16_t device_type:4;
        uint16_t slot_implemented:1;
        uint16_t inter_message_number:5;
        uint16_t reserved:2;
    }pcie_cap;                    // PCIe能力寄存器
    struct{
        uint16_t max_payload_size_supported:3;
        uint16_t phantom_function_support:2;
        uint16_t ext_tag_field_supported:1;
        uint16_t endpoint_l0s_acceptance_latency:3;
        uint16_t endpoint_l1_acceptance_latency:3;
        uint16_t reserved:3;
        uint16_t role_based_error_reporting:1;
        uint16_t err_cor_subclass_capable:1;
        uint16_t reserved2:1;
        uint16_t captured_slot_power_limit_value:8;
        uint16_t captured_slot_power_limit_scale:2;
        uint16_t function_level_reset_capable:1;
        uint16_t reserved3:3;
    }dev_cap;                // 设备能力
    uint16_t dev_ctrl;               // 设备控制
    uint16_t dev_sta;                // 设备状态
    uint32_t link_cap;               // 链路能力
    uint16_t link_ctrl;              // 链路控制
    uint16_t link_sta;               // 链路状态
    uint32_t slot_cap;               // 插槽能力
    uint16_t slot_ctrl;              // 插槽控制
    uint16_t slot_sta;               // 插槽状态
    uint16_t root_ctrl;              // 根控制
    uint16_t root_cap;               // 根能力
    uint32_t root_sta;               // 根状态
    uint32_t dev_cap2;               // 设备能力2
    uint16_t dev_ctrl2;              // 设备控制2
    uint16_t dev_sta2;               // 设备状态2
    uint32_t link_cap2;              // 链路能力2
    uint16_t link_ctrl2;             // 链路控制2
    uint16_t link_sta2;              // 链路状态2
    uint32_t slot_cap2;              // 插槽能力2
    uint16_t slot_ctrl2;             // 插槽控制2
    uint16_t slot_sta2;              // 插槽状态2
} __attribute__((packed)) pci_express_cap_t;