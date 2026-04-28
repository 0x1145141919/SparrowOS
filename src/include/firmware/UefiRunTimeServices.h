#include <efi.h>
#include <abi/boot.h>
class EFI_RT_SVS
{   
    static EFI_SYSTEM_TABLE *gST;
    static EFI_RESET_SYSTEM reset_system;
    static EFI_GET_TIME get_time;
    static EFI_SET_TIME set_time;
    static EFI_GET_WAKEUP_TIME get_wakeup_time;
    static EFI_SET_WAKEUP_TIME set_wakeup_time;
    static EFI_SET_VIRTUAL_ADDRESS_MAP set_virtual_address_map;
    static EFI_CONVERT_POINTER convert_pointer;
    static bool is_virtual;//标记这些是否加载到高位空间内
    public:
        EFI_RT_SVS();
        EFI_RT_SVS(EFI_SYSTEM_TABLE *sti, uint64_t mapver);
        static EFI_TIME rt_time_get();
        static EFI_STATUS rt_time_set(EFI_TIME &time);
        static EFI_STATUS rt_reset(
            EFI_RESET_TYPE reset_type,
            EFI_STATUS status,
            uint64_t data_size,
            void *data_ptr);
        static int Init(EFI_SYSTEM_TABLE *sti);
        static void rt_hotreset();
        static void rt_coldreset();
        static void rt_shutdown();
        /**
         * @brief 打印静态函数指针表（调试用）
         *
         * 输出所有 7 个 UEFI 运行时服务函数指针及其地址（HEX）。
         */
        static void dump_func_ptrs();
};
extern EFI_SYSTEM_TABLE*global_gST;

// kshell UEFI 运行时服务命令注册（实现在 src/firmware/uefi_kshell_commands.cpp）
#ifdef __cplusplus
extern void register_uefi_kshell_commands();
#endif