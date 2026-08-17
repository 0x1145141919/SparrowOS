#include "arch/x86_64/mem_init.h"
#include "util/kout.h"
#include "panic.h"
#include "arch/x86_64/core_hardwares/tsc.h"
void init_halt(uint64_t will,bool is_kurd){
    bsp_kout<<"oops there is an fatal hanppen when initialzing in basic_init"<<kendl;
    if(is_kurd){
        bsp_kout<<"Will is kurd :";
    }else{
        bsp_kout<<"Will is a locator :";
    }
    bsp_kout<<HEX<<will<<kendl;
    asm volatile("hlt");
};
extern "C" void basic_init(){
    bsp_kout<<"Welcome to basic init stage\n";
    tsc_regist();
    GlobalKernelStatus=kernel_state::PANIC_WILL_ANALYZE;
    Panic::will_check();
    KURD_t    bsp_init_kurd=mem_init();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"mem_init Failed"<<kendl;
        init_halt(kurd_get_raw(bsp_init_kurd),true);
    }
}