#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "abi/os_error_definitions.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "arch/x86_64/mem_init.h"
#include "kcirclebufflogMgr.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "memory/kpoolmemmgr.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "memory/AddresSpace.h"
#include "memory/all_pages_arr.h"
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
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/core_hardwares/DMAR.h"
#include "arch/x86_64/core_hardwares/ioapic.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "arch/x86_64/PCIe/prased.h"
#include "arch/x86_64/boot.h"
#include "arch/x86_64/intel_processor_trace.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "KImage_Introspection.h"
#include "Scheduler/task_pool.h"
#include "exec_env_detect.h"



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
void ipi_start_sched(x64_standard_context_v2* ctx){
    ctx=nullptr;
    uint32_t self_processor_id=fast_get_processor_id();
    bsp_kout<<"processor id "<< self_processor_id<<" start scheduling"<<kendl;
    get_self_scheduler()->next_task_with_routine();
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
extern void* bq_timeout_sweeper(void*);

void*kthread_ymir(void*null){//所有内核线程的始祖之"尤米尔线程"（出自进击的巨人）
    (void)null;
    KURD_t kurd = KURD_t();
    
    i8042_char_subscriber_init();
    pcie_text_praser();
    //text_input_subscriber_init();

    // 启动 BQ 超时扫描线程
    {
        kthread_creating_package pkg = {};
        pkg.func_raw = (uint64_t)bq_timeout_sweeper;
        pkg.args[0]  = (uint64_t)nullptr;
        pkg.launch_pid = 0;
        KURD_t kurd2{};
        uint64_t tid = creat_kthread(&pkg, &kurd2);
        if (error_kurd(kurd2)) {
            bsp_kout << "[BQ] sweeper thread spawn failed" << kendl;
        } else {
            bsp_kout << "[BQ] sweeper thread tid="<<tid  << kendl;
        }
    }

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

extern "C" x2apicid_t x2apic_core_init();


void create_first_kthread(){
    textconsole_GoP::RuntimeInitServiceThread();
    serial_init_stage2();
    GlobalKernelStatus=SCHEDUL_READY;
    /** */
    // 逐 AP 发送跑飞型 IPI 启动调度器（串行，任意超时即失败）
    for (uint32_t pid = 1; pid < logical_processor_count; pid++) {
        ipi_package_t ipi;
        ipi.arg        = nullptr;
        ipi.func       = (uint64_t)ipi_start_sched;
        ipi.id         = pid;
        ipi.is_apicid  = false;
        ipi.is_returnable = false;

        uint64_t rc = fly_ipi_send(&ipi);
        if (rc != 1) {
            KURD_t fatal = KURD_t(result_code::FATAL, 0,
                module_code::INTERRUPT, 0, 0,
                level_code::FATAL, err_domain::CORE_MODULE);
            fatal.reason = static_cast<uint16_t>(rc);
            panic_info_inshort inshort = {
                .is_bug = true, .is_policy = true,
                .is_hw_fault = false, .is_mem_corruption = false,
                .is_escalated = false
            };
            Panic::panic(default_panic_behaviors_flags,
                (char*)"create_first_kthread: AP start failed",
                nullptr, &inshort, fatal);
            __builtin_unreachable();
        }
    }

    kthread_creating_package pkg;
    pkg.func_raw = (uint64_t)kthread_ymir;
    pkg.args[0] = 0;
    pkg.args[1] = 0;
    pkg.args[2] = 0;
    pkg.args[3] = 0;
    pkg.args[4] = 0;
    pkg.launch_pid = fast_get_processor_id();
    KURD_t kurd = KURD_t();
    uint64_t kthread_ymir_tid = creat_kthread(&pkg, &kurd);
    get_self_scheduler()->next_task_with_routine();
}

// ─── 核心关机 IPI handler（跑飞型，三指令收工） ────────────
// 由 fly_ipi_send 投递到目标核，cli + wbinvd + hlt 后永不复返

static uint64_t ipi_shutdown_func(void*)
{
    asm volatile("cli; wbinvd; hlt" ::: "memory");
    __builtin_unreachable();
    return 1;
}

// ─── 广播关机 ─────────────────────────────────────────────
// 遍历除 self 外所有 AP，逐一 fly_ipi_send 关机
// 50ms 硬上限，超时则跳过剩余 AP，发起者自救

extern "C" void broadcast_shutdown()
{
    uint32_t self = fast_get_processor_id();
    uint32_t nproc = logical_processor_count;
    uint64_t deadline = ktime::get_microsecond_stamp() + 50000;

    for (uint32_t pid = 0; pid < nproc; pid++) {
        if (pid == self) continue;
        if (ktime::get_microsecond_stamp() >= deadline)
            break;

        ipi_package_t ipi;
        ipi.arg        = nullptr;
        ipi.func       = (uint64_t)ipi_shutdown_func;
        ipi.id         = pid;
        ipi.is_apicid  = false;
        ipi.is_returnable = false;

        fly_ipi_send(&ipi);  // best-effort
    }

    // 发起者自救
    asm volatile("cli; wbinvd; hlt" ::: "memory");
    __builtin_unreachable();
}

loaded_VM_interval* VM_intervals;
GlobalBasicGraphicInfoType gop_info;
XSDT_Table *XSDT;
void very_early_init(init_to_kernel_header* transfer){
    g_env = probe_env();
    GlobalKernelStatus=kernel_state::EARLY_BOOT;
    kpoolmemmgr_t::Init();
    kIMG_self_window=transfer->kIMG_self_window;
    pages_arr=transfer->pages_arr;
    FPA_bitmaps=transfer->FPA_bitmaps;
    kIMG_size=transfer->kIMG_self_size;
    log_buffer=transfer->log_buffer;
    symtable_file=transfer->symtable_file;
    initramfs_file=transfer->initramfs_file;
    x86_specify_init_to_kernel_info* arch=(x86_specify_init_to_kernel_info*)(uint64_t(transfer)+transfer->arch_specify_offset);
    hpet_mmio=arch->hpet_mmio;
    conjucnt_GSs=arch->conjunc_GSs;
    gop_info=arch->gop_info;
    g_xsdt_base      = arch->XSDT_base;
    gop_buffer.vpn   = arch->Gop_vbase >> 12;
    gop_buffer.ppn   = arch->gop_info.FrameBufferBase >> 12;
    gop_buffer.npages= align_up(arch->gop_info.FrameBufferSize, 4096) >> 12;
    gop_buffer.access={1,1,1,0,1,WC};
    Kspace_phyaddr_access_window = transfer->Kspace_phyaddr_access_window;
    logical_processor_count=transfer->logical_processor_count;
    self_introspection_init();  // 读取 kIMG_self_window 全局，内部缓存 BSS 程序头
    if(transfer->loaded_VM_interval_count)
    VM_intervals=new loaded_VM_interval[transfer->loaded_VM_interval_count];
    ksystemramcpy((void*)(uint64_t(transfer)+transfer->loaded_VM_intervals_offset),VM_intervals,transfer->loaded_VM_interval_count*sizeof(loaded_VM_interval));
    phymem_segments=new phymem_segment[transfer->phymem_segment_count];
    ksystemramcpy((void*)(uint64_t(transfer)+transfer->memory_map_offset),phymem_segments,transfer->phymem_segment_count*sizeof(phymem_segment));
    VM_intervals_count=transfer->loaded_VM_interval_count;
    phymem_segments_count=transfer->phymem_segment_count;
    hw_stacks.vpn=arch->hdstacks_interval_vbase>>12;
    hw_stacks.ppn=arch->hdstacks_interval_pbase>>12;
    hw_stacks.npages=arch->hdstacks_4kbpgs_count;
}
extern "C" void fred_enable(gs_complex_t*gs_complex);
extern "C" void kernel_start(init_to_kernel_header* transfer) 
{   
    very_early_init(transfer);
    ksetmem_8(transfer,0,transfer->self_pages_count*0x1000);
    transfer=nullptr;//此信息包是属于阅后即焚
    int  Status=0;
    KURD_t bsp_init_kurd=KURD_t();
    bsp_init_kurd=GfxPrim::Init(&gop_info,gop_buffer);//要开发直接写图形缓冲区的接口
    if(error_kurd(bsp_init_kurd)){
        return;
    }
    ksymmanager::Init(&symtable_file.interval, symtable_file.size);  
    readonly_timer = new HPET_driver();
    readonly_timer->Init(&hpet_mmio);
    DmesgRingBuffer::Init(&log_buffer);
    Vec2i font_vec={.x=16, .y=32};
    bsp_init_kurd=textconsole_GoP::Init(&ter16x32_data[0][0][0],font_vec,0x00ffffffff,0);
    textconsole_GoP::Clear();
    serial_init_stage1();
    bsp_kout.Init();
    bsp_kout.shift_dec();
    if (Status!=OS_SUCCESS)
    {
        bsp_kout<<"InitialKernelShellControler Failed\n";return ;
    }
    bsp_kout<<"Kernel Shell Initialed Success\n";
    tsc_regist();
    GlobalKernelStatus=kernel_state::PANIC_WILL_ANALYZE;
    Panic::will_check();
    bsp_init_kurd=mem_init();
    if(error_kurd(bsp_init_kurd)){
        bsp_kout<<"mem_init Failed"<<kendl;
        return;
    }/*
    if(g_env==ENV_BARE_METAL){
        global_pt_blackboxes= new pt_blackbox[logical_processor_count];
        ksetmem_8(global_pt_blackboxes,0,sizeof(pt_blackbox)*logical_processor_count);
        prepare_blackbox(global_pt_blackboxes);
        enable_blackbox(global_pt_blackboxes);
    }*/
    gAcpiVaddrSapceMgr.Init(g_xsdt_base);
    if(fred_support_catch_bit){
        fred_enable((gs_complex_t*)rdmsr(msr::syscall::IA32_GS_BASE));
    }
    x2apic_core_init();
    ktime::heart_beat_alarm::processor_regist();
    bsp_kout<<now<<"BSP online"<<kendl;
    gAnalyzer = new APIC_table_analyzer((MADT_Table*)gAcpiVaddrSapceMgr.get_acpi_table("APIC"));

    // 调度器数组必须在 AP 启动前就绪（AP 在 ap_init 中写 GS slot 5）
    size_t sched_bytes = sizeof(per_processor_scheduler) * logical_processor_count;
    size_t sched_pages = (sched_bytes + 4095) / 4096;
    KURD_t alloc_kurd;
    global_schedulers = (per_processor_scheduler*)__wrapped_pgs_valloc(
        &alloc_kurd, sched_pages, page_state_t::kernel_pinned, 12);
    if (!global_schedulers || error_kurd(alloc_kurd)) {
        panic_info_inshort inshort = {
            .is_bug = true, .is_policy = false,
            .is_hw_fault = false, .is_mem_corruption = false,
            .is_escalated = false
        };
        Panic::panic(default_panic_behaviors_flags,
            "global_schedulers alloc failed", nullptr, &inshort, alloc_kurd);
    }
    for (uint32_t i = 0; i < logical_processor_count; i++) {
        new (&global_schedulers[i]) per_processor_scheduler();
        gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + i * GS_COMPLEX_STRIDE);
        global_schedulers[i].placed_init(cx->stacks_ptr);
    }
    gs_u64_write(PROCESSOR_SCHEDULER_GS_INDEX, (uint64_t)&global_schedulers[fast_get_processor_id()]);

    bsp_init_kurd = ap_init_one_by_one();
    if (error_kurd(bsp_init_kurd)) {
        bsp_kout << "x86_smp_processors_container::AP_Init_one_by_one Failed maybe code bug" << kendl;
    }
    Status = task_pool::Init();
    if (Status) {
        bsp_kout << "task_pool::Init Failed" << kendl; return;
    }
    asm volatile("sti");
    //中断接管工作
    dmar::Init((dmar::acpi::DMAR_head*)gAcpiVaddrSapceMgr.get_acpi_table("DMAR"));
    main_router=new ioapic_driver(gAnalyzer->io_apic_list->front());
    bq_system_init();
    i8042_interrupt_enable();
    global_container=new ecams_container_t((MCFG_Table*)gAcpiVaddrSapceMgr.get_acpi_table("MCFG"));
    create_first_kthread();
}
extern "C" void ap_final_work();
check_point init_finish_checkpoint;
extern void apply_umwait_control(void);
extern "C" void ap_init()
{   
    asm volatile("sfence");
    apply_umwait_control();
    ktime::heart_beat_alarm::processor_regist();
    if(fred_support_catch_bit){
        fred_enable((gs_complex_t*)rdmsr(msr::syscall::IA32_GS_BASE));
    }/*
    if(g_env==ENV_BARE_METAL){
        prepare_blackbox(global_pt_blackboxes+fast_get_processor_id());
        enable_blackbox(global_pt_blackboxes+fast_get_processor_id());
    }*/
    gs_u64_write(PROCESSOR_SCHEDULER_GS_INDEX, (uint64_t)&global_schedulers[fast_get_processor_id()]);
    init_finish_checkpoint.success_word=~query_x2apicid();
    asm volatile("sfence");
    ap_final_work();
}
