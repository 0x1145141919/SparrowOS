#include <arch/x86_64/core_hardwares/i8042.h>
#include <arch/x86_64/core_hardwares/ioapic.h>
#include <arch/x86_64/core_hardwares/lapic.h>
#include <arch/x86_64/core_hardwares/DMAR.h>
#include <arch/x86_64/Interrupt_system/loacl_processor.h>
#include <util/kout.h>
const char* scancode_to_key(uint8_t scancode);
extern "C" void i8042_cpp_enter(x64_standard_context* frame){
    bsp_kout<<"i8042_cpp_enter: "<<scancode_to_key(inb(0x60))<<kendl;
    x2apic::x2apic_driver::write_eoi();
}
extern "C" char i8042_fault_deal;
const char* scancode_to_key(uint8_t scancode) {
    switch(scancode) {
        // 字母区
        case 0x1c: return "ENTER";
        case 0x15: return "Q";
        case 0x1d: return "W";
        case 0x24: return "E";
        case 0x2d: return "R";
        case 0x2c: return "T";
        case 0x35: return "Y";
        case 0x3c: return "U";
        case 0x43: return "I";
        case 0x44: return "O";
        case 0x4d: return "P";
        case 0x1a: return "Z";
        case 0x1b: return "S";
        case 0x23: return "D";
        case 0x2b: return "F";
        case 0x34: return "G";
        case 0x33: return "H";
        case 0x3b: return "J";
        case 0x42: return "K";
        case 0x4b: return "L";
        case 0x29: return "SPACE";
        
        // 数字区
        case 0x16: return "1";
        case 0x1e: return "2";
        case 0x26: return "3";
        case 0x25: return "4";
        case 0x2e: return "5";
        case 0x36: return "6";
        case 0x3d: return "7";
        case 0x3e: return "8";
        case 0x46: return "9";
        case 0x45: return "0";
        
        // 特殊键
        case 0x66: return "BACKSPACE";
        case 0x0d: return "TAB";
        case 0x58: return "CAPSLOCK";
        case 0x77: return "NUMLOCK";
        case 0x7e: return "SCROLLLOCK";
        case 0x76: return "ESC";
        
        // 修饰键
        case 0x12: return "LEFT_SHIFT";
        case 0x59: return "RIGHT_SHIFT";
        case 0x14: return "LEFT_CTRL";
        case 0x11: return "LEFT_ALT";
        case 0x5a: return "ENTER (keypad)";
        
        default: return "UNKNOWN";
    }
}
void i8042_interrupt_enable(){
    x64_local_processor* manage=x86_smp_processors_container::get_processor_mgr_by_processor_id(++legacy_rotate_interrupt_alloc_id);
    uint32_t target_apicid=manage->get_apic_id();
    uint8_t vec= manage->handler_alloc((void*)&i8042_cpp_enter);
    if(vec==0xff){
        //panic
    }
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
    KURD_t kurd=dmar::regist_interrupt_simp(arg,remap_table_idx,dmar_id);
    if(error_kurd(kurd)){

    }
    kurd=main_router->irq_regist(1,remap_table_idx,false);
    if(error_kurd(kurd)){

    }
    while(inb(0x64)&0x3);
    outb(0x64,0x20);
    uint8_t command=inb(0x60);
    while(inb(0x64)&0x3);
    command|=1;
    outb(0x64,0x20);
    while(inb(0x64)&0x3);
    outb(0x60,command);
    while(inb(0x64)&0x3);
}