#include "arch/x86_64/core_hardwares/DMAR.h"
#include "memory/all_pages_arr.h"
#include "ktime.h"
#include "panic.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "util/kout.h"
#include "global_controls.h"
uint8_t dmar::iommu_fault_alloced_vector;
extern "C" char iommu_fault_deal;
 dmar::driver** dmar::dmar_table;
    uint32_t dmar::dmars_count;
    uint32_t dmar::main_dmar_id;

    dmar::special_location* dmar::special_locations;
    uint32_t dmar::special_locations_count;
    uint32_t dmar::ioapic_idx;//只会支持一个ioapic,用于给legacy设备，其它完全走msi/msix
    uint32_t dmar::hpet_idx;
extern "C" void iommu_fault_cpp_enter(x64_standard_context*regs){
    for(uint32_t i=0;i<dmar::dmars_count;i++){
        dmar::dmar_table[i]->err_handle();
    }
}
int dmar::Init(acpi::DMAR_head *head)
{
    if(is_iremap_try)
    {if(head==nullptr){
        return OS_INVALID_PARAMETER;
    }
    if(!head->flag_interrupt_remap_enable){
        return OS_NOT_SUPPORT;
    }

    dmar_table = nullptr;
    dmars_count = 0;
    main_dmar_id = ~0u;
    special_locations = nullptr;
    special_locations_count = 0;

    const uint8_t* dmar_begin = reinterpret_cast<const uint8_t*>(head);
    const uint8_t* cur = dmar_begin + sizeof(acpi::DMAR_head);
    const uint8_t* end = dmar_begin + head->Header.Length;
    if (end < cur) {
        return OS_BAD_FUNCTION;
    }
    x64_local_processor*self=(x64_local_processor*)read_gs_u64(PROCESSOR_SELF_RESOURCES_COMPELX_GS_INDEX);
    dmar::iommu_fault_alloced_vector=self->handler_alloc(&iommu_fault_deal);
    if(dmar::iommu_fault_alloced_vector==0xff){

        Panic::panic(default_panic_behaviors_flags,"DMAR:iommu vec regist fault",
            nullptr,
            nullptr,
            KURD_t()
        );
    }
    uint32_t drhd_count = 0;
    uint32_t scope_total = 0;

    // Pass 1: 统计 DRHD 数量与全部 scope 数量
    while (cur + sizeof(acpi::sub_table_head) <= end) {
        const acpi::sub_table_head* sub = reinterpret_cast<const acpi::sub_table_head*>(cur);
        if (sub->length < sizeof(acpi::sub_table_head) || cur + sub->length > end) {
            return OS_BAD_FUNCTION;
        }
        if (sub->table_type == acpi::sub_table_type::DRHD) {
            if (sub->length < sizeof(acpi::DRHD_table)) {
                return OS_BAD_FUNCTION;
            }
            drhd_count++;

            const uint8_t* scope_cur = cur + sizeof(acpi::DRHD_table);
            const uint8_t* scope_end = cur + sub->length;
            while (scope_cur + sizeof(acpi::device_scope) <= scope_end) {
                const acpi::device_scope* scope = reinterpret_cast<const acpi::device_scope*>(scope_cur);
                if (scope->length < sizeof(acpi::device_scope) || scope_cur + scope->length > scope_end) {
                    break;
                }
                if (scope->length >= sizeof(acpi::device_scope_simp)) {
                    scope_total++;
                }
                scope_cur += scope->length;
            }
        }
        cur += sub->length;
    }

    if (drhd_count == 0) {
        return OS_NOT_EXIST;
    }

    dmar_table = new driver*[drhd_count];
    if (dmar_table == nullptr) {
        return OS_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < drhd_count; ++i) {
        dmar_table[i] = nullptr;
    }

    if (scope_total > 0) {
        special_locations = new special_location[scope_total];
        if (special_locations == nullptr) {
            return OS_OUT_OF_MEMORY;
        }
    }

    // Pass 2: 构造 driver 并填表
    cur = dmar_begin + sizeof(acpi::DMAR_head);
    uint32_t dmar_idx = 0;
    while (cur + sizeof(acpi::sub_table_head) <= end) {
        const acpi::sub_table_head* sub = reinterpret_cast<const acpi::sub_table_head*>(cur);
        if (sub->length < sizeof(acpi::sub_table_head) || cur + sub->length > end) {
            return OS_BAD_FUNCTION;
        }
        if (sub->table_type == acpi::sub_table_type::DRHD) {
            acpi::DRHD_table* drhd = reinterpret_cast<acpi::DRHD_table*>(const_cast<uint8_t*>(cur));
            dmar_table[dmar_idx] = new driver(drhd);
            if (drhd->flag_INCLUDE_PCI_ALL) {
                main_dmar_id = dmar_idx;
            }

            const uint8_t* scope_cur = cur + sizeof(acpi::DRHD_table);
            const uint8_t* scope_end = cur + sub->length;
            while (scope_cur + sizeof(acpi::device_scope) <= scope_end) {
                const acpi::device_scope* scope = reinterpret_cast<const acpi::device_scope*>(scope_cur);
                if (scope->length < sizeof(acpi::device_scope) || scope_cur + scope->length > scope_end) {
                    break;
                }
                if (scope->length >= sizeof(acpi::device_scope_simp) && special_locations != nullptr) {
                    const acpi::device_scope_simp* simp =
                        reinterpret_cast<const acpi::device_scope_simp*>(scope_cur);
                    special_location& slot = special_locations[special_locations_count++];
                    slot.location.segment_num = drhd->pcie_seg_number;
                    slot.location.bus_num = simp->start_bus_num;
                    slot.location.device_num = static_cast<uint8_t>(simp->device_num & 0x1F);
                    slot.location.func_num = static_cast<uint8_t>(simp->func_num & 0x07);
                    slot.dmar_id = dmar_idx;
                }
                scope_cur += scope->length;
            }
            dmar_idx++;
        }
        cur += sub->length;
    }

    dmars_count = dmar_idx;
    return OS_SUCCESS;
    }else{
        return OS_SUCCESS;
    }
}
dmar::driver::driver(acpi::DRHD_table *drhd)
{
    KURD_t kurd;
    KURD_t fatal=default_fatal;
    default_fatal.result=COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::INIT;
    using namespace COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::INIT_RESULTS;
    panic_info_inshort info={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
    this->drhd=drhd;
    regs_interval.access={
        .is_kernel=1,
        .is_writeable=1,
        .is_readable=1,
        .is_executable=false,
        .is_global=1,
        .cache_strategy=UC
    };
    regs_interval.pbase=drhd->register_base_addr;
    regs_interval.size=(1<<(drhd->mmio_size_specify+12));
    regs_vbase=(head_regs*)phyaddr_direct_map(&regs_interval,&kurd);
    if(error_kurd(kurd)){
        
        Panic::panic(default_panic_behaviors_flags,"DMAR:regs_vbase map fail",
            nullptr,
            &info,
            kurd
        );
    }
    uint64_t extend_cap =regs_vbase->extend_regs;
    uint64_t check_caps= extend_cap&regs_specify::extended_caps_specify::mask_smallest_cap_set;
    if(check_caps!=regs_specify::extended_caps_specify::mask_smallest_cap_set){
        fatal.reason=FATAL_REASONS::CAP_DIDNT_MET_LOWEST_DEMAND;
        bsp_kout<<"check_caps:0x"<<check_caps<<" extend_cap:0x"<<extend_cap<<"\n";
        Panic::panic(default_panic_behaviors_flags,"DMAR:didnt meet lowest demand",
            nullptr,
            &info,
            fatal
        );
    }
    interrupt_remmaptable=(irte*)__wrapped_pgs_valloc(&kurd,interrupt_remapp_table_default_size/4096,page_state_t::kernel_pinned,12);
    interrupt_remmaptable_interval.vbase=(vaddr_t)interrupt_remmaptable;
    kurd=KspacePageTable::v_to_phyaddrtraslation(interrupt_remmaptable_interval.vbase,interrupt_remmaptable_interval.pbase);
    if(error_kurd(kurd)){
        Panic::panic(default_panic_behaviors_flags,"DMAR:iremap_table reverse map fail",
            nullptr,
            &info,
            kurd
        );
    }
    interrupt_remmaptable_interval.size=interrupt_remapp_table_default_size;
    ksetmem_64(interrupt_remmaptable,0,interrupt_remmaptable_interval.size);
    uint64_t iremap_ptr_reg_draft=interrupt_remmaptable_interval.pbase|regs_specify::imap_tbptr_reg_specify::mask_iremap_x2apic_support|interrupt_remapp_table_default_S;
    regs_vbase->Interrupt_remapping_table_addr=iremap_ptr_reg_draft;
    uint32_t global_command_draft=regs_specify::global_command_status_specify::mask_interrupt_remap_enable|
    regs_specify::global_command_status_specify::mask__set_iremapp_tbptr;
    regs_vbase->global_command=global_command_draft;
    ktime::microsecond_polling_delay(1000);
    uint32_t global_status=regs_vbase->global_status;
    bool test1=!!(global_status&regs_specify::global_command_status_specify::mask_interrupt_remap_enable);
    bool test2=!!(global_status&regs_specify::global_command_status_specify::mask__set_iremapp_tbptr);
    bool test3=!(global_status&regs_specify::global_command_status_specify::mask_compatibility_format_interrupt);
    if((test1&&test2&&test3)==false){
        fatal.reason=FATAL_REASONS::TIME_OUT_COMMAND_SET;
        Panic::panic(default_panic_behaviors_flags,"DMAR:time out command set",
            nullptr,
            &info,
            fatal
        );
    }
    uint32_t bsp_x2apicid=query_x2apicid();
    uint32_t fault_event_data_draft=dmar::iommu_fault_alloced_vector;
    regs_vbase->fault_event_data=fault_event_data_draft;
    uint8_t bsp_apicid_low=bsp_x2apicid&0xff;
    uint32_t fault_event_addr_low_draft=0xfee00000|(1<<3)|(bsp_apicid_low<<12);
    regs_vbase->fault_event_addr=fault_event_addr_low_draft;
    uint32_t bsp_x2apicid_high24bits=bsp_x2apicid&0xffffff00;
    uint32_t fault_event_addr_high_draft=bsp_x2apicid_high24bits;
    regs_vbase->fault_event_addr_high=fault_event_addr_high_draft;
    head_regs::fault_event_control_union union_draft={.value=regs_vbase->fault_event_control};
    union_draft.fields.IM=0;
    regs_vbase->fault_event_control=union_draft.value;
}
KURD_t dmar::driver::regist_interrupt_simp(regist_remmap_struct arg, uint16_t &idx)
{
    KURD_t fail=default_err;
    KURD_t success=default_success;
    using namespace COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY_RESULTS;
    fail.event_code=COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY;
    success.event_code=COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY;
    uint32_t entry_max=interrupt_remapp_table_default_size/sizeof(irte);
    uint32_t i=0;
    for(;i<entry_max;i++){
        if(interrupt_remmaptable[i].present==0)break;
    }
    if(i==entry_max){
        fail.reason=FAIL_REASONS::FAIL_NO_AVALIABLE_ENTRY;
        return fail;
    }
    irte& entry=interrupt_remmaptable[i];
    entry.IRTE_mode=0;
    entry.delivery_mode=arg.delivery_mode;
    entry.destination=arg.destination;
    entry.destination_mode=arg.destination_mode;
    entry.redirection_hint=arg.redirection_hint;
    entry.trigger_mode=arg.trigger_mode;
    entry.vec=arg.vec;
    entry.fpd=1;
    entry.source_id=arg.location.bus_num<<8|arg.location.device_num<<3|arg.location.func_num;
    entry.source_qualifier=0;
    entry.source_validation_type=2;
    entry.reserved1=0;
    entry.reserved2=0;
    entry.reserved3=0;
    entry.present=1;
    idx = static_cast<uint16_t>(i);
    // 仅强制该 IRTE 所在缓存行写回，避免全局 wbinvd 的高代价
    asm volatile("clflush (%0)" :: "r"(&entry) : "memory");
    asm volatile("sfence" ::: "memory");
    return success;
}
KURD_t dmar::regist_interrupt_simp(regist_remmap_struct arg, uint16_t &idx, uint32_t &dmar_id)
{
    KURD_t fail = KURD_t(
        result_code::FAIL,
        0,
        module_code::DEVICES_CORE,
        COREHARDWARES_LOCATIONS::LOCATION_CODE_DMAR,
        COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY,
        level_code::ERROR,
        err_domain::ARCH
    );
    using namespace COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY_RESULTS;

    if (dmar_table == nullptr || dmars_count == 0) {
        fail.reason = FAIL_REASONS::FAIL_NO_AVALIABLE_ENTRY;
        return fail;
    }

    uint32_t selected_id = ~0u;
    for (uint32_t i = 0; i < special_locations_count; ++i) {
        const special_location& s = special_locations[i];
        if (s.location.segment_num == arg.location.segment_num &&
            s.location.bus_num == arg.location.bus_num &&
            s.location.device_num == arg.location.device_num &&
            s.location.func_num == arg.location.func_num) {
            selected_id = s.dmar_id;
            break;
        }
    }

    if (selected_id == ~0u) {
        selected_id = main_dmar_id;
    }
    if (selected_id >= dmars_count || dmar_table[selected_id] == nullptr) {
        fail.reason = FAIL_REASONS::FAIL_NO_AVALIABLE_ENTRY;
        return fail;
    }

    dmar_id = selected_id;
    return dmar_table[selected_id]->regist_interrupt_simp(arg, idx);
}
KURD_t dmar::driver::err_handle()
{
    bsp_kout<<"DMAR: error detect"<<kendl;
    return KURD_t();
}
uint32_t legacy_rotate_interrupt_alloc_id;