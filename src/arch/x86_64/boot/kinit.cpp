#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "abi/os_error_definitions.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/tsc.h"
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
    asm volatile("cli");
    sc.sched();
}

extern "C" uint32_t assigned_cr3;
loaded_VM_interval* VM_intervals;
uint64_t VM_intervals_count;
phymem_segment *phymem_segments;
uint64_t phymem_segments_count; 
uint32_t logical_processor_count;
extern "C" void kernel_start(init_to_kernel_info* transfer) 
{   
    GlobalKernelStatus=kernel_state::EARLY_BOOT;
    logical_processor_count=transfer->logical_processor_count;
    int  Status=0;
    KURD_t bsp_init_kurd=KURD_t();
    pass_through_device_info*tfg=nullptr;
    loaded_VM_interval*hpet_mmio_interval,*graphic_buffer,*logbuffer,*first_heap,*first_heap_bitmap,*BCBs_bitmap,*kspaceUPpdpt,*ksymbols;
    for(uint64_t i=0;i<transfer->pass_through_device_info_count;i++){
        if(transfer->pass_through_devices[i].device_info==PASS_THROUGH_DEVICE_GRAPHICS_INFO){
            tfg=(pass_through_device_info*)&transfer->pass_through_devices[i];
        }
    }
    for(uint64_t i=0;i<transfer->loaded_VM_interval_count;i++){
        switch (transfer->loaded_VM_intervals[i].VM_interval_specifyid)
        {
        case VM_ID_BSP_INIT_STACK:
            
            break;
        
        case VM_ID_FIRST_HEAP_BITMAP:
            first_heap_bitmap = &transfer->loaded_VM_intervals[i];
            break;
        
        case VM_ID_FIRST_HEAP:
            first_heap = &transfer->loaded_VM_intervals[i];
            break;
        
        case VM_ID_LOGBUFFER:
            logbuffer = &transfer->loaded_VM_intervals[i];
            break;
        
        case VM_ID_KSYMBOLS:
            ksymbols = &transfer->loaded_VM_intervals[i];
            break;
        
        case VM_ID_UP_KSPACE_PDPT:
            kspaceUPpdpt = &transfer->loaded_VM_intervals[i];
            break;
        
        case VM_ID_GRAPHIC_BUFFER:
            graphic_buffer = &transfer->loaded_VM_intervals[i];
            break;
        case VM_ID_BCBS_BITMAPS:
            BCBs_bitmap = &transfer->loaded_VM_intervals[i];
            break;
        case VM_ID_HPET_MMIO:
            hpet_mmio_interval=&transfer->loaded_VM_intervals[i];
            break;
        default:
            // 未知的 VM interval，可以选择忽略或记录日志
            break;
        }
    }
    bsp_init_kurd=GfxPrim::Init((GlobalBasicGraphicInfoType*)tfg->specify_data,*graphic_buffer);//要开发直接写图形缓冲区的接口
    if(error_kurd(bsp_init_kurd)){
        asm volatile("hlt");
    }
    ksymmanager::Init(ksymbols,transfer->ksymbols_file_size);  
    kpoolmemmgr_t::Init(first_heap,first_heap_bitmap);  
    readonly_timer=new HPET_driver_only_read_time_stamp(hpet_mmio_interval);
    global_gST=(EFI_SYSTEM_TABLE*)transfer->gST_ptr;
    DmesgRingBuffer::Init(logbuffer);
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
    VM_intervals=new loaded_VM_interval[transfer->loaded_VM_interval_count];
    ksystemramcpy(transfer->loaded_VM_intervals,VM_intervals,transfer->loaded_VM_interval_count*sizeof(loaded_VM_interval));
    phymem_segments=new phymem_segment[transfer->phymem_segment_count];
    ksystemramcpy(transfer->memory_map,phymem_segments,transfer->phymem_segment_count*sizeof(phymem_segment));
    VM_intervals_count=transfer->loaded_VM_interval_count;
    phymem_segments_count=transfer->phymem_segment_count;
    GlobalKernelStatus=kernel_state::PANIC_WILL_ANALYZE;
    Panic::will_check();
    if(transfer->kmmu_root_table>=0x100000000){
        bsp_kout<<"Kernel Mmu Root Table is not in low memory"<<kendl;return;
    }
    asm volatile("sfence");
    assigned_cr3=transfer->kmmu_root_table;
    bsp_init_kurd=all_pages_arr::Init(transfer);
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"phymemspace_mgr Init Failed"<<kendl;return;
    }
    bsp_init_kurd=FreePagesAllocator::Init(FreePagesAllocator::BEST_FIT,BCBs_bitmap);//传入一个loaded_VM_entry
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"FreePagesAllocator Init Failed"<<kendl;return;
    }
    bsp_init_kurd=KspacePageTable::Init(kspaceUPpdpt);
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"KspaceMapMgr Init Failed"<<kendl;return;
    }
    gKernelSpace=new AddressSpace();
    for(uint64_t i=0;i<transfer->phymem_segment_count;i++){
        bsp_kout<<"[INFO] phymem_segment["<<i<<"]: start=0x"
                <<HEX<<transfer->memory_map[i].start
                <<", size=0x"<<HEX<<(transfer->memory_map[i].size)
                <<", type=";
        
        // 打印内存类型
        switch(transfer->memory_map[i].type){
            case EFI_RESERVED_MEMORY_TYPE:
                bsp_kout<<"EFI_RESERVED"; break;
            case EFI_LOADER_CODE:
                bsp_kout<<"EFI_LOADER_CODE"; break;
            case EFI_LOADER_DATA:
                bsp_kout<<"EFI_LOADER_DATA"; break;
            case EFI_BOOT_SERVICES_CODE:
                bsp_kout<<"EFI_BOOT_SERVICES_CODE"; break;
            case EFI_BOOT_SERVICES_DATA:
                bsp_kout<<"EFI_BOOT_SERVICES_DATA"; break;
            case EFI_RUNTIME_SERVICES_CODE:
                bsp_kout<<"EFI_RUNTIME_SERVICES_CODE"; break;
            case EFI_RUNTIME_SERVICES_DATA:
                bsp_kout<<"EFI_RUNTIME_SERVICES_DATA"; break;
            case freeSystemRam:
                bsp_kout<<"freeSystemRam"; break;
            case EFI_UNUSABLE_MEMORY:
                bsp_kout<<"EFI_UNUSABLE"; break;
            case EFI_ACPI_RECLAIM_MEMORY:
                bsp_kout<<"EFI_ACPI_RECLAIM"; break;
            case EFI_ACPI_MEMORY_NVS:
                bsp_kout<<"EFI_ACPI_NVS"; break;
            case EFI_MEMORY_MAPPED_IO:
                bsp_kout<<"EFI_MMIO"; break;
            case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
                bsp_kout<<"EFI_MMIO_PORT"; break;
            case EFI_PAL_CODE:
                bsp_kout<<"EFI_PAL_CODE"; break;
            case EFI_PERSISTENT_MEMORY:
                bsp_kout<<"EFI_PERSISTENT"; break;
            case EFI_UNACCEPTED_MEMORY_TYPE:
                bsp_kout<<"EFI_UNACCEPTED"; break;
            case EFI_MAX_MEMORY_TYPE:
                bsp_kout<<"EFI_MAX_TYPE"; break;
            case OS_KERNEL_DATA:
                bsp_kout<<"OS_KERNEL_DATA"; break;
            case OS_KERNEL_CODE:
                bsp_kout<<"OS_KERNEL_CODE"; break;
            case OS_KERNEL_STACK:
                bsp_kout<<"OS_KERNEL_STACK"; break;
            case OS_HARDWARE_GRAPHIC_BUFFER:
                bsp_kout<<"OS_GFX_BUFFER"; break;
            case ERROR_FAIL_TO_FIND:
                bsp_kout<<"ERROR_NOT_FOUND"; break;
            case OS_ALLOCATABLE_MEMORY:
                bsp_kout<<"OS_ALLOCATABLE"; break;
            case OS_RESERVED_MEMORY:
                bsp_kout<<"OS_RESERVED"; break;
            case OS_PGTB_SEGS:
                bsp_kout<<"OS_PGTABLE"; break;
            case OS_MEMSEG_HOLE:
                bsp_kout<<"OS_HOLE"; break;
            default:
                if(transfer->memory_map[i].type >= MEMORY_TYPE_OEM_RESERVED_MIN && 
                   transfer->memory_map[i].type <= MEMORY_TYPE_OEM_RESERVED_MAX){
                    bsp_kout<<"OEM_RESERVED(0x"<<HEX<<static_cast<uint32_t>(transfer->memory_map[i].type)<<")";
                } else if(transfer->memory_map[i].type >= MEMORY_TYPE_OS_RESERVED_MIN && 
                          transfer->memory_map[i].type <= MEMORY_TYPE_OS_RESERVED_MAX){
                    bsp_kout<<"OS_RESERVED_EXT(0x"<<HEX<<static_cast<uint32_t>(transfer->memory_map[i].type)<<")";
                } else {
                    bsp_kout<<"UNKNOWN(0x"<<HEX<<static_cast<uint32_t>(transfer->memory_map[i].type)<<")";
                }
                
                break;
        }
        bsp_kout<<DEC<<kendl;
    }
    bsp_init_kurd=gKernelSpace->second_stage_init();//传入trasfer,特殊重载，要接手相关内存
    bsp_init_kurd=[&]()->KURD_t{
        KURD_t kurd=KURD_t();
        pgaccess WB_ACCESS={
                .is_kernel=1,
                .is_writeable=1,
                .is_readable=1,
                .is_executable=0,
                .is_global=0,
                .cache_strategy=WB
            };
        pgaccess UC_ACCESS={
                .is_kernel=1,
                .is_writeable=1,
                .is_readable=1,
                .is_executable=0,
                .is_global=0,
                .cache_strategy=UC
            };
        pgaccess WB_RX_ACCESS{
            .is_kernel=1,
                .is_writeable=1,
                .is_readable=0,
                .is_executable=1,
                .is_global=0,
                .cache_strategy=WB
        };
        VM_DESC identity_map_template={
            .start=0,
            .end=0,
            .map_type=VM_DESC::MAP_PHYSICAL,
            .phys_start=0,
            .access=WB_ACCESS,
            .committed_full=1,
            .is_vaddr_alloced=1,
            .is_out_bound_protective=0,
            .SEG_SIZE_ONLY_UES_IN_BASIC_SEG=0xffffffffffffffff
        };
        for(uint64_t i=0;i<phymem_segments_count;i++){
            identity_map_template.start=phymem_segments[i].start;
            identity_map_template.phys_start=phymem_segments[i].start;
            identity_map_template.end=phymem_segments[i].size+phymem_segments[i].start;
            if(phymem_segments[i].type==PHY_MEM_TYPE::EFI_MEMORY_MAPPED_IO||
            phymem_segments[i].type==PHY_MEM_TYPE::EFI_ACPI_MEMORY_NVS||
            phymem_segments[i].type==PHY_MEM_TYPE::EFI_RESERVED_MEMORY_TYPE){
                identity_map_template.access=UC_ACCESS;
            }else if(phymem_segments[i].type==PHY_MEM_TYPE::EFI_RUNTIME_SERVICES_CODE){
                identity_map_template.access=WB_RX_ACCESS;
            }else if(phymem_segments[i].type==PHY_MEM_TYPE::EFI_RUNTIME_SERVICES_DATA){
                identity_map_template.access=WB_ACCESS;
            }
            kurd=gKernelSpace->enable_VM_desc(identity_map_template);
            
            if(error_kurd(kurd))return kurd;
        }
        for(uint64_t i=0;i<VM_intervals_count;i++){
            if(VM_intervals[i].VM_interval_specifyid<VM_ID_architecture_agnostic_base){
                if(VM_intervals[i].vbase<0xffff800000000000){
                    identity_map_template.start=VM_intervals[i].vbase;
            identity_map_template.phys_start=VM_intervals[i].pbase;
            identity_map_template.end=VM_intervals[i].vbase+align_up(VM_intervals[i].size,4096);
            identity_map_template.access=VM_intervals[i].access;
            kurd=gKernelSpace->enable_VM_desc(identity_map_template);
            if(error_kurd(kurd))return kurd;
                }
            }
        }
        return kurd;
    }();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"identity map fail"<<kendl;return;
    }
    Status=EFI_RT_SVS::Init((EFI_SYSTEM_TABLE*)transfer->gST_ptr);
    gAcpiVaddrSapceMgr.Init(global_gST);
    gKernelSpace->unsafe_load_pml4_to_cr3(KERNEL_SPACE_PCID);
    GlobalKernelStatus=kernel_state::MM_READY;
    bsp_init_kurd=idt_vec_dispatch_mgr::Init(logical_processor_count);
    x86_smp_processors_container::regist_core(0);
    ktime::heart_beat_alarm::processor_regist();
    bsp_kout<<now<<"BSP online"<<kendl;
    gAnalyzer=new APIC_table_analyzer((MADT_Table*)gAcpiVaddrSapceMgr.get_acpi_table("APIC"));
    bsp_init_kurd=kpoolmemmgr_t::multi_heap_enable();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"Kpoolmemmgr_t::multi_heap_enable Failed"<<kendl;
    }
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
    //dmar::Init((dmar::acpi::DMAR_head*)gAcpiVaddrSapceMgr.get_acpi_table("DMAR"));
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
