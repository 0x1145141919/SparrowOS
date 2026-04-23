#include <arch/x86_64/core_hardwares/i8042.h>
#include <arch/x86_64/core_hardwares/ioapic.h>
#include <arch/x86_64/core_hardwares/lapic.h>
#include <arch/x86_64/core_hardwares/DMAR.h>
#include <arch/x86_64/Interrupt_system/loacl_processor.h>
#include <util/kout.h>
#include <panic.h>
#include <global_controls.h>
extern "C" char i8042_code_deal;
extern "C" char i8042_fault_deal;
u16ka scan_code_buffer_tail_idx;
u8ka analyzed_buffer_tail_idx;
union led_status {
    uint8_t raw;
    struct {
        uint8_t scrolllock:1;
        uint8_t numlock:1;
        uint8_t capslock:1;
        uint8_t res:5;
    }field;
};
alignas(4096) uint8_t scan_code_buffer[4096];
tid_wait_queue* i8042_scancode_buffer_subscriber_queue;
tid_wait_queue* i8042_analyzed_buffer_subscriber_queue;
led_status key_board_led;
const char* scancode_to_key(uint8_t scancode);

void wait_until_in_buff_clear(){
    while(inb(0x64)&0x2);
}
void led_set(){
    wait_until_in_buff_clear();
    outb(0xed,0x60);
    uint8_t ack;
    wait_until_in_buff_clear();
    if(ack!=0xfa){
        return;
    }
    wait_until_in_buff_clear();
    outb(key_board_led.raw,0x60);
    wait_until_in_buff_clear();
    if(ack!=0xfa){
        return;
    }
}
extern "C" void i8042_cpp_enter(x64_standard_context* frame){
    uint8_t scancode= inb(0x60);
    if(scancode==0x3a){
        key_board_led.raw^=4;
        led_set();
    }else  if(scancode==0x45){
        key_board_led.raw^=2;
        led_set();
    }
    bsp_kout<<"i8042 cpp enter: "<<HEX<<scancode<<kendl;
    uint16_t tail=scan_code_buffer_tail_idx.load();
    tail++;
    tail/=i8042_buffer_max_size;
    scan_code_buffer_tail_idx.store(tail);
    scan_code_buffer[tail]=scancode;
    if(GlobalKernelStatus>=SCHEDUL_READY)
    {
        spinlock_cpp_t lock(i8042_scancode_buffer_subscriber_queue->lock);
        i8042_scancode_buffer_subscriber_queue->wakeup_all();
    }
    x2apic::x2apic_driver::write_eoi();
}
void*scancode_analyze_kthread(void*not_use){
    uint16_t idx;
    while(true){
        idx=scan_code_buffer_tail_idx.load();
        block_queue(i8042_scancode_buffer_subscriber_queue);
        
    }
}
//然后有两个VM_interval,只读的暴露给外界，读写的给中断handler 




void i8042_interrupt_enable(){
    x64_local_processor* manage=x86_smp_processors_container::get_processor_mgr_by_processor_id(++legacy_rotate_interrupt_alloc_id);
    uint32_t target_apicid=manage->get_apic_id();
    uint8_t vec= manage->handler_alloc((void*)&i8042_code_deal);
    if(vec==0xff){
        //panic
    }
    KURD_t kurd;
    if(is_iremap_try)
    {
    pcie_location ioapic_ioapic_location=dmar::special_locations[dmar::ioapic_idx].location;
    dmar::regist_remmap_struct arg={
        .location=ioapic_ioapic_location,
        .vec=vec,
        .delivery_mode=dmar::interrupt_mode_type_t::fixed,
        .destination=target_apicid,
        .destination_mode=0,
        .trigger_mode=0,
        .redirection_hint=0
    };
    uint16_t remap_table_idx;
    uint32_t dmar_id;
    kurd=dmar::regist_interrupt_simp(arg,remap_table_idx,dmar_id);
    if(error_kurd(kurd)){

    }
    kurd=main_router->irq_regist(1,remap_table_idx,false);
    if(error_kurd(kurd)){

    }
    }else{
        ioapic_driver::compact_flag flag={
            .vec=vec,
            .trigger_mode=0,
            .polarity=0  
        };
        flag.target_apicid=target_apicid;
        kurd=main_router->irq_regist(1,flag);
    }
    key_board_led.raw=0;
    led_set();
    while(inb(0x64)&0x3);
    outb(0x64,0x20);
    uint8_t command=inb(0x60);
    while(inb(0x64)&0x3);
    command|=1;
    outb(0x64,0x20);
    while(inb(0x64)&0x3);
    outb(0x60,command);
    while(inb(0x64)&0x3);
    
    i8042_scancode_buffer_subscriber_queue=new tid_wait_queue;
    
}
