#pragma once
#include <stdint.h>
#define OS_SUCCESS 0
#define OS_ERROR_BASE 0x1000
#define OS_OUT_OF_MEMORY (OS_ERROR_BASE + 1)
#define OS_OUT_OF_RESOURCE (OS_ERROR_BASE + 2)
#define OS_INVALID_PARAMETER (OS_ERROR_BASE + 3)
#define OS_INVALID_ADDRESS (OS_ERROR_BASE + 4)
#define OS_INVALID_HANDLE (OS_ERROR_BASE + 5)
#define OS_INVALID_OPERATION (OS_ERROR_BASE + 6)
#define OS_INVALID_FILE_NAME (OS_ERROR_BASE + 7)
#define OS_INVALID_FILE_TYPE (OS_ERROR_BASE + 8)
#define OS_INVALID_FILE_SIZE (OS_ERROR_BASE + 9)
#define OS_INVALID_FILE_MODE (OS_ERROR_BASE + 10)
#define OS_INVALID_FILE_ACCESS (OS_ERROR_BASE + 11)
#define OS_INVALID_FILE_POSITION (OS_ERROR_BASE + 12)
#define OS_INVALID_FILE_ATTRIBUTE (OS_ERROR_BASE + 13)
#define OS_INVALID_FILE_TIME (OS_ERROR_BASE + 14)
#define OS_INVALID_FILE_PATH (OS_ERROR_BASE + 15)
#define OS_OUT_OF_RANGE (OS_ERROR_BASE + 16)
#define OS_NOT_EXIST (OS_ERROR_BASE + 17)
#define OS_BAD_FUNCTION (OS_ERROR_BASE + 18)
#define OS_MEMRY_ALLOCATE_FALT (OS_ERROR_BASE + 19)
#define OS_TARGET_BUSY (OS_ERROR_BASE + 20)
#define OS_NOT_SUPPORT (OS_ERROR_BASE + 21)
#define OS_FILE_SYSTEM_DAMAGED (OS_ERROR_BASE + 22)
#define OS_FILE_NOT_FOUND (OS_ERROR_BASE + 23)
#define OS_INSUFFICIENT_STORAGE_SPACE (OS_ERROR_BASE + 25)
#define OS_PERMISSON_DENIED (OS_ERROR_BASE + 26)
#define OS_UNREACHABLE_CODE (OS_ERROR_BASE + 27)
#define OS_TRY_LOCK_FAIL (OS_ERROR_BASE + 28)
#define OS_INVALID_LOCK_MODE (OS_ERROR_BASE + 29)
#define OS_HEAP_OBJ_EXCEPTION (OS_ERROR_BASE + 30)//堆对象异常
#define OS_HEAP_OBJ_DESTROYED (OS_ERROR_BASE + 31)//堆对象已被损毁
#define OS_INHEAP_NOT_ENOUGH_MEMORY (OS_ERROR_BASE + 32)//一个堆内内存不足
#define OS_FAIL_PAGE_ALLOC (OS_ERROR_BASE + 33)
#define OS_MMIO_REGIST_FAIL (OS_ERROR_BASE + 34)
#define OS_PAGE_STATE_ERROR (OS_ERROR_BASE + 35)
#define OS_PAGE_REFCOUNT_NONZERO (OS_ERROR_BASE + 36)
#define OS_PAGE_MAPCOUNT_NONZERO (OS_ERROR_BASE + 37)
#define OS_NOT_SUPPORTED (OS_ERROR_BASE + 39)
#define OS_RESOURCE_CONFILICT (OS_ERROR_BASE + 40)
#define OS_RERELEASE_ERROR   (OS_ERROR_BASE + 42)
#define OS_PGTB_FREE_VALIDATION_FAIL (OS_ERROR_BASE + 43)
#define OS_SIZE_TO_LARGE (OS_ERROR_BASE + 44)
#define OS_MEMORY_FREE_FAULT (OS_ERROR_BASE + 45)
#define OS_ACPI_NOT_FOUNED (OS_ERROR_BASE + 46)
#define OS_EARLY_RETURN 100 //返回这个不一定是错误，具体看函数定义
struct KURD_t {//设计意图详见文档
    uint16_t result:4;//[0:3]是result[4:15]是result
    uint16_t reason:12;
    uint8_t in_module_location;
    uint8_t module_code;
    uint16_t free_to_use;
    uint8_t event_code;
    uint8_t level:3;//[0:2]是level[3:7]是domain
    uint8_t domain:5;
    
    // 通用构造函数
    constexpr KURD_t(uint16_t res, uint16_t reas, uint8_t mod_loc, uint8_t mod_code, 
                     uint16_t f1, uint8_t evt_code, uint8_t lvl, uint8_t dom)
        : result(res), reason(reas), in_module_location(mod_loc), module_code(mod_code),
          free_to_use(f1), event_code(evt_code), level(lvl), domain(dom) {}
    
    // 简化版构造函数，使用命名空间中的常用值
    constexpr KURD_t(uint16_t res, uint16_t reas, uint8_t mod_code, uint8_t mod_loc,uint8_t evt_code, uint8_t lvl, uint8_t dom)
        : result(res), reason(reas), in_module_location(mod_loc), module_code(mod_code),
          free_to_use(0), event_code(evt_code), level(lvl), domain(dom) {}
    
    // 默认构造函数，初始化为0
    constexpr KURD_t() : result(0), reason(0), in_module_location(0), module_code(0),
                         free_to_use(0), event_code(0), level(0), domain(0) {}
};
typedef uint64_t ckurd;
constexpr KURD_t empty_kurd = KURD_t();
KURD_t set_result_fail_and_error_level(KURD_t pre);
KURD_t set_fatal_result_level(KURD_t pre);
ckurd kurd_get_raw(KURD_t kurd);
KURD_t raw_analyze(ckurd raw);
union result_t{
    KURD_t kernel_result;
    uint64_t raw;
};
static_assert(sizeof(KURD_t) == 8,"KURD_t must be 8 bytes");
namespace level_code {
    constexpr uint8_t INVALID = 0;
    constexpr uint8_t INFO = 1;
    constexpr uint8_t NOTICE = 2;
    constexpr uint8_t WARNING = 3;
    constexpr uint8_t ERROR = 4;
    constexpr uint8_t FATAL = 5;
}
namespace module_code {
    constexpr uint8_t INVALID = 0;
    constexpr uint8_t MEMORY = 1;//内存模块
    constexpr uint8_t SCHEDULER = 2;
    constexpr uint8_t INTERRUPT = 3;
    constexpr uint8_t FIRMWARE = 4;
    constexpr uint8_t VFS = 5;
    constexpr uint8_t VMM = 6;
    constexpr uint8_t INFRA = 7;  //基础设施，比如内核特供strlen,strcmp这种
    constexpr uint8_t DEVICE = 8;//允许生死/插拔的硬件
    constexpr uint8_t DEVICES_CORE = 9;//系统设计上启动时必须找到/加载的硬件，不允许也不会设计卸载，panic路径中仍可使用的硬件，现行包括lapic,ioapic,重映射硬件pcie根复合体以及根端口，hpet,uart,i8042.
    constexpr uint8_t HARDWARE_DEBUG = 10;
    constexpr uint8_t USER_KERNEL_ABI = 11;
    constexpr uint8_t TIME = 12;
    constexpr uint8_t PANIC = 13;
}
namespace result_code {
    constexpr uint16_t SUCCESS = 0;
    constexpr uint16_t SUCCESS_BUT_SIDE_EFFECT = 1;
    constexpr uint16_t PARTIAL_SUCCESS = 2;
    constexpr uint16_t FAIL = 8;
    constexpr uint16_t RETRY = 9;
    constexpr uint16_t FATAL = 0xF;
}
namespace err_domain {
    constexpr uint8_t INVALID = 0;
    constexpr uint8_t CORE_MODULE = 1;
    constexpr uint8_t ARCH = 2;
    constexpr uint8_t USER = 3;
    constexpr uint8_t HYPERVISOR = 4;
    constexpr uint8_t OUT_MODULES = 8;
    constexpr uint8_t FILE_SYSTEM = 9;
    constexpr uint8_t HARDWARE = 10;
};
bool error_kurd(KURD_t kurd);
bool success_all_kurd(KURD_t kurd);