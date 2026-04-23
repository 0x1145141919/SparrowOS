#include "arch/x86_64/core_hardwares/DMAR.h"
#include "memory/all_pages_arr.h"
#include "ktime.h"
#include "panic.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "arch/x86_64/core_hardwares/lapic.h"
#include "util/kout.h"
#include "global_controls.h"
#include "memory/FreePagesAllocator.h"
#include "memory/phyaddr_accessor.h"
uint8_t dmar::iommu_fault_alloced_vector;
extern "C" char iommu_fault_deal;
dmar::driver** dmar::dmar_table;
uint32_t dmar::dmars_count;
uint32_t dmar::main_dmar_id;
dmar::special_location* dmar::special_locations;
uint32_t dmar::special_locations_count;
uint32_t dmar::ioapic_idx;//只会支持一个ioapic,用于给legacy设备，其它完全走msi/msix
uint32_t dmar::hpet_idx;
void flush_cache_serial(vaddr_t addr){
    asm volatile("clflush %0": :"m"(addr));
}
void flush_cache_serial_no(vaddr_t addr){
    asm volatile("clflushopt %0": :"m"(addr));
}
extern "C" void iommu_fault_cpp_enter(x64_standard_context*regs){
    for(uint32_t i=0;i<dmar::dmars_count;i++){
        dmar::dmar_table[i]->err_handle();
    }
    x2apic::x2apic_driver::write_eoi();
}
KURD_t dmar::driver::device_regist(pcie_location location)
{
    if(location.segment_num!=drhd->pcie_seg_number){
        return default_err;
    }
    KURD_t kurd;
    phyaddr_t context_table_base;
    translation_structs::legacy_root_entry&root=legacy_root_entry[location.bus_num];
    if(!root.field.present){
        context_table_base=
        FreePagesAllocator::alloc(
            0x1000,
            BUDDY_ALLOC_ALWAYS_TRY,
            page_state_t::kernel_pinned,
            kurd
        );
        if(error_kurd(kurd)){
            return kurd;
        }
        translation_structs::legacy_root_entry entry{};
        entry.field.present=1;
        entry.unit[0]|=context_table_base;
        //根表直接用legacy_root_entry的虚拟地址数据结构
        root=entry;
        flush_cache_serial_no(vaddr_t(legacy_root_entry+location.bus_num));
    }else{
        context_table_base=root.unit[0]&(~1ull);
    }
    phyaddr_t context_entry_base=context_table_base+((location.device_num<<3)+location.func_num)*sizeof(translation_structs::legacy_context_entry);
    uint16_t did=(((location.bus_num)<<8)+(location.device_num<<3)+(location.func_num));
    translation_structs::legacy_context_entry entry{};
    entry.field.present=1;
    entry.field.FPD=0;
    entry.field.translation_type=2;
    entry.field.reserved=0;
    entry.field.addr_width=0;
    entry.field.reserved1=0;
    entry.field.domain_identifier=0;
    entry.field.reserved2=0;
    //相关设置
    //用PhyAddrAccessor写入并刷掉缓存行
    PhyAddrAccessor::writeu64(context_entry_base,entry.unit[0]);
    PhyAddrAccessor::writeu64(context_entry_base+sizeof(uint64_t),entry.unit[1]);
    PhyAddrAccessor::cache_flush_serial(context_entry_base);
    return default_success;
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
dmar::driver::driver(acpi::DRHD_table *drhd)//未完全完成之DMA重映射之根表的初始化
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
    regs=(head_regs*)phyaddr_direct_map(&regs_interval,&kurd);
    if(error_kurd(kurd)){
        
        Panic::panic(default_panic_behaviors_flags,"DMAR:regs_vbase map fail",
            nullptr,
            &info,
            kurd
        );
    }
    head_regs::cap_reg_union cap={
        .value=regs->cap_regs
    };
    fault_record_regs_count=cap.fields.NFR+1;
    fault_regs_bases=(fault_record_reg_raw*)((vaddr_t)regs+(cap.fields.FRO<<4));
    uint64_t extend_cap =regs->extend_regs;
    uint64_t check_caps= extend_cap&regs_specify::extended_caps_specify::mask_smallest_cap_set;
    bsp_kout<<"check_caps:0x"<<HEX<<check_caps<<" extend_cap:0x"<<extend_cap<<DEC<<kendl;
    if(check_caps!=regs_specify::extended_caps_specify::mask_smallest_cap_set){
        fatal.reason=FATAL_REASONS::CAP_DIDNT_MET_LOWEST_DEMAND;
        Panic::panic(default_panic_behaviors_flags,"DMAR:didnt meet lowest demand",
            nullptr,
            &info,
            fatal
        );
    }
    interrupt_remmaptable=(translation_structs::irte*)__wrapped_pgs_valloc(&kurd,interrupt_remapp_table_default_size/4096,page_state_t::kernel_pinned,12);
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
    legacy_root_entry=(translation_structs::legacy_root_entry*)__wrapped_pgs_valloc(&kurd,1,page_state_t::kernel_pinned,12);
    if(error_kurd(kurd)){
        Panic::panic(default_panic_behaviors_flags,"DMAR:root_entry alloc fail",
            nullptr,
            &info,
            kurd
        );
    }
    phyaddr_t root_table_phybase;
    kurd=KspacePageTable::v_to_phyaddrtraslation((vaddr_t)legacy_root_entry,root_table_phybase);
    if(error_kurd(kurd)){
        Panic::panic(default_panic_behaviors_flags,"DMAR:root_entry reverse map fail",
            nullptr,
            &info,
            kurd
        );
    }
    uint64_t root_table_reg_draft=root_table_phybase;
    command_disable_traslation();
    set_command_disable_iremap();
    uint64_t iremap_ptr_reg_draft=interrupt_remmaptable_interval.pbase|interrupt_remapp_table_default_S;
    __sync_synchronize();
    regs->root_table=root_table_reg_draft;

    //传入的drhd开始解析下面的scope(只考虑用device_scope_simp简单解析，非device_scope_simp跳过),对应的scope调用device_regist
    {
        const uint8_t* scope_cur=reinterpret_cast<const uint8_t*>(drhd)+sizeof(acpi::DRHD_table);
        const uint8_t* scope_end=reinterpret_cast<const uint8_t*>(drhd)+drhd->head.length;
        while(scope_cur+sizeof(acpi::device_scope)<=scope_end){
            const acpi::device_scope* scope=reinterpret_cast<const acpi::device_scope*>(scope_cur);
            if(scope->length<sizeof(acpi::device_scope)||scope_cur+scope->length>scope_end){
                break;
            }
            if(scope->length>=sizeof(acpi::device_scope_simp)){
                const acpi::device_scope_simp* simp=
                    reinterpret_cast<const acpi::device_scope_simp*>(scope_cur);
                pcie_location loc={
                    .segment_num=drhd->pcie_seg_number,
                    .bus_num=simp->start_bus_num,
                    .device_num=static_cast<uint8_t>(simp->device_num&0x1f),
                    .func_num=static_cast<uint8_t>(simp->func_num&0x07)
                };
                kurd=device_regist(loc);
                if(error_kurd(kurd)){
                    Panic::panic(default_panic_behaviors_flags,"DMAR:device_regist fail",
                        nullptr,
                        &info,
                        kurd
                    );
                }
            }
            scope_cur+=scope->length;
        }
    }
    command_enable_root_table();
    regs->Interrupt_remapping_table_addr=iremap_ptr_reg_draft;
    set_command_set_iremap_tbptr();
    set_command_enable_iremap();
    command_enable_traslation();
    uint32_t bsp_x2apicid=query_x2apicid();
    uint32_t fault_event_data_draft=dmar::iommu_fault_alloced_vector;
    regs->fault_event_data=fault_event_data_draft;
    uint8_t bsp_apicid_low=bsp_x2apicid&0xff;
    uint32_t fault_event_addr_low_draft=0xfee00000|(1<<3)|(bsp_apicid_low<<12);
    regs->fault_event_addr=fault_event_addr_low_draft;
    uint32_t bsp_x2apicid_high24bits=bsp_x2apicid&0xffffff00;
    uint32_t fault_event_addr_high_draft=bsp_x2apicid_high24bits;
    regs->fault_event_addr_high=fault_event_addr_high_draft;
    head_regs::fault_event_control_union union_draft={.value=regs->fault_event_control};
    union_draft.fields.IM=0;
    regs->fault_event_control=union_draft.value;
}
KURD_t dmar::driver::regist_interrupt_simp(regist_remmap_struct arg, uint16_t &idx)
{
    KURD_t fail=default_err;
    KURD_t success=default_success;
    using namespace COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY_RESULTS;
    fail.event_code=COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY;
    success.event_code=COREHARDWARES_LOCATIONS::DMAR_DRIVERS_EVENTS::ALLOC_ENTRY;
    uint32_t entry_max=interrupt_remapp_table_default_size/sizeof(translation_structs::irte);
    uint32_t i=0;
    for(;i<entry_max;i++){
        if(interrupt_remmaptable[i].present==0)break;
    }
    if(i==entry_max){
        fail.reason=FAIL_REASONS::FAIL_NO_AVALIABLE_ENTRY;
        return fail;
    }
    translation_structs::irte& entry=interrupt_remmaptable[i];
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
    head_regs::fault_status_union status={
        .value=regs->fault_status
    };
    if(status.fields.PPF){
        if(fault_record_regs_count==0||fault_regs_bases==nullptr){
            return KURD_t();
        }
        uint32_t fail_tail_index=status.fields.FRI%fault_record_regs_count;
        uint32_t cur_idx=fail_tail_index;

        // 从 fail_tail_index 开始向下环形遍历一圈，最多处理 fault_record_regs_count 个条目
        for(uint32_t walked=0;walked<fault_record_regs_count;walked++){
            fault_record record={.raw=fault_regs_bases[cur_idx]};
            if(record.fields.F){
                bsp_kout<<"DMAR fault idx:"<<cur_idx
                        <<" sid:0x"<<HEX<<record.fields.SID
                        <<" reason:0x"<<record.fields.FAULT_REASON
                        <<" fi:0x"<<record.fields.FI<<DEC<<kendl;

                // F 位按规范为 RW1C：写 1 清故障
                fault_record clear_record=record;
                clear_record.fields.F=1;
                fault_regs_bases[cur_idx]=clear_record.raw;
                asm volatile("mfence":::"memory");
            }

            status.value=regs->fault_status;
            if(status.fields.PPF==0){
                regs->fault_status=status.value;
                break;
            }
            cur_idx=(cur_idx==0)?(fault_record_regs_count-1):(cur_idx-1);
        }
    }
    return KURD_t();
}
uint32_t legacy_rotate_interrupt_alloc_id;
void dmar::driver::set_command_enable_iremap()
{
    uint32_t global_command_draft=regs->global_status;
    global_command_draft&=0x96FFFFFF;
    global_command_draft|=regs_specify::global_command_status_specify::mask_interrupt_remap_enable;
    asm volatile("mfence":::"memory");
    regs->global_command=global_command_draft;
    while(true){
        uint32_t status=regs->global_status;
        if(status&regs_specify::global_command_status_specify::mask_interrupt_remap_enable){
            break;
        }
        asm volatile("pause" ::: "memory");
    }
}
void dmar::driver::set_command_disable_compatiable_format_interrupt()
{
    uint32_t global_command_draft = regs->global_status;
    global_command_draft &= 0x96FFFFFF;
    global_command_draft &= (~regs_specify::global_command_status_specify::mask_compatibility_format_interrupt);
    asm volatile("mfence" ::: "memory");
    regs->global_command = global_command_draft;
    while(regs->global_status & 
          regs_specify::global_command_status_specify::mask_compatibility_format_interrupt) {
        asm volatile("pause" ::: "memory");
    }
}
void dmar::driver::set_command_disable_iremap()
{
    uint32_t global_command_draft = regs->global_status;
    global_command_draft &= 0x96FFFFFF;
    global_command_draft &= (~regs_specify::global_command_status_specify::mask_interrupt_remap_enable);
    asm volatile("mfence" ::: "memory");
    regs->global_command = global_command_draft;
    
    // 等待 IRES = 0（注意：没有 !）
    while(regs->global_status & 
          regs_specify::global_command_status_specify::mask_interrupt_remap_enable) {
        asm volatile("pause" ::: "memory");
    }
}
void dmar::driver::set_command_set_iremap_tbptr()
{
    uint32_t global_command_draft=regs->global_status;
    global_command_draft&=0x96FFFFFF;
    global_command_draft|=regs_specify::global_command_status_specify::mask__set_iremapp_tbptr;
    asm volatile("mfence":::"memory");
    regs->global_command=global_command_draft;
    while(true){
        uint32_t status=regs->global_status;
        if(status&regs_specify::global_command_status_specify::mask__set_iremapp_tbptr){
            break;
        }
        asm volatile("pause" ::: "memory");
    }
}
void dmar::driver::command_disable_traslation()
{
    uint32_t global_command_draft=regs->global_status;
    global_command_draft&=0x96FFFFFF;
    global_command_draft&=(~regs_specify::global_command_status_specify::mask_dma_remapping_enable);
    asm volatile("mfence":::"memory");
    regs->global_command=global_command_draft;
    while(regs->global_status&
          regs_specify::global_command_status_specify::mask_dma_remapping_enable){
        asm volatile("pause" ::: "memory");
    }
}
void dmar::driver::command_enable_traslation()
{
    uint32_t global_command_draft=regs->global_status;
    global_command_draft&=0x96FFFFFF;
    global_command_draft|=regs_specify::global_command_status_specify::mask_dma_remapping_enable;
    asm volatile("mfence":::"memory");
    regs->global_command=global_command_draft;
    while((regs->global_status&
          regs_specify::global_command_status_specify::mask_dma_remapping_enable)==0){
        asm volatile("pause" ::: "memory");
    }
}
void dmar::driver::command_enable_root_table()
{
    uint32_t global_command_draft=regs->global_status;
    global_command_draft&=0x96FFFFFF;
    global_command_draft|=regs_specify::global_command_status_specify::mask_set_root_table_ptr;
    asm volatile("mfence":::"memory");
    regs->global_command=global_command_draft;
    while((regs->global_status&
          regs_specify::global_command_status_specify::mask_set_root_table_ptr)==0){
        asm volatile("pause" ::: "memory");
    }
}
