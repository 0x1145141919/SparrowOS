#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "abi/os_error_definitions.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "arch/x86_64/mem_init.h"
#include "kcirclebufflogMgr.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "memory/memory_base.h"
#include "memory/kpoolmemmgr.h"
#include "memory/all_pages_arr.h"
#include "memory/FreePagesAllocator.h"
#include "memory/init_memory_info.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "memory/AddresSpace.h"
#include "memory/phyaddr_accessor.h"
#include "util/OS_utils.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "ktime.h"
#include "util/kout.h"
#include "util/textConsole.h"
#include "firmware/UefiRunTimeServices.h"
#include "panic.h"
#include "firmware/gSTResloveAPIs.h"
#include "util/kptrace.h"
#include "util/kshell.h"
#include "firmware/ACPI_APIC.h"
#include "arch/x86_64/Interrupt_system/AP_Init_error_observing_protocol.h"
#include "Scheduler/per_processor_scheduler.h"
#include "arch/x86_64/core_hardwares/DMAR.h"
#include "arch/x86_64/core_hardwares/ioapic.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "arch/x86_64/PCIe/prased.h"
#include "arch/x86_64/boot.h"
#include "KImage_Introspection.h"
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
extern "C" void delay(unsigned int milliseconds) {
    for (unsigned int i = 0; i < milliseconds * 10000; ++i) {
        // 空循环，占用 CPU
        asm volatile("nop"); // 防止编译器优化（可选）
    }
}
EFI_TIME global_time;
uint32_t efi_map_ver;
extern hard_interrupt_func_t*all_processors_interrupt_functions;
void ipi_test(){
    uint32_t self_processor_id=fast_get_processor_id();
    bsp_kout<<"processor id "<< self_processor_id<<kendl;
    KURD_t kurd=KURD_t();
    x2apic::x2apic_driver::write_eoi();
    ktime::heart_beat_alarm::set_clock_by_offset(20000);
    global_schedulers[self_processor_id].sched();
    asm volatile("hlt");
}

void* Collatz_kthread(void* init_value){
    uint64_t value = (uint64_t)init_value;
    uint64_t loop_count = 0;
    while (true) {
        if (value & 1) {
            value = value * 3 + 1;
        } else {
            value >>= 1;
        }
        loop_count++;
        if (value == 1) {
            return (void*)fast_get_processor_id();
        }
    }
}


constexpr uint8_t test_kthread_count = 100;
uint64_t test_kthreads[test_kthread_count];
void* burnin_thread(void* arg);
void* i8042_char_listener_thread(void* arg);
void*kthread_ymir(void*null){//所有内核线程的始祖之"尤米尔线程"（出自进击的巨人）
    (void)null;
    KURD_t kurd = KURD_t();
    
    i8042_char_subscriber_init();
    pcie_text_praser();
    //text_input_subscriber_init();
    // 初始化 kshell 框架
    kurd=kshell_framework_t::Init();
    if (error_kurd(kurd)) {
        bsp_kout << "[KSHELL] Failed to initialize framework!" << kendl;
    } else {
        bsp_kout << "[KSHELL] Framework initialized, ready for commands" << kendl;
    }
    
    
    while (true)
    {
        kthread_sleep(1000000);
    }
    
    return nullptr;
}




void create_first_kthread(){
    textconsole_GoP::RuntimeInitServiceThread();
    serial_init_stage2();
    GlobalKernelStatus=SCHEDUL_READY;
    x2apic::x2apic_driver::broadcast_exself_fixed_ipi(ipi_test);
    KURD_t kurd=KURD_t();
    uint64_t kthread_ymir_tid=create_kthread(kthread_ymir,nullptr,&kurd);
    per_processor_scheduler&sc=global_schedulers[0];
    ktime::heart_beat_alarm::set_clock_by_offset(20000);
    sc.sched();
}

extern "C" uint32_t assigned_cr3;
loaded_VM_interval* VM_intervals;
GlobalBasicGraphicInfoType gop_info;
XSDT_Table *XSDT;
void very_early_init(init_to_kernel_header* transfer){
    GlobalKernelStatus=kernel_state::EARLY_BOOT;
    self_introspection_init(transfer->kIMG_self_window,transfer->kBSS_interval);
    kpoolmemmgr_t::Init();
    kIMG_self_window=transfer->kIMG_self_window;
    kBSS_interval=transfer->kBSS_interval;
    pages_arr=transfer->pages_arr;
    FPA_bitmaps=transfer->FPA_bitmaps;
    log_buffer=transfer->log_buffer;
    kernel_entry_stack=transfer->kernel_entry_stack;
    symtable_file=transfer->symtable_file;
    initramfs_file=transfer->initramfs_file;
    identity_map_window=transfer->identity_map_window;
    x86_specify_init_to_kernel_info* arch=(x86_specify_init_to_kernel_info*)(uint64_t(transfer)+transfer->arch_specify_offset);
    hpet_mmio=arch->hpet_mmio;
    gop_info=arch->gop_info;
    gop_buffer.vbase=arch->Gop_vbase;
    gop_buffer.pbase=arch->gop_info.FrameBufferBase;
    gop_buffer.size=arch->gop_info.FrameBufferSize;
    gop_buffer.access={1,1,1,0,1,WC};
    logical_processor_count=transfer->logical_processor_count;
    VM_intervals=new loaded_VM_interval[transfer->loaded_VM_interval_count];
    ksystemramcpy((void*)(uint64_t(transfer)+transfer->loaded_VM_intervals_offset),VM_intervals,transfer->loaded_VM_interval_count*sizeof(loaded_VM_interval));
    phymem_segments=new phymem_segment[transfer->phymem_segment_count];
    ksystemramcpy((void*)(uint64_t(transfer)+transfer->memory_map_offset),phymem_segments,transfer->phymem_segment_count*sizeof(phymem_segment));
    VM_intervals_count=transfer->loaded_VM_interval_count;
    phymem_segments_count=transfer->phymem_segment_count;
}
extern "C" void kernel_start(init_to_kernel_header* transfer) 
{   
    very_early_init(transfer);
    transfer=nullptr;//此信息包是属于阅后即焚
    int  Status=0;
    KURD_t bsp_init_kurd=KURD_t();
    bsp_init_kurd=GfxPrim::Init(&gop_info,gop_buffer);//要开发直接写图形缓冲区的接口
    if(error_kurd(bsp_init_kurd)){
        asm volatile("hlt");
    }
    ksymmanager::Init(&symtable_file,symtable_file.size);  
    readonly_timer=new HPET_driver_only_read_time_stamp(&hpet_mmio);
    DmesgRingBuffer::Init(&log_buffer);
    Vec2i font_vec={.x=16, .y=32};
    bsp_init_kurd=textconsole_GoP::Init(&ter16x32_data[0][0][0],font_vec,0x00ffffffff,0);
    textconsole_GoP::Clear();
    serial_init_stage1();
    bsp_kout.Init();
    bsp_kout.shift_dec();
    tsc_regist();
    if (Status!=OS_SUCCESS)
    {
        bsp_kout<<"InitialKernelShellControler Failed\n";return ;
    }
    bsp_kout<<"Kernel Shell Initialed Success\n";
    GlobalKernelStatus=kernel_state::PANIC_WILL_ANALYZE;
    Panic::will_check();
    bsp_init_kurd=mem_init();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"mem_init Failed"<<kendl;
        return;
    }
    
    gAcpiVaddrSapceMgr.Init(global_gST);
    bsp_init_kurd=idt_vec_dispatch_mgr::Init(logical_processor_count);
    x86_smp_processors_container::regist_core(0);
    ktime::heart_beat_alarm::processor_regist();
    bsp_kout<<now<<"BSP online"<<kendl;
    gAnalyzer=new APIC_table_analyzer((MADT_Table*)gAcpiVaddrSapceMgr.get_acpi_table("APIC"));
    bsp_init_kurd=x86_smp_processors_container::AP_Init_one_by_one();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"x86_smp_processors_container::AP_Init_one_by_one Failed maybe code bug"<<kendl;
    }    
    Status=task_pool::Init();
    if(Status){
        bsp_kout<<"task_pool::Init Failed"<<kendl;return;
    }
    asm volatile("sti");   
    //中断接管工作
    new(global_schedulers) per_processor_scheduler;
    dmar::Init((dmar::acpi::DMAR_head*)gAcpiVaddrSapceMgr.get_acpi_table("DMAR"));
    main_router=new ioapic_driver(gAnalyzer->io_apic_list->front());
    i8042_interrupt_enable();
    global_container=new ecams_container_t((MCFG_Table*)gAcpiVaddrSapceMgr.get_acpi_table("MCFG"));
    create_first_kthread();
}
extern "C" void ap_final_work();
extern "C" void ap_init(uint32_t processor_id)
{
    longmode_enter_checkpoint.success_word=~processor_id;
    asm volatile("sfence");
    gKernelSpace->unsafe_load_pml4_to_cr3(KERNEL_SPACE_PCID);
    x86_smp_processors_container::regist_core(processor_id); 
    ktime::heart_beat_alarm::processor_regist();
    new(global_schedulers+processor_id) per_processor_scheduler;
    init_finish_checkpoint.success_word=~query_x2apicid();
    asm volatile("sfence");
    ap_final_work();
}
