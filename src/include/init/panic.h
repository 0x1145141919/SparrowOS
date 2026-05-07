#pragma once
#include "arch/x86_64/abi/pt_regs.h"
#include "abi/os_error_definitions.h"
#include <cstdarg>
enum kernel_state:uint8_t{
    ENTER=0,
    EARLY_BOOT=1,
    PANIC_WILL_ANALYZE=2,
    MM_READY=3,
    SCHEDUL_READY=4,
    PANIC=0xFF
};
struct panic_info_inshort{
    uint32_t is_bug:1;              // 实现错误
    uint32_t is_policy:1;           // 策略性停机
    uint32_t is_hw_fault:1;          // 硬件故障
    uint32_t is_mem_corruption:1;    // 内存损坏嫌疑
    uint32_t is_escalated:1;         // panic中的panic
    };
extern kernel_state GlobalStatus;
constexpr uint64_t panic_will_magic=0x5A5A5A5A5A5A5A5A;
constexpr uint32_t panic_will_version=0x1;
constexpr uint64_t x86_64_arch_spcify=0x1;
struct panic_last_will {
    uint64_t magic;          // 固定魔数
    uint32_t version;        // 结构版本
    uint32_t size;           // 结构大小
    uint64_t panic_seq;      // 第几次 panic（同一次启动内）
    panic_info_inshort latest_panic_info;
    struct Whistleblower_processor{
        uint32_t Whistleblower_id;//x86_64下是x2apicid
        uint32_t arch_specify;//暂时只考虑x86_64
        uint64_t end_timestamp;//x86_64下是rdtsc
    }Whistleblower;    // 触发 panic 的 CPU

    KURD_t kurd;           // 核心：KURD
    uint64_t kernel_final_state;    // 最后状态
    uint64_t extra[4];       // 自由扩展（寄存器摘要、地址hash等）
};
struct panic_behaviors_flags{
        uint32_t will_write_will:1;//控制是否写内存遗言
        uint32_t allow_broadcast:1;//控制kout后端要不要给后端那些输出设备的行为
    }; 
constexpr panic_behaviors_flags default_panic_behaviors_flags={
    .will_write_will=1,
    .allow_broadcast=1
};
extern panic_last_will will;
/**
 * 内核恐慌管理器
 * 单例模式实现，用于处理内核严重错误
 */
/**
 * TODO:需要配套的bsp_earlyboot_struct来给内核的写遗言过程兜底
 */
class Panic {
private:

    static bool is_latest_panic_valid;
    static void write_will();
    static void other_processors_froze_handler();
public:
    Panic();
    ~Panic();

    /**
    * 行为控制位
    */
    static void panic(
        panic_behaviors_flags behaviors,
        char*message,
        panic_context::x64_context*context,
        panic_info_inshort*panic_info,
        KURD_t kurd
    );
    /**
     * 打印指定的 x86_64 CPU 上下文寄存器内容。
     * @param regs 指向 panic_context::x64_context 结构的指针，包含需要打印的寄存器状态。
     */
    static KURD_t will_check();
    static void dumpregisters(panic_context::x64_context* regs);
};
