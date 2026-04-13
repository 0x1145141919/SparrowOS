#include "panic.h"
#include "firmware/UefiRunTimeServices.h"
#include "util/kout.h"
#include "util/kptrace.h"
#include "util/OS_utils.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "linker_symbols.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "memory/init_memory_info.h"
#ifdef USER_MODE
#include <unistd.h>
#endif 
kernel_state GlobalKernelStatus;
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
#ifdef KERNEL_MODE
void Panic::panic(panic_behaviors_flags behaviors, char *message, panic_context::x64_context *context,panic_info_inshort*panic_info, KURD_t kurd)
{
    if(GlobalKernelStatus>=kernel_state::SCHEDUL_READY){
        x2apic::x2apic_driver::broadcast_exself_fixed_ipi(other_processors_froze_handler);
    }
    will.kernel_final_state=GlobalKernelStatus;
    GlobalKernelStatus=kernel_state::PANIC;
    resources_shift();
    will.magic=panic_will_magic;
    if(panic_info)will.latest_panic_info=*panic_info;
    will.kurd=kurd;
    will.magic=panic_will_magic;
    will.version=panic_will_version;
    will.size=sizeof(panic_last_will);
    will.panic_seq++;
    will.Whistleblower.Whistleblower_id=query_x2apicid();
    will.Whistleblower.arch_specify=x86_64_arch_spcify;
    will.Whistleblower.end_timestamp=rdtsc();
    //if(behaviors.will_write_will)write_will();
    bsp_kout<<"PANIC: "<<now<<kendl;
    bsp_kout<<kurd<<kendl;
    if(message)bsp_kout<<message<<kendl;
    if(context){dumpregisters(context);
    symbol_entry *first_entry=ksymmanager::get_entry_near_addr(context->rip);
    if(first_entry){
        bsp_kout<<"Kernel panic at "<<first_entry->name<<" + "<<context->rip-first_entry->address<<kendl;
    }
    else_trace(context->rbp);
    }
    asm volatile("cli");
    asm volatile("hlt");
}
// 注意：global_gST已经在UefiRunTimeServices.cpp中定义，此处不再重复定义
// EFI_SYSTEM_TABLE* global_gST = nullptr;

void Panic::write_will()
{
    // 遍历所有 freeSystemRam 类型的物理内存段
    for(uint64_t seg_idx = 0; seg_idx < phymem_segments_count; seg_idx++) {
        phymem_segment& seg = phymem_segments[seg_idx];
        
        // 只处理 freeSystemRam 类型的内存段
        if(seg.type != freeSystemRam) {
            continue;
        }
        
        // 确保起始地址对齐到 4KB
        phyaddr_t seg_start = (seg.start + 0xFFF) & (~0xFFF);
        phyaddr_t seg_end = seg.start + seg.size;
        
        // 收集该段中被 VM_intervals 占用的区间
        struct occupied_range_t {
            phyaddr_t start;
            phyaddr_t end;
        };
        
        constexpr uint64_t MAX_OCCUPIED_RANGES = 256;
        occupied_range_t occupied_ranges[MAX_OCCUPIED_RANGES];
        uint64_t occupied_count = 0;
        
        // 查找与该物理段有交集的 VM_intervals
        for(uint64_t vm_idx = 0; vm_idx < VM_intervals_count; vm_idx++) {
            loaded_VM_interval& vm = VM_intervals[vm_idx];
            
            // 检查是否有交集
            if(vm.pbase >= seg_end || (vm.pbase + vm.size) <= seg_start) {
                continue;
            }
            
            // 计算交集范围
            phyaddr_t intersect_start = (vm.pbase > seg_start) ? vm.pbase : seg_start;
            phyaddr_t intersect_end = (vm.pbase + vm.size < seg_end) ? (vm.pbase + vm.size) : seg_end;
            
            // 保存占用的区间（相对于段的起始位置）
            if(occupied_count < MAX_OCCUPIED_RANGES) {
                occupied_ranges[occupied_count].start = intersect_start;
                occupied_ranges[occupied_count].end = intersect_end;
                occupied_count++;
            }
        }
        
        // 对占用区间按起始地址排序（简单冒泡排序）
        for(uint64_t i = 0; i < occupied_count - 1; i++) {
            for(uint64_t j = 0; j < occupied_count - i - 1; j++) {
                if(occupied_ranges[j].start > occupied_ranges[j+1].start) {
                    occupied_range_t temp = occupied_ranges[j];
                    occupied_ranges[j] = occupied_ranges[j+1];
                    occupied_ranges[j+1] = temp;
                }
            }
        }
        
        // 在空闲区域写入遗言
        phyaddr_t current_addr = seg_start;
        uint64_t next_occupied_index = 0;
        
        while(current_addr + sizeof(panic_last_will) <= seg_end) {
            // 检查是否到达下一个占用区间
            if(next_occupied_index < occupied_count) {
                phyaddr_t occupied_start = occupied_ranges[next_occupied_index].start;
                
                // 如果当前地址已经过了这个占用区间，跳过它
                if(current_addr >= occupied_ranges[next_occupied_index].end) {
                    next_occupied_index++;
                    continue;
                }
                
                // 如果下一个占用区间在当前地址之前，跳过它
                if(occupied_start <= current_addr) {
                    next_occupied_index++;
                    continue;
                }
                
                // 如果下一个占用区间就在当前地址，跳过整个占用区间
                if(occupied_start == current_addr) {
                    current_addr = occupied_ranges[next_occupied_index].end;
                    next_occupied_index++;
                    continue;
                }
                
                // 如果当前地址到占用区间之间有足够的空间，写入遗言
                if(occupied_start >= current_addr + sizeof(panic_last_will)) {
                    panic_last_will* will_ptr = (panic_last_will*)current_addr;
                    ksystemramcpy(&will, will_ptr, sizeof(panic_last_will));
                    current_addr += 0x1000;
                    continue;
                }
                
                // 空间不足，跳到占用区间之后
                current_addr = occupied_ranges[next_occupied_index].end;
                next_occupied_index++;
                continue;
            }
            
            // 没有更多占用区间，写入遗言
            panic_last_will* will_ptr = (panic_last_will*)current_addr;
            ksystemramcpy(&will, will_ptr, sizeof(panic_last_will));
            current_addr += 0x1000;
        }
    }
}
#endif
#ifdef USER_MODE
void Panic::panic(panic_behaviors_flags behaviors, char *message, panic_context::x64_context *context,panic_info_inshort*panic_info, KURD_t kurd)
{
    bsp_kout<<now<<"USERMODE EMULATION PANIC: "<<kendl;
    bsp_kout<<kurd<<kendl;
    if(message)bsp_kout<<message<<kendl;
    _exit(-1);
}
#endif
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
