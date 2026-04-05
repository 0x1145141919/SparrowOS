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
static const char* memory_type_to_string(PHY_MEM_TYPE type) {
    switch (type) {
        case EFI_RESERVED_MEMORY_TYPE: return "EFI_RESERVED_MEMORY_TYPE";
        case EFI_LOADER_CODE: return "EFI_LOADER_CODE";
        case EFI_LOADER_DATA: return "EFI_LOADER_DATA";
        case EFI_BOOT_SERVICES_CODE: return "EFI_BOOT_SERVICES_CODE";
        case EFI_BOOT_SERVICES_DATA: return "EFI_BOOT_SERVICES_DATA";
        case EFI_RUNTIME_SERVICES_CODE: return "EFI_RUNTIME_SERVICES_CODE";
        case EFI_RUNTIME_SERVICES_DATA: return "EFI_RUNTIME_SERVICES_DATA";
        case freeSystemRam: return "freeSystemRam";
        case EFI_UNUSABLE_MEMORY: return "EFI_UNUSABLE_MEMORY";
        case EFI_ACPI_RECLAIM_MEMORY: return "EFI_ACPI_RECLAIM_MEMORY";
        case EFI_ACPI_MEMORY_NVS: return "EFI_ACPI_MEMORY_NVS";
        case EFI_MEMORY_MAPPED_IO: return "EFI_MEMORY_MAPPED_IO";
        case EFI_MEMORY_MAPPED_IO_PORT_SPACE: return "EFI_MEMORY_MAPPED_IO_PORT_SPACE";
        case EFI_PAL_CODE: return "EFI_PAL_CODE";
        case EFI_PERSISTENT_MEMORY: return "EFI_PERSISTENT_MEMORY";
        case EFI_UNACCEPTED_MEMORY_TYPE: return "EFI_UNACCEPTED_MEMORY_TYPE";
        case EFI_MAX_MEMORY_TYPE: return "EFI_MAX_MEMORY_TYPE";
        case OS_KERNEL_DATA: return "OS_KERNEL_DATA";
        case OS_KERNEL_CODE: return "OS_KERNEL_CODE";
        case OS_KERNEL_STACK: return "OS_KERNEL_STACK";
        case OS_HARDWARE_GRAPHIC_BUFFER: return "OS_HARDWARE_GRAPHIC_BUFFER";
        case ERROR_FAIL_TO_FIND: return "ERROR_FAIL_TO_FIND";
        case OS_ALLOCATABLE_MEMORY: return "OS_ALLOCATABLE_MEMORY";
        case OS_RESERVED_MEMORY: return "OS_RESERVED_MEMORY";
        case OS_PGTB_SEGS: return "OS_PGTB_SEGS";
        case OS_MEMSEG_HOLE: return "OS_MEMSEG_HOLE";
        default: return "UNKNOWN_TYPE";
    }
}

static const char* cache_strategy_to_string(cache_strategy_t strategy) {
    switch (strategy) {
        case UC: return "UC";
        case WC: return "WC";
        case WT: return "WT";
        case WP: return "WP";
        case WB: return "WB";
        case UC_minus: return "UC-";
        default: return "UNKNOWN";
    }
}

static const char* vm_id_to_string(uint32_t vm_id) {
    switch (vm_id) {
        case VM_ID_BSP_INIT_STACK: return "VM_ID_BSP_INIT_STACK";
        case VM_ID_FIRST_HEAP_BITMAP: return "VM_ID_FIRST_HEAP_BITMAP";
        case VM_ID_FIRST_HEAP: return "VM_ID_FIRST_HEAP";
        case VM_ID_LOGBUFFER: return "VM_ID_LOGBUFFER";
        case VM_ID_KSYMBOLS: return "VM_ID_KSYMBOLS";
        case VM_ID_UP_KSPACE_PDPT: return "VM_ID_UP_KSPACE_PDPT";
        case VM_ID_GRAPHIC_BUFFER: return "VM_ID_GRAPHIC_BUFFER";
        default: return "UNKNOWN_VM_ID";
    }
}

static void print_init_to_kernel_info(const init_to_kernel_info* info)
{
    bsp_kout<< kendl;
    bsp_kout<< "========================================" << kendl;
    bsp_kout<< "[INFO] init_to_kernel_info Details:" << kendl;
    bsp_kout<< "========================================" << kendl;
    bsp_kout<<HEX << "  magic: 0x" << info->magic << kendl;
    bsp_kout<<DEC << "  self_pages_count: " << info->self_pages_count << " pages (0x" << (info->self_pages_count * 4096) << " bytes)" << kendl;
    bsp_kout<<HEX << "  gST_ptr: 0x" << reinterpret_cast<uint64_t>(info->gST_ptr) << kendl;
    bsp_kout<< "  ksymbols_file_size: " << info->ksymbols_file_size << " bytes (0x" << info->ksymbols_file_size << ")" << kendl;
    bsp_kout<<HEX  << "  kmmu_root_table: 0x" << info->kmmu_root_table << kendl;
    bsp_kout<< "  kmmu_interval:" << kendl;
    bsp_kout<<HEX  << "    start: 0x" << info->kmmu_interval.start << kendl;
    bsp_kout<<HEX  << "    size: 0x" << info->kmmu_interval.size << " bytes" << kendl;
    bsp_kout<< "    type: " << static_cast<uint32_t>(info->kmmu_interval.type) << kendl;
    bsp_kout<<DEC << "  phymem_segment_count: " << info->phymem_segment_count << kendl;
    bsp_kout<<DEC << "  loaded_VM_interval_count: " << info->loaded_VM_interval_count << kendl;
    bsp_kout<<DEC << "  pass_through_device_info_count: " << info->pass_through_device_info_count << kendl;

    bsp_kout<< kendl;
    bsp_kout<< "--- Memory Map (" << info->phymem_segment_count << " entries) ---" << kendl;
    for(uint64_t i = 0; i < info->phymem_segment_count; i++){
        bsp_kout<<HEX << "  [" << i << "] start: 0x" << info->memory_map[i].start 
                     <<DEC  << ", size:" << info->memory_map[i].size 
                      << ", type: " << memory_type_to_string(info->memory_map[i].type) << kendl;
    }

    bsp_kout<< kendl;
    bsp_kout<< "--- Loaded VM Intervals (" << info->loaded_VM_interval_count << " entries) ---" << kendl;
    for(uint64_t i = 0; i < info->loaded_VM_interval_count; i++){
        bsp_kout<<HEX<< "  [" << i << "] pbase: 0x" << info->loaded_VM_intervals[i].pbase 
                      << ", vbase: 0x" << info->loaded_VM_intervals[i].vbase 
                      << ", size: 0x" << info->loaded_VM_intervals[i].size 
                      << DEC <<" PAGES_COUNT: "<< info->loaded_VM_intervals[i].size/0x1000
                      << ", VM_id: " << vm_id_to_string(info->loaded_VM_intervals[i].VM_interval_specifyid)
                      << ", access: kernel=" << static_cast<uint32_t>(info->loaded_VM_intervals[i].access.is_kernel)
                      << ", wr=" << static_cast<uint32_t>(info->loaded_VM_intervals[i].access.is_writeable)
                      << ", rd=" << static_cast<uint32_t>(info->loaded_VM_intervals[i].access.is_readable)
                      << ", exec=" << static_cast<uint32_t>(info->loaded_VM_intervals[i].access.is_executable)
                      << ", glob=" << static_cast<uint32_t>(info->loaded_VM_intervals[i].access.is_global)
                      << ", cache=" << cache_strategy_to_string(info->loaded_VM_intervals[i].access.cache_strategy)
                      << kendl;
    }

    bsp_kout<< kendl;
    bsp_kout<< "--- Pass-through Devices (" << info->pass_through_device_info_count << " entries) ---" << kendl;
    for(uint64_t i = 0; i < info->pass_through_device_info_count; i++){
        bsp_kout<< "  [" << i << "] device_info: 0x" << info->pass_through_devices[i].device_info 
                      << ", specify_data: 0x" << reinterpret_cast<uint64_t>(info->pass_through_devices[i].specify_data) << kendl;
        if(info->pass_through_devices[i].device_info == PASS_THROUGH_DEVICE_GRAPHICS_INFO && 
           info->pass_through_devices[i].specify_data != nullptr){
            GlobalBasicGraphicInfoType* gfx = reinterpret_cast<GlobalBasicGraphicInfoType*>(info->pass_through_devices[i].specify_data);
            bsp_kout.shift_dec();
            bsp_kout<< "      -> Graphics Info:" << kendl;
            bsp_kout<<HEX<< "         FrameBufferBase: 0x" << (void*)gfx->FrameBufferBase << kendl;
            bsp_kout<<DEC<< "         FrameBufferSize: 0x" << gfx->FrameBufferSize << " bytes" << kendl;
            bsp_kout<< "         Resolution: " << gfx->horizentalResolution << "x" << gfx->verticalResolution << kendl;
            bsp_kout<< "         PixelsPerScanLine: " << gfx->PixelsPerScanLine << kendl;
            bsp_kout<< "         PixelFormat: " << static_cast<uint32_t>(gfx->pixelFormat) << kendl;
        }
    }

    bsp_kout<< "========================================" << kendl;
    bsp_kout<< kendl;
}

init_to_kernel_info* build_init_to_kernel_info(
    kernel_mmu* kmmu,
    BootInfoHeader* header,
    loaded_file_entry* symbols_entry,
    load_kernel_info_pack& pak)
{
    constexpr uint8_t default_info_alloc_pages_count=4;
    init_to_kernel_info*info=(init_to_kernel_info*)basic_allocator::pages_alloc(4096*default_info_alloc_pages_count);
    info->self_pages_count=default_info_alloc_pages_count;
    info->gST_ptr=header->gST_ptr;
    info->kmmu_root_table=kmmu->get_root_table_base();
    info->kmmu_interval=phymem_segment{kmmu->get_self_alloc_interval().start,kmmu->get_self_alloc_interval().size,PHY_MEM_TYPE::OS_ALLOCATABLE_MEMORY};
    info->ksymbols_file_size=symbols_entry->file_size;
    uint64_t physegs_count=0;
    phymem_segment*tmp_onheap_segbase=basic_allocator::get_pure_memory_view(&physegs_count);
    info->phymem_segment_count=physegs_count;
    info->loaded_VM_interval_count=pak.VM_entry_count;
    info->pass_through_device_info_count=header->pass_through_device_info_count;

    uint64_t memory_map_size = physegs_count * sizeof(phymem_segment);
    uint64_t loaded_VM_intervals_size = pak.VM_entry_count * sizeof(loaded_VM_interval);
    uint64_t pass_through_devices_size = header->pass_through_device_info_count * sizeof(pass_through_device_info);
    uint64_t total_additional_size = memory_map_size + loaded_VM_intervals_size + pass_through_devices_size;
    uint64_t info_base_size = sizeof(init_to_kernel_info);
    uint64_t info_aligned_size = align_up(info_base_size, 4096);
    uint64_t total_required_bytes = info_aligned_size + total_additional_size;
    uint64_t allocated_bytes = 4096 * default_info_alloc_pages_count;
    if(total_required_bytes > allocated_bytes){
        bsp_kout<< "[ERROR] init_to_kernel_info allocation overflow: required 0x";
        bsp_kout<< total_required_bytes;
        bsp_kout<< " bytes, but only allocated 0x";
        bsp_kout<< allocated_bytes;
        bsp_kout<< " bytes (" << default_info_alloc_pages_count << " pages)" << kendl;
        return nullptr;
    }

    uint8_t* base_ptr = reinterpret_cast<uint8_t*>(info);
    info->memory_map = reinterpret_cast<phymem_segment*>(base_ptr + info_aligned_size);
    info->loaded_VM_intervals = reinterpret_cast<loaded_VM_interval*>(base_ptr + info_aligned_size + memory_map_size);
    info->pass_through_devices = reinterpret_cast<pass_through_device_info*>(base_ptr + info_aligned_size + memory_map_size + loaded_VM_intervals_size);

    if(memory_map_size > 0 && tmp_onheap_segbase != nullptr){
        ksystemramcpy(tmp_onheap_segbase, info->memory_map, memory_map_size);
    }
    if(loaded_VM_intervals_size > 0 && pak.VM_entries != nullptr){
        ksystemramcpy(pak.VM_entries, info->loaded_VM_intervals, loaded_VM_intervals_size);
    }
    if(header->pass_through_device_info_count > 0 && header->pass_through_devices != nullptr){
        ksystemramcpy(header->pass_through_devices, info->pass_through_devices, pass_through_devices_size);

        uint64_t total_specify_data_size = 0;
        for(uint64_t i = 0; i < header->pass_through_device_info_count; i++){
            void* src_specify_data = header->pass_through_devices[i].specify_data;
            if(src_specify_data != nullptr){
                switch(header->pass_through_devices[i].device_info){
                    case PASS_THROUGH_DEVICE_GRAPHICS_INFO:
                        total_specify_data_size += align_up(sizeof(GlobalBasicGraphicInfoType), 8);
                        break;
                    default:
                        break;
                }
            }
        }

        uint64_t specify_data_offset = pass_through_devices_size;
        uint64_t total_used_after_info = info_aligned_size + memory_map_size + loaded_VM_intervals_size + specify_data_offset + total_specify_data_size;
        if(total_used_after_info > allocated_bytes){
            bsp_kout<< "[ERROR] pass_through_devices specify_data overflow: required 0x";
            bsp_kout<< total_used_after_info;
            bsp_kout<< " bytes, but only allocated 0x";
            bsp_kout<< allocated_bytes;
            bsp_kout<< " bytes" << kendl;
            return nullptr;
        }

        uint8_t* specify_data_base = reinterpret_cast<uint8_t*>(info->pass_through_devices) + specify_data_offset;
        uint64_t current_offset = 0;
        for(uint64_t i = 0; i < header->pass_through_device_info_count; i++){
            void* src_specify_data = header->pass_through_devices[i].specify_data;
            if(src_specify_data != nullptr){
                uint64_t specify_data_size = 0;
                switch(header->pass_through_devices[i].device_info){
                    case PASS_THROUGH_DEVICE_GRAPHICS_INFO:
                        specify_data_size = sizeof(GlobalBasicGraphicInfoType);
                        break;
                    default:
                        specify_data_size = 0;
                        break;
                }

                if(specify_data_size > 0){
                    void* new_specify_data = specify_data_base + current_offset;
                    ksystemramcpy(src_specify_data, new_specify_data, specify_data_size);
                    info->pass_through_devices[i].specify_data = new_specify_data;
                    current_offset += align_up(specify_data_size, 8);
                } else {
                    info->pass_through_devices[i].specify_data = nullptr;
                }
            } else {
                info->pass_through_devices[i].specify_data = nullptr;
            }
        }
    }

    print_init_to_kernel_info(info);
    return info;
}