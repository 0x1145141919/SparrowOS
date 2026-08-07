#include <abi/boot.h>
#include <abi/os_error_definitions.h>
extern "C"
{
    /**
     * exec_env_prepare，函数人如其名，是环境准备。首先是g_env = probe_env();
     * 其次再初始化kpoolmemmgr_t::Init();以第一堆初始化，而后再对init_to_kernel_header_v2的信息包进行链接
     * 把phymem_segments，bcb_table给暂时复制到堆里面，properties_table，暂时把"log_buffer mem" ，"phyaddr_window mem"
     */
void exec_env_prepare(init_to_kernel_header_v2*pkg);
void basic_init();
void main();
}