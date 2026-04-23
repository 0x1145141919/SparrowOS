#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/Interrupt_system/AP_Init_error_observing_protocol.h"
#include "util/kout.h"
#include "firmware/ACPI_APIC.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "ktime.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/textConsole.h"
check_point longmode_enter_checkpoint={0};
check_point init_finish_checkpoint={0};
constexpr uint32_t error_code_bitmap = 0
    | (1 << 8)   // #DF
    | (1 << 10)  // #TS
    | (1 << 11)  // #NP
    | (1 << 12)  // #SS
    | (1 << 13)  // #GP
    | (1 << 14)  // #PF
    | (1 << 17)  // #AC
    | (1 << 21); // #CP
KURD_t x86_smp_processors_container::AP_Init_one_by_one()
{
    //此函数会需要保证低0～64k内存的恒等映射
    // 使用gAnalyzer的processor_x64_list链表进行遍历
    KURD_t fail=default_fail();
    fail.event_code=INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::EVENT_CODE_APS_INIT;
    KURD_t success=default_success();
    success.event_code=INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::EVENT_CODE_APS_INIT;
    KURD_t fatal=default_fatal();
    fatal.event_code=INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::EVENT_CODE_APS_INIT;
    if((uint64_t)&AP_realmode_start%4096){
        bsp_kout<<"[x64_local_processor]AP_realmode_start is not aligned to 4K"<<kendl;
        //ap入口必须4K对齐，否则触发bugpanic
    }
    if(gAnalyzer == nullptr) {
        bsp_kout<< "gAnalyzer is null, cannot initialize processors" << kendl;
        fail.result=result_code::RETRY;
        fail.reason=INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::RETRY_REASON_CODE::RETRY_REASON_CODE_DEPENDIES_NOT_INITIALIZED;
        return fail;
    }
    x2apicid_t self_x2apicid = query_x2apicid();
    phyaddr_t AP_realmode_start_addr=(phyaddr_t)&AP_realmode_start;
    x2apic::x2apic_icr_t icr_sipi={
        .param={
            .vector=(uint8_t)(AP_realmode_start_addr/4096),
            .delivery_mode=6,
            .destination_mode=LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
            .reserved1=0,
            .level=1,
            .trigger_mode=0,
            .reserved2=0,
            .destination_shorthand=LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::NO_SHORTHAND,
            .reserved3=0,
            .destination=0
        }
    };
    
    
    {
        x2apic::x2apic_icr_t icr_init={
        .param={
            .vector=0,
            .delivery_mode=5,
            .destination_mode=LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
            .reserved1=0,
            .level=1,
            .trigger_mode=0,
            .reserved2=0,
            .destination_shorthand=LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::ALL_EXCLUDING_SELF,
            .reserved3=0,
            .destination=0
        }
        };
    bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one try init all exclued self prcessor"<<kendl;
    x2apic::x2apic_driver::raw_send_ipi(icr_init);
    bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one init all exclued self prcessor sucess"<<kendl;
    ktime::microsecond_polling_delay_by_hpet(20000);
    }
    {
        x2apic::x2apic_icr_t icr_init_de_assert={
        .param={
            .vector=0,
            .delivery_mode=5,
            .destination_mode=LAPIC_PARAMS_ENUM::DESTINATION_T::PHYSICAL,
            .reserved1=0,
            .level=0,
            .trigger_mode=1,
            .reserved2=0,
            .destination_shorthand=LAPIC_PARAMS_ENUM::DESTINATION_SHORTHAND_T::ALL_INCLUDING_SELF,
            .reserved3=0,
            .destination=0
        }
    };
    bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one try init-de-assert all exclued self prcessor"<<kendl;
    x2apic::x2apic_driver::raw_send_ipi(icr_init_de_assert);
    bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one init-de-assert all exclued self prcessor sucess"<<kendl;
    ktime::microsecond_polling_delay_by_hpet(1000);
    }
    
    uint32_t processor_id = 1;
    enum ap_observe_once_result_t:uint8_t{
        WAIT_TAHT_TIME,
        SUCCESS_TAHT_TIME,
        FAIL_TAHT_TIME,
    };
    enum ap_observe_result_t:uint8_t{
        CHECKPOINT_SUCCESS,
        CHECKPOINT_FAIL,
        CHECKPOINT_TIMEOUT,
    };
    ap_observe_once_result_t (*observe_realmode)(uint32_t)=[](uint32_t succeed_word)->ap_observe_once_result_t {
        if(realmode_enter_checkpoint.success_word==succeed_word)
            return SUCCESS_TAHT_TIME;
        return WAIT_TAHT_TIME;
    };
    ap_observe_once_result_t (*observe_pemode_enter)(uint32_t)=[](uint32_t succeed_word)->ap_observe_once_result_t {
        if(pemode_enter_checkpoint.success_word==succeed_word)
            return SUCCESS_TAHT_TIME;
        if(realmode_enter_checkpoint.failure_flags&1)
            return FAIL_TAHT_TIME;
        return WAIT_TAHT_TIME;
    };
    void (*pe_fail_dealing)()=[](){
        bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one observe_realmode_enter failed with flags"<<realmode_enter_checkpoint.failure_flags<<kendl;
        if(realmode_enter_checkpoint.failure_flags&2){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one observe realmode exception:"<<realmode_enter_checkpoint.failure_caused_excption_num<<kendl;
            uint8_t interrupt_vec=realmode_enter_checkpoint.failure_caused_excption_num;
            AP_Init_error_observing_protocol::realmode_final_stack_frame*ap_frame=(AP_Init_error_observing_protocol::realmode_final_stack_frame*)realmode_enter_checkpoint.failure_final_stack_top;
            if(ap_frame->magic==AP_Init_error_observing_protocol::realmode_magic){
                bsp_kout<<now<<"[Realmode Exception Frame]"<<kendl;
                bsp_kout<<now<<"Magic: 0x"<<ap_frame->magic<<kendl;
                bsp_kout<<now<<"CR0: 0x"<<ap_frame->cr0<<kendl;
                bsp_kout<<now<<"GS: 0x"<<ap_frame->gs<<kendl;
                bsp_kout<<now<<"FS: 0x"<<ap_frame->fs<<kendl;
                bsp_kout<<now<<"SS: 0x"<<ap_frame->ss<<kendl;
                bsp_kout<<now<<"DS: 0x"<<ap_frame->ds<<kendl;
                bsp_kout<<now<<"ES: 0x"<<ap_frame->es<<kendl;
                bsp_kout<<now<<"DI: 0x"<<ap_frame->di<<kendl;
                bsp_kout<<now<<"SI: 0x"<<ap_frame->si<<kendl;
                bsp_kout<<now<<"BP: 0x"<<ap_frame->bp<<kendl;
                bsp_kout<<now<<"SP: 0x"<<ap_frame->sp<<kendl;
                bsp_kout<<now<<"DX: 0x"<<ap_frame->dx<<kendl;
                bsp_kout<<now<<"CX: 0x"<<ap_frame->cx<<kendl;
                bsp_kout<<now<<"BX: 0x"<<ap_frame->bx<<kendl;
                bsp_kout<<now<<"AX: 0x"<<ap_frame->ax<<kendl;
                bsp_kout<<now<<"CS: 0x"<<ap_frame->cs<<kendl;
                bsp_kout<<now<<"IP: 0x"<<ap_frame->ip<<kendl;
                bsp_kout<<now<<"EFLAGS: 0x"<<ap_frame->eflags<<kendl;
            }
        }
    };
    ap_observe_once_result_t (*observe_longmode_enter)(uint32_t)=[](uint32_t succeed_word)->ap_observe_once_result_t {
        if(longmode_enter_checkpoint.success_word==succeed_word)
            return SUCCESS_TAHT_TIME;
        if(pemode_enter_checkpoint.failure_flags&1)
            return FAIL_TAHT_TIME;
        return WAIT_TAHT_TIME;
    };
    void (*longmode_enter_fail_dealing)()=[](){
        bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one observe_pemode_enter failed with flags"<<pemode_enter_checkpoint.failure_flags<<kendl;
        if(pemode_enter_checkpoint.failure_flags&2){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one observe pemode exception:"<<pemode_enter_checkpoint.failure_caused_excption_num<<kendl;
            uint8_t interrupt_vec=pemode_enter_checkpoint.failure_caused_excption_num;
            if((1ULL<<interrupt_vec)&error_code_bitmap){
                AP_Init_error_observing_protocol::pemode_final_stack_frame_with_errcode* AP_frame=
                (AP_Init_error_observing_protocol::pemode_final_stack_frame_with_errcode*)
                pemode_enter_checkpoint.failure_final_stack_top;
                if(AP_frame->magic==AP_Init_error_observing_protocol::PE_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC){
                    bsp_kout<<now<<"[Pemode Exception Frame with Error Code]"<<kendl;
                    bsp_kout<<now<<"Magic: 0x"<<AP_frame->magic<<kendl;
                    bsp_kout<<now<<"IA32_EFER: 0x"<<AP_frame->IA32_EFER<<kendl;
                    bsp_kout<<now<<"CR4: 0x"<<AP_frame->cr4<<kendl;
                    bsp_kout<<now<<"CR3: 0x"<<AP_frame->cr3<<kendl;
                    bsp_kout<<now<<"CR2: 0x"<<AP_frame->cr2<<kendl;
                    bsp_kout<<now<<"CR0: 0x"<<AP_frame->cr0<<kendl;
                    bsp_kout<<now<<"GS: 0x"<<AP_frame->gs<<kendl;
                    bsp_kout<<now<<"FS: 0x"<<AP_frame->fs<<kendl;
                    bsp_kout<<now<<"SS: 0x"<<AP_frame->ss<<kendl;
                    bsp_kout<<now<<"DS: 0x"<<AP_frame->ds<<kendl;
                    bsp_kout<<now<<"ES: 0x"<<AP_frame->es<<kendl;
                    bsp_kout<<now<<"EDI: 0x"<<AP_frame->edi<<kendl;
                    bsp_kout<<now<<"ESI: 0x"<<AP_frame->esi<<kendl;
                    bsp_kout<<now<<"EBP: 0x"<<AP_frame->ebp<<kendl;
                    bsp_kout<<now<<"ESP: 0x"<<AP_frame->esp<<kendl;
                    bsp_kout<<now<<"EDX: 0x"<<AP_frame->edx<<kendl;
                    bsp_kout<<now<<"ECX: 0x"<<AP_frame->ecx<<kendl;
                    bsp_kout<<now<<"EBX: 0x"<<AP_frame->ebx<<kendl;
                    bsp_kout<<now<<"EAX: 0x"<<AP_frame->eax<<kendl;
                    bsp_kout<<now<<"Error Code: 0x"<<AP_frame->errcode<<kendl;
                    bsp_kout<<now<<"CS: 0x"<<AP_frame->cs<<kendl;
                    bsp_kout<<now<<"EIP: 0x"<<AP_frame->eip<<kendl;
                    bsp_kout<<now<<"EFLAGS: 0x"<<AP_frame->eflags<<kendl;
                }
            }else{
                AP_Init_error_observing_protocol::pemode_final_stack_frame* AP_frame=
                (AP_Init_error_observing_protocol::pemode_final_stack_frame*)
                pemode_enter_checkpoint.failure_final_stack_top;
                if(AP_frame->magic==AP_Init_error_observing_protocol::PE_FINAL_STACK_NO_ERRCODE_TOP_MAGIC){
                    bsp_kout<<now<<"[Pemode Exception Frame without Error Code]"<<kendl;
                    bsp_kout<<now<<"Magic: 0x"<<AP_frame->magic<<kendl;
                    bsp_kout<<now<<"IA32_EFER: 0x"<<AP_frame->IA32_EFER<<kendl;
                    bsp_kout<<now<<"CR4: 0x"<<AP_frame->cr4<<kendl;
                    bsp_kout<<now<<"CR3: 0x"<<AP_frame->cr3<<kendl;
                    bsp_kout<<now<<"CR2: 0x"<<AP_frame->cr2<<kendl;
                    bsp_kout<<now<<"CR0: 0x"<<AP_frame->cr0<<kendl;
                    bsp_kout<<now<<"GS: 0x"<<AP_frame->gs<<kendl;
                    bsp_kout<<now<<"FS: 0x"<<AP_frame->fs<<kendl;
                    bsp_kout<<now<<"SS: 0x"<<AP_frame->ss<<kendl;
                    bsp_kout<<now<<"DS: 0x"<<AP_frame->ds<<kendl;
                    bsp_kout<<now<<"ES: 0x"<<AP_frame->es<<kendl;
                    bsp_kout<<now<<"EDI: 0x"<<AP_frame->edi<<kendl;
                    bsp_kout<<now<<"ESI: 0x"<<AP_frame->esi<<kendl;
                    bsp_kout<<now<<"EBP: 0x"<<AP_frame->ebp<<kendl;
                    bsp_kout<<now<<"ESP: 0x"<<AP_frame->esp<<kendl;
                    bsp_kout<<now<<"EDX: 0x"<<AP_frame->edx<<kendl;
                    bsp_kout<<now<<"ECX: 0x"<<AP_frame->ecx<<kendl;
                    bsp_kout<<now<<"EBX: 0x"<<AP_frame->ebx<<kendl;
                    bsp_kout<<now<<"EAX: 0x"<<AP_frame->eax<<kendl;
                    bsp_kout<<now<<"CS: 0x"<<AP_frame->cs<<kendl;
                    bsp_kout<<now<<"EIP: 0x"<<AP_frame->eip<<kendl;
                    bsp_kout<<now<<"EFLAGS: 0x"<<AP_frame->eflags<<kendl;
                }
            }
        }
    };
    ap_observe_once_result_t (*observe_finish)(uint32_t)=[](uint32_t succeed_word)->ap_observe_once_result_t {
        if(init_finish_checkpoint.success_word==succeed_word)
            return SUCCESS_TAHT_TIME;
        if(longmode_enter_checkpoint.failure_flags&1)
            return FAIL_TAHT_TIME;
        return WAIT_TAHT_TIME;
    };
    void (*finish_fail_dealing)()=[](){
        bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one observe_pemode_enter failed with flags"<<longmode_enter_checkpoint.failure_flags<<kendl;
        if(longmode_enter_checkpoint.failure_flags&2){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one observe pemode exception:"<<longmode_enter_checkpoint.failure_caused_excption_num<<kendl;
            uint8_t interrupt_vec=longmode_enter_checkpoint.failure_caused_excption_num;
            if((1ULL<<interrupt_vec)&error_code_bitmap){
                AP_Init_error_observing_protocol::longmode_final_stack_frame_with_errcode* AP_frame=
                (AP_Init_error_observing_protocol::longmode_final_stack_frame_with_errcode*)
                longmode_enter_checkpoint.failure_final_stack_top;
                if(AP_frame->magic==AP_Init_error_observing_protocol::LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC){
                    bsp_kout<<now<<"[Longmode Exception Frame with Error Code]"<<kendl;
                    bsp_kout<<now<<"Magic: 0x"<<AP_frame->magic<<kendl;
                    bsp_kout<<now<<"IA32_EFER: 0x"<<AP_frame->IA32_EFER<<kendl;
                    bsp_kout<<now<<"GS: 0x"<<AP_frame->gs<<kendl;
                    bsp_kout<<now<<"FS: 0x"<<AP_frame->fs<<kendl;
                    bsp_kout<<now<<"SS: 0x"<<AP_frame->ss<<kendl;
                    bsp_kout<<now<<"DS: 0x"<<AP_frame->ds<<kendl;
                    bsp_kout<<now<<"ES: 0x"<<AP_frame->es<<kendl;
                    bsp_kout<<now<<"CR4: 0x"<<AP_frame->cr4<<kendl;
                    bsp_kout<<now<<"CR3: 0x"<<AP_frame->cr3<<kendl;
                    bsp_kout<<now<<"CR2: 0x"<<AP_frame->cr2<<kendl;
                    bsp_kout<<now<<"CR0: 0x"<<AP_frame->cr0<<kendl;
                    bsp_kout<<now<<"R15: 0x"<<AP_frame->r15<<kendl;
                    bsp_kout<<now<<"R14: 0x"<<AP_frame->r14<<kendl;
                    bsp_kout<<now<<"R13: 0x"<<AP_frame->r13<<kendl;
                    bsp_kout<<now<<"R12: 0x"<<AP_frame->r12<<kendl;
                    bsp_kout<<now<<"R11: 0x"<<AP_frame->r11<<kendl;
                    bsp_kout<<now<<"R10: 0x"<<AP_frame->r10<<kendl;
                    bsp_kout<<now<<"R9: 0x"<<AP_frame->r9<<kendl;
                    bsp_kout<<now<<"R8: 0x"<<AP_frame->r8<<kendl;
                    bsp_kout<<now<<"RDI: 0x"<<AP_frame->rdi<<kendl;
                    bsp_kout<<now<<"RSI: 0x"<<AP_frame->rsi<<kendl;
                    bsp_kout<<now<<"RBP: 0x"<<AP_frame->rbp<<kendl;
                    bsp_kout<<now<<"RDX: 0x"<<AP_frame->rdx<<kendl;
                    bsp_kout<<now<<"RCX: 0x"<<AP_frame->rcx<<kendl;
                    bsp_kout<<now<<"RBX: 0x"<<AP_frame->rbx<<kendl;
                    bsp_kout<<now<<"RAX: 0x"<<AP_frame->rax<<kendl;
                    bsp_kout<<now<<"Error Code: 0x"<<AP_frame->errcode<<kendl;
                    bsp_kout<<now<<"CS: 0x"<<AP_frame->cs<<kendl;
                    bsp_kout<<now<<"RIP: 0x"<<AP_frame->rip<<kendl;
                    bsp_kout<<now<<"RFLAGS: 0x"<<AP_frame->rflags<<kendl;
                }
            }else{
                AP_Init_error_observing_protocol::longmode_final_stack_frame* AP_frame=
                (AP_Init_error_observing_protocol::longmode_final_stack_frame*)
                longmode_enter_checkpoint.failure_final_stack_top;
                if(AP_frame->magic==AP_Init_error_observing_protocol::LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC){
                    bsp_kout<<now<<"[Longmode Exception Frame without Error Code]"<<kendl;
                    bsp_kout<<now<<"Magic: 0x"<<AP_frame->magic<<kendl;
                    bsp_kout<<now<<"IA32_EFER: 0x"<<AP_frame->IA32_EFER<<kendl;
                    bsp_kout<<now<<"GS: 0x"<<AP_frame->gs<<kendl;
                    bsp_kout<<now<<"FS: 0x"<<AP_frame->fs<<kendl;
                    bsp_kout<<now<<"SS: 0x"<<AP_frame->ss<<kendl;
                    bsp_kout<<now<<"DS: 0x"<<AP_frame->ds<<kendl;
                    bsp_kout<<now<<"ES: 0x"<<AP_frame->es<<kendl;
                    bsp_kout<<now<<"CR4: 0x"<<AP_frame->cr4<<kendl;
                    bsp_kout<<now<<"CR3: 0x"<<AP_frame->cr3<<kendl;
                    bsp_kout<<now<<"CR2: 0x"<<AP_frame->cr2<<kendl;
                    bsp_kout<<now<<"CR0: 0x"<<AP_frame->cr0<<kendl;
                    bsp_kout<<now<<"R15: 0x"<<AP_frame->r15<<kendl;
                    bsp_kout<<now<<"R14: 0x"<<AP_frame->r14<<kendl;
                    bsp_kout<<now<<"R13: 0x"<<AP_frame->r13<<kendl;
                    bsp_kout<<now<<"R12: 0x"<<AP_frame->r12<<kendl;
                    bsp_kout<<now<<"R11: 0x"<<AP_frame->r11<<kendl;
                    bsp_kout<<now<<"R10: 0x"<<AP_frame->r10<<kendl;
                    bsp_kout<<now<<"R9: 0x"<<AP_frame->r9<<kendl;
                    bsp_kout<<now<<"R8: 0x"<<AP_frame->r8<<kendl;
                    bsp_kout<<now<<"RDI: 0x"<<AP_frame->rdi<<kendl;
                    bsp_kout<<now<<"RSI: 0x"<<AP_frame->rsi<<kendl;
                    bsp_kout<<now<<"RBP: 0x"<<AP_frame->rbp<<kendl;
                    bsp_kout<<now<<"RDX: 0x"<<AP_frame->rdx<<kendl;
                    bsp_kout<<now<<"RCX: 0x"<<AP_frame->rcx<<kendl;
                    bsp_kout<<now<<"RBX: 0x"<<AP_frame->rbx<<kendl;
                    bsp_kout<<now<<"RAX: 0x"<<AP_frame->rax<<kendl;
                    bsp_kout<<now<<"CS: 0x"<<AP_frame->cs<<kendl;
                    bsp_kout<<now<<"RIP: 0x"<<AP_frame->rip<<kendl;
                    bsp_kout<<now<<"RFLAGS: 0x"<<AP_frame->rflags<<kendl;
                }
            }
        }
    };
    auto ap_init_stage_func=[](
        uint64_t delay_microseconds,
        ap_observe_once_result_t(*observe_ap)(uint32_t),//返回值0代表等待，返回值1代表成功，-1代表失败
        void(*failer_dealing)(),uint32_t success_word)->ap_observe_result_t{//启用全局错误码的情况下，0成功，1失败，2超时
        uint64_t now_microseconds = ktime::get_microsecond_stamp();//GS槽位里面会专门放每个核心的时间token的
        uint64_t ddline_stamp = now_microseconds + delay_microseconds;
        while(now_microseconds < ddline_stamp){
            int observe_ap_result= observe_ap(success_word);
            if(observe_ap_result==SUCCESS_TAHT_TIME)return CHECKPOINT_SUCCESS;
            if(observe_ap_result==FAIL_TAHT_TIME){
                failer_dealing();
                return CHECKPOINT_FAIL;}    
            
            now_microseconds = ktime::get_microsecond_stamp();
        }
        return CHECKPOINT_TIMEOUT;
    };
    uint64_t ipi_fai_count=0;
    // 遍历gAnalyzer的processor_x64_list链表
    for(auto it = gAnalyzer->processor_x64_list->begin(); it != gAnalyzer->processor_x64_list->end(); ++it) {
        APICtb_analyzed_structures::processor_x64_lapic_struct& proc = *it;
        // 跳过当前处理器（BSP）
        if(proc.apicid == self_x2apicid) continue;
        
        // 检查处理器是否启用（根据xAPIC或x2APIC类型）
        // 这里我们跳过检查is_bsp字段，因为我们要初始化所有AP
        
        icr_sipi.param.destination.raw = proc.apicid;
        assigned_processor_id=processor_id;
        asm volatile("sfence");
        bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one send sipi for "<<proc.apicid<<" processor"<<kendl;
        GfxPrim::Flush();
        x2apic::x2apic_driver::raw_send_ipi(icr_sipi);
        ap_observe_result_t status= ap_init_stage_func(1000,observe_realmode,nullptr,processor_id);//只有成功/超时两种状态
        if(status==CHECKPOINT_TIMEOUT){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one realmode enter timeout for processor "<<proc.apicid<<kendl;
            ipi_fai_count++;
        }
        status= ap_init_stage_func(1000,observe_pemode_enter,pe_fail_dealing,proc.apicid);
        if(status==CHECKPOINT_FAIL){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one pemode enter fail for processor "<<proc.apicid<<kendl;
            goto stage_fail;
        }
        if(status==CHECKPOINT_TIMEOUT){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one pemode enter timeout for processor "<<proc.apicid<<kendl;
            goto stage_fail;
        }
        status= ap_init_stage_func(1000,observe_longmode_enter,longmode_enter_fail_dealing,~processor_id);
        if(status==CHECKPOINT_FAIL){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one longmode enter fail for processor "<<proc.apicid<<kendl;
            goto stage_fail;
        }
        if(status==CHECKPOINT_TIMEOUT){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one longmode enter timeout for processor "<<proc.apicid<<kendl;
            goto stage_fail;
        }
        status= ap_init_stage_func(2000000,observe_finish,finish_fail_dealing,~proc.apicid);    
        if(status==CHECKPOINT_FAIL){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one finish stage fail for processor "<<proc.apicid<<kendl;
            goto stage_fail;
        }
        if(status==CHECKPOINT_TIMEOUT){
            bsp_kout<<now<<"[x64_local_processor]AP_Init_one_by_one finish stage timeout for processor "<<proc.apicid<<kendl;
            goto stage_fail;
        }
        processor_id++;

    }
    if(ipi_fai_count){
        success.result=result_code::PARTIAL_SUCCESS;
        success.reason=INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::PARTIAL_SUCCESS_CODE::PARTIAL_SUCCESS_CODE_SOME_APS_IPI_TIME_OUT;
    }
    return success;
    stage_fail:
    fatal.reason=INTERRUPT_SUB_MODULES_LOCATIONS::PROCESSORS_EVENT_CODE::APS_INIT_RESULTS_CODE::FATAL_REASON::AP_STAGE_FAIL;
    return fatal;

}