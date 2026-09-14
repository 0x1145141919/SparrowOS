#include "panic.h"
#include "init/util/printk.h"
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
    init_printk("================= CPU REGISTERS DUMP =================");
    init_printk("General Purpose Registers:");
    init_printk("RAX: 0x%lx", (unsigned long)regs->rax);
    init_printk("RBX: 0x%lx", (unsigned long)regs->rbx);
    init_printk("RCX: 0x%lx", (unsigned long)regs->rcx);
    init_printk("RDX: 0x%lx", (unsigned long)regs->rdx);
    init_printk("RSI: 0x%lx", (unsigned long)regs->rsi);
    init_printk("RDI: 0x%lx", (unsigned long)regs->rdi);
    init_printk("RSP: 0x%lx", (unsigned long)regs->rsp);
    init_printk("RBP: 0x%lx", (unsigned long)regs->rbp);
    init_printk("R8:  0x%lx", (unsigned long)regs->r8);
    init_printk("R9:  0x%lx", (unsigned long)regs->r9);
    init_printk("R10: 0x%lx", (unsigned long)regs->r10);
    init_printk("R11: 0x%lx", (unsigned long)regs->r11);
    init_printk("R12: 0x%lx", (unsigned long)regs->r12);
    init_printk("R13: 0x%lx", (unsigned long)regs->r13);
    init_printk("R14: 0x%lx", (unsigned long)regs->r14);
    init_printk("R15: 0x%lx", (unsigned long)regs->r15);

    init_printk("Control Registers:");
    init_printk("CR0: 0x%lx", (unsigned long)regs->cr0);
    init_printk("CR2: 0x%lx", (unsigned long)regs->cr2);
    init_printk("CR3: 0x%lx", (unsigned long)regs->cr3);
    init_printk("CR4: 0x%lx", (unsigned long)regs->cr4);
    init_printk("EFER: 0x%lx", (unsigned long)regs->IA32_EFER);

    init_printk("Segment Registers:");
    init_printk("CS: 0x%lx", (unsigned long)regs->cs);
    init_printk("DS: 0x%lx", (unsigned long)regs->ds);
    init_printk("ES: 0x%lx", (unsigned long)regs->es);
    init_printk("FS: 0x%lx", (unsigned long)regs->fs);
    init_printk("GS: 0x%lx", (unsigned long)regs->gs);
    init_printk("SS: 0x%lx", (unsigned long)regs->ss);

    init_printk("Other Registers:");
    init_printk("RFLAGS: 0x%lx", (unsigned long)regs->rflags);
    init_printk("RIP: 0x%lx", (unsigned long)regs->rip);
    init_printk("FS_BASE: 0x%lx", (unsigned long)regs->fs_base);
    init_printk("GS_BASE: 0x%lx", (unsigned long)regs->gs_base);

    init_printk("Descriptor Tables:");
    init_printk("GDTR: Limit=0x%lx, Base=0x%lx", (unsigned long)regs->gdtr.limit, (unsigned long)regs->gdtr.base);
    init_printk("IDTR: Limit=0x%lx, Base=0x%lx", (unsigned long)regs->idtr.limit, (unsigned long)regs->idtr.base);

    init_printk("=====================================================");
}
void Panic::panic(panic_behaviors_flags behaviors, char *message, panic_context::x64_context *context,panic_info_inshort*panic_info, uint64_t arg5)
{
    will.kernel_final_state=GlobalStatus;
    GlobalStatus=kernel_state::PANIC;
    init_printk("PANIC:");
    if (behaviors.interpret_arg5_as_err_locator) {
        init_printk("ERR_LOCATOR %lx", (unsigned long)arg5);
    } else {
        init_printk("KURD raw=0x%lx", (unsigned long)arg5);
    }
    if(message) init_printk("%s", message);
    if(context)dumpregisters(context);
    asm volatile("cli");
    asm volatile("hlt");
}