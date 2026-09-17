#include "arch/x86_64/core_hardwares/PortDriver.h"
#include "abi/os_error_definitions.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/tsc.h"
#include "arch/x86_64/mem_init.h"
#include "kcirclebufflogMgr.h"
#include "util/debug_tmp_ring_buff.h"   // interrupt_log_ring（IRQ-safe 断言/日志环）
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/HPET.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "memory/kpoolmemmgr.h"
#include "memory/FreePagesAllocator.h"            // FreePagesAllocator::alloc（4MiB 环内存）
#include "memory/main_phyaddr_access_window.h"    // PHYACC_VA（主窗口直映射重链）
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
#include "kernel_boot_functions.h"
#include "boot/info_pkg_link.h"
#include "boot/asset_table.h"



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
    {
        // 启动 BQ 超时扫描线程
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
    i8042_char_subscriber_init();
    //pcie_text_praser();
    // 并行初始化所有 NVMe 控制器（每控制器一线程，共享 u64 汇报画板，≤5s 轮询提前退出）
    nvme_parallel_init_all();
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
                nullptr, &inshort, kurd_get_raw(fatal));
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



loaded_VM_interval* VM_intervals;
GlobalBasicGraphicInfoType gop_info;
XSDT_Table *XSDT;

// interrupt_log_ring 的对象本体：.bss 对齐存储。
// 全局 C++ 构造函数被禁 ⇒ 由 kernel_start 运行时 placement new 出生。
alignas(debug_tmp_ring_buff) static uint8_t g_interrupt_log_ring_obj[sizeof(debug_tmp_ring_buff)];

extern "C" void fred_enable(gs_complex_t*gs_complex);


extern "C" void kernel_start() 
{   
    int  Status=0;
    KURD_t bsp_init_kurd=KURD_t();
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
            "global_schedulers alloc failed", nullptr, &inshort, kurd_get_raw(alloc_kurd));
    }
    for (uint32_t i = 0; i < logical_processor_count; i++) {
        new (&global_schedulers[i]) per_processor_scheduler();
        gs_complex_t* cx = (gs_complex_t*)(conjucnt_GSs.vbase() + i * GS_COMPLEX_STRIDE);
        global_schedulers[i].placed_init(cx->stacks_ptr);
    }
    gs_u64_write(PROCESSOR_SCHEDULER_GS_INDEX, (uint64_t)&global_schedulers[fast_get_processor_id()]);

    // ── IRQ-safe 断言 / 日志环：AP bring-up 前就绪 ──
    // AP 起来后所有核都会写它（断言 / 中断），故必须早于 ap_init_one_by_one。
    {
        constexpr uint64_t IRQ_LOG_RING_BYTES = 4ull << 20;   // 4 MiB
        KURD_t ring_kurd = KURD_t();
        phyaddr_t ring_pbase = FreePagesAllocator::alloc(
            IRQ_LOG_RING_BYTES, BUDDY_ALLOC_DEFAULT_FLAG,
            page_state_t::kernel_pinned, ring_kurd);
        if (ring_pbase == FreePagesAllocator::INVALID_ALLOC_BASE || error_kurd(ring_kurd)) {
            panic_info_inshort inshort = {
                .is_bug = true, .is_policy = false,
                .is_hw_fault = false, .is_mem_corruption = false,
                .is_escalated = false
            };
            Panic::panic(default_panic_behaviors_flags,
                (char*)"interrupt_log_ring: FPA alloc failed", nullptr, &inshort,
                kurd_get_raw(ring_kurd));
        }
        // 基址是物理地址 → 经主窗口（恒等窗口，pbase=0）换算成本 ELF 可访问 VA。
        DmesgRingBuffer_soul ring_soul = {
            (void*)PHYACC_VA(ring_pbase), IRQ_LOG_RING_BYTES, 0, 0
        };
        interrupt_log_ring = new (g_interrupt_log_ring_obj) debug_tmp_ring_buff(&ring_soul);
        bsp_kout << "interrupt_log_ring online: pbase=0x" << HEX << ring_pbase
                 << " va=0x" << (uint64_t)interrupt_log_ring->get_soul()->buff << DEC
                 << " bytes=" << IRQ_LOG_RING_BYTES << kendl;
    }

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
static uint64_t ipi_shutdown_func(void*)
{
    asm volatile("cli; wbinvd; hlt" ::: "memory");
    return 1;
}
// ─── 广播关机 ─────────────────────────────────────────────
// 遍历除 self 外所有 AP，逐一 fly_ipi_send 关机
// 50ms 硬上限，超时则跳过剩余 AP，发起者自救

extern "C" void broadcast_shutdown()
{
    // 关机：并行析构/关机所有 NVMe 控制器（每控制器一线程，汇报画板 ≤5s 轮询）
    nvme_parallel_offline_all();

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
}



