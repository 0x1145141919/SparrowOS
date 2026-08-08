#include <abi/boot.h>
#include <abi/os_error_definitions.h>
#include <boot/info_pkg_link.h>
#include <boot/asset_table.h>
#include <boot/exec_env_prepare.h>
#include <memory/kpoolmemmgr.h>
#include <exec_env_detect.h>
#include <panic.h>
extern "C"
{
void basic_init();
    /**
     * truly_start，mem_init 之后的复杂业务初始化：ACPI/APIC 分析、调度器数组、
     * AP 启动、task_pool、中断接管，随后进入调度。
     */
int truly_start();
}