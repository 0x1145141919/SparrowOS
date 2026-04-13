#include "panic.h"
#include "firmware/UefiRunTimeServices.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "linker_symbols.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "util/arch/x86-64/cpuid_intel.h"
#ifdef USER_MODE
#include <unistd.h>
#endif 
kernel_state GlobalStatus;
panic_last_will will;
bool Panic::is_latest_panic_valid;


/**
 * 私有构造函数
 */
Panic::Panic(){
    // shutdownDelay已经作为静态成员初始化为5
}

Panic::~Panic()
{
}
//首先是其它CPU冻结
//其次是无条件切换CPU资源，使用BSP的EARLY_BOOT那一套
//第三步是will_write_will控制下写遗言
//第四步是allow_broadcast控制下对于非空message，context进行打印，kurd甩给kout分析
//最后停机
extern "C" void resources_shift();
void Panic::other_processors_froze_handler()
{
    asm volatile("cli");
    asm volatile("hlt");
}
KURD_t Panic::will_check()
{
    return KURD_t();
}
/**
 * 转储 panic_context 中的 CPU 寄存器信息
 */
void Panic::dumpregisters(panic_context::x64_context* regs) {
    bsp_kout<< "================= CPU REGISTERS DUMP =================" << kendl;
    bsp_kout<< "General Purpose Registers:" << kendl;
    bsp_kout.shift_hex();
    bsp_kout<< "RAX: 0x" << regs->rax << kendl;
    bsp_kout<< "RBX: 0x" << regs->rbx << kendl;
    bsp_kout<< "RCX: 0x" << regs->rcx << kendl;
    bsp_kout<< "RDX: 0x" << regs->rdx << kendl;
    bsp_kout<< "RSI: 0x" << regs->rsi << kendl;
    bsp_kout<< "RDI: 0x" << regs->rdi << kendl;
    bsp_kout<< "RSP: 0x" << regs->rsp << kendl;
    bsp_kout<< "RBP: 0x" << regs->rbp << kendl;
    bsp_kout<< "R8:  0x" << regs->r8 << kendl;
    bsp_kout<< "R9:  0x" << regs->r9 << kendl;
    bsp_kout<< "R10: 0x" << regs->r10 << kendl;
    bsp_kout<< "R11: 0x" << regs->r11 << kendl;
    bsp_kout<< "R12: 0x" << regs->r12 << kendl;
    bsp_kout<< "R13: 0x" << regs->r13 << kendl;
    bsp_kout<< "R14: 0x" << regs->r14 << kendl;
    bsp_kout<< "R15: 0x" << regs->r15 << kendl;
    
    bsp_kout<< "Control Registers:" << kendl;
    bsp_kout<< "CR0: 0x" << regs->cr0 << kendl;
    bsp_kout<< "CR2: 0x" << regs->cr2 << kendl;
    bsp_kout<< "CR3: 0x" << regs->cr3 << kendl;
    bsp_kout<< "CR4: 0x" << regs->cr4 << kendl;
    bsp_kout<< "EFER: 0x" << regs->IA32_EFER << kendl;
    
    bsp_kout<< "Segment Registers:" << kendl;
    bsp_kout<< "CS: 0x" << regs->cs << kendl;
    bsp_kout<< "DS: 0x" << regs->ds << kendl;
    bsp_kout<< "ES: 0x" << regs->es << kendl;
    bsp_kout<< "FS: 0x" << regs->fs << kendl;
    bsp_kout<< "GS: 0x" << regs->gs << kendl;
    bsp_kout<< "SS: 0x" << regs->ss << kendl;
    
    bsp_kout<< "Other Registers:" << kendl;
    bsp_kout<< "RFLAGS: 0x" << regs->rflags << kendl;
    bsp_kout<< "RIP: 0x" << regs->rip << kendl;
    bsp_kout<< "FS_BASE: 0x" << regs->fs_base << kendl;
    bsp_kout<< "GS_BASE: 0x" << regs->gs_base << kendl;
    
    bsp_kout<< "Descriptor Tables:" << kendl;
    bsp_kout<< "GDTR: Limit=0x" << regs->gdtr.limit << ", Base=0x" << regs->gdtr.base << kendl;
    bsp_kout<< "IDTR: Limit=0x" << regs->idtr.limit << ", Base=0x" << regs->idtr.base << kendl;
    
    bsp_kout<< "=====================================================" << kendl;
}
void Panic::panic(panic_behaviors_flags behaviors, char *message, panic_context::x64_context *context,panic_info_inshort*panic_info, KURD_t kurd)
{
    will.kernel_final_state=GlobalStatus;
    GlobalStatus=kernel_state::PANIC;
    bsp_kout<<"PANIC: "<<kendl;
    bsp_kout<<kurd<<kendl;
    if(message)bsp_kout<<message<<kendl;
    if(context)dumpregisters(context);
    asm volatile("cli");
    asm volatile("hlt");
}