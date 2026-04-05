#include "abi/boot.h"
#include "../init/include/load_kernel.h"
#include "../init/include/pages_alloc.h"
#include "../init/include/util/textConsole.h"
#include "../init/include/util/kout.h"
#include "../init/include/core_hardwares/PortDriver.h"
#include "../init/include/panic.h"
#include "../init/include/init_linker_symbols.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "abi/boot.h"
extern init_to_kernel_info* build_init_to_kernel_info(
    kernel_mmu* kmmu,
    BootInfoHeader* header,
    loaded_file_entry* symbols_entry,
    load_kernel_info_pack& pak);
extern int setup_low_identity_maps(kernel_mmu* kmmu, BootInfoHeader* header);
extern int map_symbols_file(kernel_mmu* kmmu, load_kernel_info_pack& pak, loaded_file_entry* symbol_file);
extern int map_gop_buffer(kernel_mmu* kmmu, load_kernel_info_pack& pak, BootInfoHeader* header);
extern int map_and_init_hpet(kernel_mmu* kmmu, load_kernel_info_pack& pak, EFI_SYSTEM_TABLE* gST);


extern "C" void shift_kernel(init_to_kernel_info*info,vaddr_t stack_bottom,vaddr_t entry_vaddr);
extern "C" void init(BootInfoHeader* header)
{
    GlobalStatus = kernel_state::EARLY_BOOT;
    // 5. 初始化 kout 输出系统 (自动注册 UART 后端)
    bsp_kout.Init();
    // 1. 初始化堆分配器
    heap.first_linekd_heap_Init();
    pass_through_device_info* pass_through_devices = header->pass_through_devices;
    for(uint64_t i = 0; i < header->pass_through_device_info_count; i++){
        switch(pass_through_devices[i].device_info){
        case PASS_THROUGH_DEVICE_GRAPHICS_INFO:
            GlobalBasicGraphicInfoType* gfx_info = (GlobalBasicGraphicInfoType*)pass_through_devices[i].specify_data;
            InitGop::Init(gfx_info);
            break;
        }
    }
    
    init_textconsole::Init(
        reinterpret_cast<const unsigned char*>(ter16x32_data),
        {16, 32},           // 字符尺寸
        0xFFFFFFFF,         // 白色文字
        0xFF000000          // 黑色背景
    );
    
    // 4. 初始化 UART 串口 (COM1, 115200)
    serial_init_stage1();
    kio::kout_backend screen_backend = {
        .name = "textconsole",
        .is_masked=0,
        .write = &init_textconsole::PutString
    };
    //bsp_kout.register_backend(screen_backend);
    
    // 6. 测试输出
    bsp_kout<< "[INIT] System initialization started" << kendl;
    int result= basic_allocator::Init(header->memory_map_ptr, header->memory_map_entry_count);
    if(result!=OS_SUCCESS){
        bsp_kout<< "[ERROR] basic_allocator::Init failed with error code: " << result << kendl;
        asm volatile("hlt");
    }
    uint64_t init_image_size=(uint64_t)&__init_heap_end-(uint64_t)&__init_text_start;
    result=basic_allocator::pages_set(mem_interval{(uint64_t)&__init_text_start,align_up(init_image_size,4096)},PHY_MEM_TYPE::OS_KERNEL_DATA);
    if(result!=OS_SUCCESS){
        bsp_kout<< "[ERROR] basic_allocator::pages_set failed for init image, error code: " << result << kendl;
        asm volatile("hlt");
    }
    result=basic_allocator::pages_set(mem_interval{(uint64_t)header,header->total_pages_count*4096},PHY_MEM_TYPE::OS_KERNEL_DATA);
    if(result!=OS_SUCCESS){
        bsp_kout<< "[ERROR] basic_allocator::pages_set failed for boot info header, error code: " << result << kendl;
        asm volatile("hlt");
    }
    uint64_t file_count=header->loaded_file_count;
    loaded_file_entry*file_entry=header->loaded_files;
    
    for(uint64_t i=0;i<file_count;i++){
        if(file_entry[i].file_type==LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD)continue;
        result=basic_allocator::pages_set(mem_interval{(uint64_t)file_entry[i].raw_data,align_up(file_entry[i].file_size,4096)},PHY_MEM_TYPE::OS_KERNEL_DATA);
        if(result!=OS_SUCCESS){
            bsp_kout<< "[ERROR] basic_allocator::pages_set failed for loaded file: " << file_entry[i].file_name;
            bsp_kout<< ", error code: " << result << kendl;
            asm volatile("hlt");
        }
    }
    kernel_mmu*kmmu=new kernel_mmu(arch_enums::x86_64_PGLV4);
    int low_identity_maps = setup_low_identity_maps(kmmu, header);
    if(low_identity_maps!=OS_SUCCESS){
        bsp_kout<< "[ERROR] Identity mapping failed during initialization" << kendl;
        asm volatile("hlt");
    }
    loaded_file_entry* kernel_entry=nullptr;
    loaded_file_entry* symbols_entry=nullptr;
    for(uint64_t i=0;i<file_count;i++){
        if(strcmp_in_kernel( file_entry[i].file_name,"\\kernel.elf")==0){
            kernel_entry=&file_entry[i];
        }
        if(strcmp_in_kernel( file_entry[i].file_name,"\\ksymbols.bin")==0){
            symbols_entry=&file_entry[i];
        }
    }
    if(kernel_entry==nullptr){
        bsp_kout<<"kernel entry not found"<<kendl;
        asm volatile("hlt");
    }
    if(symbols_entry==nullptr){
        bsp_kout<<"symbols entry not found"<<kendl;
        asm volatile("hlt");
    }
    load_kernel_info_pack pak={
        .kmmu=kmmu,
        .kernel_file_entry=*kernel_entry,
        .VM_entry_count=0,
        .VM_entries=nullptr
    };
    basic_allocator::print_now_segs();
    result=kernel_load(pak);
    if(result!=OS_SUCCESS){
        bsp_kout<< "[ERROR] kernel_load failed with error code: " << result << kendl;
        asm volatile("hlt");
    }
    result = map_symbols_file(kmmu, pak, symbols_entry);
    if(result!=OS_SUCCESS){
        bsp_kout<< "[ERROR] Failed to map symbols file, error code: " << result << kendl;
        asm volatile("hlt");
    }
    result = map_gop_buffer(kmmu, pak, header);
    if(result!=OS_SUCCESS){
        bsp_kout<< "[ERROR] Failed to setup GOP buffer mapping, error code: " << result << kendl;
        asm volatile("hlt");
    }
    result =map_and_init_hpet(kmmu, pak, (EFI_SYSTEM_TABLE*)header->gST_ptr);
    if(result!=OS_SUCCESS){ 
        bsp_kout<< "[ERROR] Failed to setup HPET mapping and initialization, error code: " << result << kendl;
        asm volatile("hlt");
    }
    init_to_kernel_info*info = build_init_to_kernel_info(kmmu, header, symbols_entry, pak);
    if(pak.stack_bottom==0||pak.entry_vaddr==0){
        bsp_kout<<"kernel entry or stack bottom is zero"<<kendl;
        asm volatile("hlt");
    }
    info->logical_processor_count=header->logical_processor_count;
    vinterval never_map_rigeon={
        .phybase=0,
        .vbase=0,
        .size=0x4000,
    };
    kmmu->unmap(never_map_rigeon);
    shift_kernel(info,pak.stack_bottom,pak.entry_vaddr);
}

/**
 * @brief 将 PHY_MEM_TYPE 转换为对应的字符串名称
 * @param type 内存类型枚举值
 * @return 对应的类型名称字符串
 */
