#include "arch/x86_64/mem_init.h"
#include "util/init_printk.h"   // 启动期日志（bsp_kout 接替者）
#include "panic.h"
#include "arch/x86_64/core_hardwares/tsc.h"
void init_halt(uint64_t will,bool is_kurd){
    init_printk("oops there is an fatal hanppen when initialzing in basic_init");
    if(is_kurd){
        init_printk("Will is kurd :");
    }else{
        init_printk("Will is a locator :");
    }
    init_printk("0x%lx", (unsigned long)will);
    asm volatile("hlt");
};
extern "C" void basic_init(){
    init_printk("Welcome to basic init stage");
    tsc_regist();
    GlobalKernelStatus=kernel_state::PANIC_WILL_ANALYZE;
    Panic::will_check();
    KURD_t    bsp_init_kurd=mem_init();
    if(error_kurd(bsp_init_kurd)){
        init_printk("mem_init Failed");
        init_halt(kurd_get_raw(bsp_init_kurd),true);
    }
}