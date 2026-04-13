#include "arch/x86_64/core_hardwares/ioapic.h"
#include "arch/x86_64/core_hardwares/DMAR.h"
#include "memory/AddresSpace.h"
#include "panic.h"
#include "util/kout.h"
ioapic_driver *main_router;
ioapic_driver::ioapic_driver(APICtb_analyzed_structures::io_apic_structure *entry)
{
    KURD_t kurd;
    ioapic_regs_interval.pbase=entry->regbase_addr;
    ioapic_regs_interval.size=0x1000;
    ioapic_regs_interval.access={
        .is_kernel=true,
        .is_writeable=true,
        .is_readable=true,
        .is_executable=false,
        .is_global=true,
        .cache_strategy=UC
    };
    head=(regs_head*)phyaddr_direct_map(&ioapic_regs_interval,&kurd);
    belonged_dmar=dmar::dmar_table[dmar::special_locations[dmar::ioapic_idx].dmar_id];
    if(error_kurd(kurd)){
        panic_info_inshort info={
            .is_bug=false,
            .is_policy=true,
            .is_hw_fault=false,
            .is_mem_corruption=false,
            .is_escalated=false
        };
        Panic::panic(default_panic_behaviors_flags,"IOAPIC: unable to map IOAPIC registers",nullptr,&info,kurd);
    }
    
    // 初始化完成，保存 IOAPIC ID
    ioapic_id = entry->ioapic_id;
    
    // 读取并打印版本信息
    head->select_reg = 0x01;  // 选择版本寄存器
    uint32_t version_info = head->window_reg;
    uint8_t version = version_info & 0xFF;  // 低8位是版本号
    uint8_t max_redirection_entries = ((version_info >> 16) & 0xFF) + 1;  // 高16-23位是最大条目数（需要+1）
    max_rte_num=max_redirection_entries;
    bsp_kout << "[IOAPIC] Initialized - ID: " << (uint32_t)ioapic_id 
             << ", Version: " << (uint32_t)version 
             << ", Max Redirection Entries: " << (uint32_t)max_redirection_entries 
             << kendl;
    
    // 屏蔽所有中断管脚
    for(uint8_t i = 0; i < max_redirection_entries; i++) {
        head->select_reg = 0x10 + (i * 2);  // 选择重定向表条目（低32位）
        head->window_reg = 0x00010000;  // 设置 Mask 位（bit 16），其他位清零
        // 高32位保持为0（默认目标 LAPIC ID 为 0，delivery mode 为 Fixed）
    }
    
    bsp_kout << "[IOAPIC] All " << (uint32_t)max_redirection_entries << " interrupt pins masked" << kendl;
}

uint64_t ioapic_driver::get_rte_raw(uint8_t rte)
{
    // 边界检查
    if(rte >= max_rte_num) {
        bsp_kout << "[WARN] IOAPIC: get_rte_raw - RTE index " << (uint32_t)rte 
                 << " exceeds max " << (uint32_t)max_rte_num << kendl;
        return 0;
    }
    
    // 读取低32位
    head->select_reg = 0x10 + (rte * 2);  // RTE 起始索引为 0x10，每个 RTE 占 2 个寄存器
    uint32_t low = head->window_reg;
    
    // 读取高32位
    head->select_reg = 0x10 + (rte * 2) + 1;
    uint32_t high = head->window_reg;
    
    // 组合成 64 位值
    return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
}

void ioapic_driver::set_rte_raw(uint8_t rte, uint64_t value)
{
    // 边界检查
    if(rte >= max_rte_num) {
        bsp_kout << "[WARN] IOAPIC: set_rte_raw - RTE index " << (uint32_t)rte 
                 << " exceeds max " << (uint32_t)max_rte_num << kendl;
        return;
    }
    
    // 分离高低32位
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFF);
    uint32_t high = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
    
    // 写入高32位（先写高位，再写低位，确保原子性）
    head->select_reg = 0x10 + (rte * 2) + 1;
    head->window_reg = high;
    
    // 写入低32位
    head->select_reg = 0x10 + (rte * 2);
    head->window_reg = low;
}

KURD_t ioapic_driver::irq_regist(uint8_t rte, uint16_t remmap_idx,bool polarity)
{   
    using namespace COREHARDWARES_LOCATIONS::IO_APIC_DRIVERS_EVENTS::RTE_REGIST_RESULTS;
    KURD_t fail=default_fail;
    KURD_t success=default_success;
    fail.event_code=COREHARDWARES_LOCATIONS::IO_APIC_DRIVERS_EVENTS::RTE_REGIST;
    success.event_code=COREHARDWARES_LOCATIONS::IO_APIC_DRIVERS_EVENTS::RTE_REGIST;
    if(rte>=max_rte_num||
    remmap_idx*sizeof(dmar::irte)>=dmar::interrupt_remapp_table_default_size)
    {
        fail.reason=FAIL_RESULTS::FAIL_BAD_PARAM;
        return fail;
    }
    dmar::irte copy=dmar::dmar_table[dmar::special_locations[dmar::ioapic_idx].dmar_id]->get_interrupt_remmaptable()[remmap_idx];
    RTE_remmap_union entry={.value=0};
    entry.filed.vector = copy.vec;
    entry.filed.delivery_mode = 0;
    entry.filed.index_15 = !!(remmap_idx & 0x8000);
    entry.filed.pin_polarity = polarity;
    entry.filed.trigger_mode = copy.trigger_mode;
    entry.filed.mask = 0;
    entry.filed.reserved = 0;
    entry.filed.interrupt_format = 1;
    entry.filed.remmap_idx_0_14 = static_cast<uint64_t>(remmap_idx) & 0x7fffULL;
    // 设置 RTE
    set_rte_raw(rte, entry.value);
    return success;
}
KURD_t ioapic_driver::irq_unregist(uint8_t rte)
{
    using namespace COREHARDWARES_LOCATIONS::IO_APIC_DRIVERS_EVENTS::RTE_REGIST_RESULTS;
    KURD_t fail=default_fail;
    KURD_t success=default_success;
    fail.event_code=COREHARDWARES_LOCATIONS::IO_APIC_DRIVERS_EVENTS::RTE_UNREGIST;
    success.event_code=COREHARDWARES_LOCATIONS::IO_APIC_DRIVERS_EVENTS::RTE_UNREGIST;
    if(rte>=max_rte_num)
    {
        fail.reason=FAIL_RESULTS::FAIL_BAD_PARAM;
        return fail;
    }
    set_rte_raw(rte, 0x10000);
    return success;
}