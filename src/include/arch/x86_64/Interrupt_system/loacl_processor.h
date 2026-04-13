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
class x64_local_processor {//承担部分当前核心状态机切换维护的语义
    private:
    IDTEntry idt[256];
    x64GDT gdt;
    TSSentry tss;
    uint32_t apic_id;
    uint32_t processor_id;
    GS_struct gs_slot;
    FS_struct fs_slot;
    struct processor_features_t
    {
        bool is_sse_supported;
        bool is_avx_supported;
        bool is_x2apic_supported;
    }processor_features;
    spinlock_cpp_t  lock;
    Ktemplats::kernel_bitmap*handler_register_bitmap;
    constexpr static uint8_t allocatable_handler_count = 192; 
    constexpr static uint8_t allocatable_handler_base = 32; 
    KURD_t default_kurd();
    KURD_t default_success();
    KURD_t default_fail();
    KURD_t default_fatal();
    public:
    static constexpr uint8_t K_cs_idx = 0x1;
    static constexpr uint8_t K_ds_ss_idx = 0x2;
    static constexpr uint8_t U_cs_idx = 0x3;
    static constexpr uint8_t U_ds_ss_idx = 0x4;
    static constexpr uint32_t  RSP0_STACKSIZE= 0x8000;
    static constexpr uint32_t  DF_STACKSIZE= 0x2000;
    static constexpr uint32_t  MC_STACKSIZE= 0x3000;
    static constexpr uint32_t  NMI_STACKSIZE= 0x3000;
    static constexpr uint32_t  BP_DBG_STACKSIZE= 0x3000;
    static constexpr uint32_t  total_stack_size= BP_DBG_STACKSIZE+RSP0_STACKSIZE+DF_STACKSIZE+MC_STACKSIZE+NMI_STACKSIZE;
    static constexpr uint32_t  L_PROCESSOR_GS_IDX= 0;
    static int template_init();
    x64_local_processor(uint32_t alloced_id);
    void unsafe_handler_register_without_vecnum_check(uint8_t vector,void*handler);
    void unsafe_handler_unregister_without_vecnum_check(uint8_t vector);
    bool handler_register(uint8_t vector,void*handler);
    uint8_t handler_alloc(void*handler);
    bool handler_unregister(uint8_t vector);
    void GS_slot_write(uint32_t idx,uint64_t content);//0号是被占用了，静默失败，超过索引（GS_SLOT_MAX_ENTRY_COUNT）则静默失败其它情况正常写
    uint64_t GS_slot_get(uint32_t idx);//超过索引（GS_SLOT_MAX_ENTRY_COUNT）则返回~0其它情况正常读
    uint32_t get_apic_id();
    uint32_t get_processor_id();
    uint64_t get_tss_rsp0();
};
/**
 * 全局单例作为所有cpu的资源管理器，主要行为靠各个核心的Local_processor_Interrupt_mgr_t实现
 * 硬件层面只考虑x2apic，所以硬件层面对于处理器只考虑x2apicid编号
 * 但是系统逻辑层面使用processor_id对逻辑处理器编号
 */
typedef uint16_t prcessor_id_t;
class  x86_smp_processors_container { 
    private:
    static constexpr uint32_t max_processor_count=4096;
    static uint32_t total_processor_count;
    static uint32_t bsp_apic_id;
    static x64_local_processor *local_processor_interrupt_mgr_array[max_processor_count];
    static spinlock_interrupt_about_cpp_t lock;
    static KURD_t default_kurd();
    static KURD_t default_success();
    static KURD_t default_fail();
    static KURD_t default_fatal();
    public:
    static x64_local_processor*get_currunt_mgr();//使用内部gs的结构体对应的
    static x64_local_processor*get_processor_mgr_by_processor_id(prcessor_id_t id);
    static x64_local_processor*get_processor_mgr_by_apic_id(x2apicid_t apic_id);
    static void template_idt_init();
    /**
     * 初始化函数，必须由bsp调用，大体思路有：
     * 解析madt表,一个核心一个核心地初始化
     */
    static KURD_t AP_Init_one_by_one();
    static int regist_core(uint32_t processor_id );//程序指针进去会自动查询apicid,是否是bsp核心，分配processor_id并且在对应processor——id的引索的local_processor注册
    //bsp核心必然会分配一个为0的processor_id
    static int unregist_core();
};
extern uint64_t ap_hlt_word;
extern "C" uint64_t get_current_processor_rsp0();
