#include<stdint.h>
#include<efi.h>
#include "PortDriver.h"
#include "KernelEntryPointDefinetion.h"
#include "os_error_definitions.h"
#include "VideoDriver.h"
#include "kcirclebufflogMgr.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "processor_Ks_stacks_mgr.h"
#include "Interrupt.h"
#include "./memory/includes/Memory.h"
#include "./memory/includes/kpoolmemmgr.h"
#include "./memory/includes/phygpsmemmgr.h"
#include "processor_self_manage.h"
#include "UefiRunTimeServices.h"
#include "panic.h"
#include "gSTResloveAPIs.h"
#undef __stack_chk_fail
extern  void __wrap___stack_chk_fail(void);
// 定义C++运行时需要的符号
 extern "C" {
    // DSO句柄，对于静态链接的内核，可以简单定义为空
    void* __dso_handle = 0;
    
    // 用于注册析构函数的函数，这里提供一个空实现
    int __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
        (void)func;
        (void)arg;
        (void)dso;
        return 0;
    }
}
void delay(unsigned int milliseconds) {
    for (unsigned int i = 0; i < milliseconds * 10000; ++i) {
        // 空循环，占用 CPU
        asm volatile("nop"); // 防止编译器优化（可选）
    }
}
EFI_TIME global_time;
uint32_t efi_map_ver;
/*
注意，这个函数刚进入时还使用的是bootloader的栈
在特定几个函数初始化好后才能使用切换到映像的栈
*/
extern "C" void kernel_start( BootInfoHeader* transfer) 
{   

    int  Status=0;
    global_gST=transfer->gST_ptr;
    efi_map_ver=transfer->mapversion;
    GlobalBasicGraphicInfoType* TFG=(GlobalBasicGraphicInfoType*)&transfer->graphic_metainfo;
    Status = InitialGlobalBasicGraphicInfo(
        TFG->horizentalResolution,
        TFG->verticalResolution,
        TFG->pixelFormat,
        TFG->PixelsPerScanLine,
        TFG->FrameBufferBase,
        TFG->FrameBufferSize
    );
    serial_init_stage1();
    gkcirclebufflogMgr.Init();
    if (Status!=OS_SUCCESS)
    {
        serial_puts("InitialGlobalBasicGraphicInfo Failed\n");
        return ;
    }
    gKpoolmemmgr.Init();
      
      // [真实历史: 切栈已在 kernel_entry.asm 中完成, 此注释为 7b7f1c7 时代的旧痕迹]
    Status =InitialGlobalCharacterSetBitmapControler(
        32,
        16,
        0x00000000,
        0x00FFFFFF,
        (UINT8*)ter16x32_data[0][0],
        FALSE,
        NULL
    );
    Status=InitialKernelShellControler(
        GlobalBasicGraphicInfo.verticalResolution,
        GlobalBasicGraphicInfo.horizentalResolution,
        ASCII,
        0,0,0,0,
        GlobalKernelShellInputBuffer,
        GlobalKernelShellOutputBuffer,
        4096,
        &GlobalCharacterSetBitmapControler,
        4096,0,0
    );
    if (Status!=OS_SUCCESS)
    {
        serial_puts("InitialKernelShellControler Failed\n");
        return ;
    }
    gkernelPanicManager.Init(5);
    LocalCPU bsp_regieters;
    // 初始化全局Ascii位图控制器
    kputsSecure("Welcome to PlayOSKernelShell\n");
    gBaseMemMgr.Init(reinterpret_cast<EFI_MEMORY_DESCRIPTORX64*>(transfer->memory_map_ptr),transfer->memory_map_entry_count);
    gBaseMemMgr.printPhyMemDesTb();
    gKspacePgsMemMgr.Init();
    gRuntimeServices.Init(global_gST, efi_map_ver);
    gBaseMemMgr.DisableBasicMemService();
    gProcessor_Ks_stacks_mgr.Init();
    gInterrupt_mgr.Init();
    gAcpiVaddrSapceMgr.Init(global_gST);
    gRuntimeServices.rt_shutdown();
    asm volatile("hlt");
    //中断接管工作

}