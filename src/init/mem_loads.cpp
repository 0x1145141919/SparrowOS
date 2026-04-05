#include "abi/boot.h"
#include "../init/include/kernel_mmu.h"
#include "../init/include/pages_alloc.h"
#include "../init/include/util/textConsole.h"
#include "../init/include/util/kout.h"
#include "../init/include/core_hardwares/PortDriver.h"
#include "../init/include/panic.h"
#include "../init/include/load_kernel.h"
#include "../init/include/init_linker_symbols.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "abi/boot.h"
#include "firmware/gSTResloveAPIs.h"
#include "arch/x86_64/init/page_table.h"
#include "arch/x86_64/core_hardwares/HPET.h"
uint64_t va_alloc(uint64_t size,uint8_t align_log2);
int setup_low_identity_maps(kernel_mmu* kmmu, BootInfoHeader* header)
{
    uint64_t entry_count = 0;
    phymem_segment* pure_view = basic_allocator::get_pure_memory_view(&entry_count);
    if (!pure_view || entry_count == 0) {
        return -1;
    }

    for (uint64_t i = 0; i < entry_count; i++) {
        phymem_segment& seg = pure_view[i];
        vinterval identity_map_interval{seg.start, seg.start, seg.size};
        pgaccess access;
        access.is_kernel = 1;
        access.is_writeable = 1;
        access.is_readable = 1;
        access.is_executable = 1;
        access.is_global = 0;

        switch (seg.type) {
            case PHY_MEM_TYPE::EFI_RUNTIME_SERVICES_CODE:
            case PHY_MEM_TYPE::EFI_RUNTIME_SERVICES_DATA:
            case PHY_MEM_TYPE::freeSystemRam:
            case PHY_MEM_TYPE::EFI_ACPI_RECLAIM_MEMORY:
            case PHY_MEM_TYPE::OS_KERNEL_DATA:
            case PHY_MEM_TYPE::OS_KERNEL_CODE:
            case PHY_MEM_TYPE::OS_KERNEL_STACK:
            case PHY_MEM_TYPE::OS_ALLOCATABLE_MEMORY:
            case PHY_MEM_TYPE::OS_MEMSEG_HOLE:
                access.cache_strategy = cache_strategy_t::WB;
                break;
            case PHY_MEM_TYPE::EFI_ACPI_MEMORY_NVS:
            case PHY_MEM_TYPE::EFI_MEMORY_MAPPED_IO:
            case PHY_MEM_TYPE::EFI_RESERVED_MEMORY_TYPE:
            case PHY_MEM_TYPE::EFI_PERSISTENT_MEMORY:
                access.cache_strategy = cache_strategy_t::UC;
                break;
            default:
                access.cache_strategy = cache_strategy_t::UC;
                break;
        }

        int result = kmmu->map(identity_map_interval, access);
        if (result != 0) {
            bsp_kout<< "[WARN] Identity map failed for segment type: ";
            bsp_kout<< static_cast<uint32_t>(seg.type);
            bsp_kout<< ", start: 0x";
            bsp_kout<< seg.start;
            bsp_kout<< ", size: 0x";
            bsp_kout<< seg.size;
            bsp_kout<< ", error code: ";
            bsp_kout<< result;
            bsp_kout<< kendl;
            continue;
        }
    }
    return 0;
}

int map_symbols_file(kernel_mmu* kmmu, load_kernel_info_pack& pak, loaded_file_entry* symbol_file)
{
    uint64_t aligned_size = align_up(symbol_file->file_size, 4096);
    vaddr_t new_base = va_alloc(aligned_size,21);
    phyaddr_t pbase = basic_allocator::pages_alloc(aligned_size,21);
    ksystemramcpy((void*)symbol_file->raw_data,(void*)pbase , symbol_file->file_size);
    pgaccess access;
    access.is_kernel = 1;
    access.is_writeable = 1;
    access.is_readable = 1;
    access.is_executable = 0;
    access.is_global = 1;
    access.cache_strategy = static_cast<cache_strategy_t>(WB);
    int result = kmmu->map(vinterval{pbase, new_base, aligned_size}, access);
    pak.VM_entries[pak.VM_entry_count] = loaded_VM_interval{pbase, new_base, aligned_size, VM_ID_KSYMBOLS, access};
    pak.VM_entry_count++;
    return result;
}

int map_gop_buffer(kernel_mmu* kmmu, load_kernel_info_pack& pak, BootInfoHeader* header)
{
    pass_through_device_info* pt_info = header->pass_through_devices;
    pass_through_device_info* device_gop = nullptr;
    pgaccess gop_buffer_map_access;
    gop_buffer_map_access.is_kernel = 1;
    gop_buffer_map_access.is_writeable = 1;
    gop_buffer_map_access.is_readable = 1;
    gop_buffer_map_access.is_executable = 0;
    gop_buffer_map_access.is_global = 1;
    gop_buffer_map_access.cache_strategy = static_cast<cache_strategy_t>(WC);
    for(uint64_t i=0;i<header->pass_through_device_info_count;i++){
        if(pt_info[i].device_info==PASS_THROUGH_DEVICE_GRAPHICS_INFO){
            device_gop=&pt_info[i];
            break;
        }
    }
    if (!device_gop) {
        return -1;
    }
    GlobalBasicGraphicInfoType* graphic = (GlobalBasicGraphicInfoType*)device_gop->specify_data;  
    uint64_t aligned_size = align_up(graphic->FrameBufferSize, 4096);
    vaddr_t new_base = va_alloc(aligned_size,21);
    phyaddr_t pbase = graphic->FrameBufferBase;
    int result = kmmu->map(vinterval{pbase, new_base, aligned_size}, gop_buffer_map_access);
    if (result != OS_SUCCESS) {
        return result;
    }
    pak.VM_entries[pak.VM_entry_count] = loaded_VM_interval{pbase, new_base, aligned_size, VM_ID_GRAPHIC_BUFFER, gop_buffer_map_access};
    pak.VM_entry_count++;
    return result;
}
int map_and_init_hpet(kernel_mmu* kmmu, load_kernel_info_pack& pak, EFI_SYSTEM_TABLE* gST)
{
    /**
     * 需要先解析acpi表找到hpet表从中解析出hpet的物理地址
     * 再给kmmu以global+rw+UC的方式给kmmu映射
     * 最后再在init.elf的恒等映射的条件下初始化计时器
     */
    auto get_hpet_table = [&]() -> HPET_Table* {
        if (gST == nullptr || gST->ConfigurationTable == nullptr) {
            return nullptr;
        }
        auto is_guid_equal = [](const EFI_GUID& a, const EFI_GUID& b) -> bool {
            if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3) {
                return false;
            }
            for (uint32_t i = 0; i < 8; ++i) {
                if (a.Data4[i] != b.Data4[i]) {
                    return false;
                }
            }
            return true;
        };

        RSDP_struct* rsdp = nullptr;
        EFI_GUID acpi20_guid = ACPI_20_TABLE_GUID;
        EFI_GUID acpi10_guid = ACPI_TABLE_GUID;
        for (uint64_t i = 0; i < gST->NumberOfTableEntries; ++i) {
            EFI_CONFIGURATION_TABLE& t = gST->ConfigurationTable[i];
            if (is_guid_equal(t.VendorGuid, acpi20_guid)) {
                rsdp = reinterpret_cast<RSDP_struct*>(t.VendorTable);
                break;
            }
        }
        if (rsdp == nullptr) {
            return nullptr;
        }

        XSDT_Table* xsdt = reinterpret_cast<XSDT_Table*>(rsdp->XsdtAddress);
        if (xsdt == nullptr || xsdt->Header.Length < sizeof(ACPI_Table_Header)) {
            return nullptr;
        }
        const uint64_t entry_count =
            (xsdt->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
        for (uint64_t i = 0; i < entry_count; ++i) {
            ACPI_Table_Header* hdr = reinterpret_cast<ACPI_Table_Header*>(xsdt->Entry[i]);
            if (hdr == nullptr) {
                continue;
            }
            uint32_t sig = *reinterpret_cast<uint32_t*>(hdr->Signature);
            if (sig == HPET_SIGNATURE_UINT32) {
                return reinterpret_cast<HPET_Table*>(hdr);
            }
        }
        return nullptr;
    };
    auto init_hpet = [](HPET_Table* hpet_table) -> int {
        //访问物理地址前要先用modify_access确保那个页的权限，缓存策略修改为UC
        //参考HPET_driver_only_read_time_stamp.second_stage_init的实现
        if (hpet_table == nullptr) {
            return OS_INVALID_PARAMETER;
        }
        const uint64_t hpet_base = hpet_table->Base_Address;
        if ((hpet_base & 0xFFFULL) != 0) {
            return OS_INVALID_PARAMETER;
        }

        pgaccess mmio_access{};
        mmio_access.is_kernel = 1;
        mmio_access.is_writeable = 1;
        mmio_access.is_readable = 1;
        mmio_access.is_executable = 0;
        mmio_access.is_global = 1;
        mmio_access.cache_strategy = UC;
        const int access_ret = modify_access(phymem_segment{
            .start = hpet_base,
            .size = 0x1000,
            .type = PHY_MEM_TYPE::EFI_MEMORY_MAPPED_IO
        }, mmio_access);
        if (access_ret != OS_SUCCESS) {
            return access_ret;
        }

        volatile uint64_t* hpet_regs = reinterpret_cast<volatile uint64_t*>(hpet_base);
        auto reg_read = [&](uint32_t off) -> uint64_t {
            return hpet_regs[off >> 3];
        };
        auto reg_write = [&](uint32_t off, uint64_t val) {
            hpet_regs[off >> 3] = val;
        };

        const uint64_t cap_id = reg_read(HPET::regs::offset_General_Capabilities_and_ID);
        const uint8_t comparator_count = static_cast<uint8_t>(
            ((cap_id >> HPET::regs::COMPARATOR_COUNT_LEFT_OFFSET) & HPET::regs::COMPARATOR_COUNT_MASK) + 1
        );

        for (uint8_t i = 0; i < comparator_count; ++i) {
            const uint32_t cfg_off =
                HPET::regs::offset_Timer0_Configuration_and_Capabilities +
                static_cast<uint32_t>(i) * HPET::regs::size_per_timer;
            uint64_t tcfg = reg_read(cfg_off);
            tcfg &= ~HPET::regs::Tn_INT_ENB_CNF_BIT;
            reg_write(cfg_off, tcfg);
        }

        uint64_t gcfg = reg_read(HPET::regs::offset_General_Config);
        gcfg |= HPET::regs::GCONFIG_ENABLE_BIT;
        reg_write(HPET::regs::offset_General_Config, gcfg);
        return OS_SUCCESS;
    };
    if (kmmu == nullptr || gST == nullptr || pak.VM_entries == nullptr) {
        return OS_INVALID_PARAMETER;
    }

    HPET_Table* hpet_table = get_hpet_table();
    if (hpet_table == nullptr) {
        return OS_ACPI_NOT_FOUNED;
    }
    if ((hpet_table->Base_Address & 0xFFFULL) != 0) {
        return OS_INVALID_PARAMETER;
    }

    pgaccess hpet_mmio_access{};
    hpet_mmio_access.is_kernel = 1;
    hpet_mmio_access.is_writeable = 1;
    hpet_mmio_access.is_readable = 1;
    hpet_mmio_access.is_executable = 0;
    hpet_mmio_access.is_global = 1;
    hpet_mmio_access.cache_strategy = UC;

    const uint64_t hpet_mmio_size = 0x1000;
    const vaddr_t hpet_vbase = va_alloc(hpet_mmio_size, 12);
    if (hpet_vbase == 0) {
        return OS_OUT_OF_MEMORY;
    }
    int map_ret = kmmu->map(vinterval{
        .phybase = hpet_table->Base_Address,
        .vbase = hpet_vbase,
        .size = hpet_mmio_size
    }, hpet_mmio_access);
    if (map_ret != OS_SUCCESS) {
        return map_ret;
    }
    pak.VM_entries[pak.VM_entry_count] = loaded_VM_interval{
        hpet_table->Base_Address, hpet_vbase, hpet_mmio_size, VM_ID_HPET_MMIO, hpet_mmio_access
    };
    pak.VM_entry_count++;

    return init_hpet(hpet_table);
}
