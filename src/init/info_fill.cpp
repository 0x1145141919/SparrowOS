#include "abi/boot.h"
#include "init/page_allocator.h"
#include "init/load_kernel.h"
#include "init/pages_alloc.h"
#include "init/init_phase_ctx.h"
#include "init/util/kout.h"
#include "init/init_linker_symbols.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/boot.h"
#include "memory/memory_base.h"

// ============================================================================
// 辅助: 调试打印
// ============================================================================
static const char* memtype_str(PHY_MEM_TYPE t) {
    switch(t) {
        case EFI_RESERVED_MEMORY_TYPE: return "EFI_RESERVED";
        case EFI_LOADER_CODE:          return "EFI_LDR_CODE";
        case EFI_LOADER_DATA:          return "EFI_LDR_DATA";
        case EFI_BOOT_SERVICES_CODE:   return "EFI_BS_CODE";
        case EFI_BOOT_SERVICES_DATA:   return "EFI_BS_DATA";
        case EFI_RUNTIME_SERVICES_CODE:return "EFI_RT_CODE";
        case EFI_RUNTIME_SERVICES_DATA:return "EFI_RT_DATA";
        case freeSystemRam:            return "free";
        case EFI_ACPI_RECLAIM_MEMORY:  return "ACPI_RECLM";
        case EFI_ACPI_MEMORY_NVS:      return "ACPI_NVS";
        case EFI_MEMORY_MAPPED_IO:     return "MMIO";
        case OS_KERNEL_DATA:           return "KERN_DATA";
        case OS_KERNEL_CODE:           return "KERN_CODE";
        case OS_ALLOCATABLE_MEMORY:    return "ALLOCATABLE";
        case OS_PGTB_SEGS:             return "PGTABLE";
        case OS_RESERVED_MEMORY:       return "RESERVED";
        default:                       return "OTHER";
    }
}

static void dump_header(const init_to_kernel_header* h, phyaddr_t pkt_base) {
    bsp_kout <<HEX<< kendl
             << "=== init_to_kernel_header @ phys 0x" << pkt_base << " ===" << kendl;
    bsp_kout << "  magic=0x" << h->magic
             << "  self_pages=" << h->self_pages_count << kendl;
    bsp_kout << "  kmmu_interval: 0x" << h->kmmu_interval.start
             << " +0x" << h->kmmu_interval.size << kendl;
    bsp_kout << "  phymem_seg_count=" << h->phymem_segment_count
             << "  map_offset=0x" << h->memory_map_offset << kendl;
    bsp_kout << "  VM_interval_count=" << h->loaded_VM_interval_count
             << "  vm_offset=0x" << h->loaded_VM_intervals_offset << kendl;
    bsp_kout << "  pt_device_count=" << h->pass_through_device_info_count
             << "  pt_offset=0x" << h->pass_through_devices_offset << kendl;
    bsp_kout << "  logical_processor_count=" << h->logical_processor_count << kendl;
    bsp_kout << "  arch_specify_offset=0x" << h->arch_specify_offset << kendl;


    // memory_map
    if (h->memory_map_offset) {
        phymem_segment* map = (phymem_segment*)(pkt_base + h->memory_map_offset);
        bsp_kout << "  -- memory_map (" << h->phymem_segment_count << ") --" << kendl;
        for (uint64_t i = 0; i < h->phymem_segment_count; i++)
            bsp_kout << "    [" << i << "] 0x" << map[i].start
                     << " +0x" << map[i].size
                     << " " << memtype_str(map[i].type) << kendl;
    }

    // VM_intervals
    if (h->loaded_VM_intervals_offset && h->loaded_VM_interval_count) {
        loaded_VM_interval* vm = (loaded_VM_interval*)(pkt_base + h->loaded_VM_intervals_offset);
        bsp_kout << "  -- VM_intervals (" << h->loaded_VM_interval_count << ") --" << kendl;
        for (uint64_t i = 0; i < h->loaded_VM_interval_count; i++)
            bsp_kout << "    [" << i << "] id=0x" << vm[i].VM_interval_specifyid
                     << " p=0x" << vm[i].pbase << " v=0x" << vm[i].vbase
                     << " sz=0x" << vm[i].size << kendl;
    }

    bsp_kout << "========================================" << kendl << kendl;
}

// ============================================================================
// 构建 init_to_kernel_header — 使用偏移量
// ============================================================================
//
// 输入:
//   pkt_pbase  — 信息包的物理基址
//   pkt_pages  — 信息包总页数
//   header     — BootInfoHeader（UEFI 传递）
//   kl         — Phase 3a 上下文（kIMG_self_window, entry 等）
//   iv         — Phase 3b 上下文（各驱动区间列表）
//   seg_view   — phymem_segment 视图
//   seg_count  — 视图条目数
//
// 输出: pkt_pbase 处填充完整的 header + payload，pages_arr 除外
//
phyaddr_t build_init_to_kernel_header(
    phyaddr_t                pkt_pbase,
    uint64_t                 pkt_pages,
    BootInfoHeader*          header,
    const ctx_kernel_loaded* kl,
    const ctx_intervals*     iv,
    phymem_segment*          seg_view,
    uint64_t                 seg_count)
{
    uint8_t* base = reinterpret_cast<uint8_t*>(pkt_pbase);

    // --- 布局计算 ---
    //  [0, header_size): header
    //  [header_size, ...): 4KB 对齐填充
    //  [hdr_off, +):       memory_map[]
    //  [vm_off,  +):       loaded_VM_interval[]
    //  [pt_off,  +):       pass_through_device_info[]
    //  [pt_off + pt_sz, +): pass_through specify_data blobs
    //  [arch_off,+):       x86_specify_init_to_kernel_info

    uint64_t hdr_sz      = sizeof(init_to_kernel_header);
    uint64_t hdr_off     = align_up(hdr_sz, 4096);       // header → 4KB 对齐

    uint64_t extra_vm_count = iv->extra_vm_count;
    uint64_t map_sz      = seg_count * sizeof(phymem_segment);
    uint64_t vm_sz       = extra_vm_count * sizeof(loaded_VM_interval);
    uint64_t pt_sz       = header->pass_through_device_info_count * sizeof(pass_through_device_info);
    uint64_t arch_sz     = sizeof(x86_specify_init_to_kernel_info);

    // pass_through specify_data 需要拷贝到包内
    uint64_t pt_data_sz = 0;
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].specify_data) {
            switch (header->pass_through_devices[i].device_info) {
            case PASS_THROUGH_DEVICE_GRAPHICS_INFO:
                pt_data_sz += align_up(sizeof(GlobalBasicGraphicInfoType), 8);
                break;
            }
        }
    }

    uint64_t map_off     = hdr_off;
    uint64_t vm_off      = map_off + map_sz;
    uint64_t pt_off      = vm_off  + vm_sz;
    uint64_t pt_data_off = pt_off  + pt_sz;
    uint64_t arch_off    = align_up(pt_data_off + pt_data_sz, 8);

    uint64_t total       = arch_off + arch_sz;
    uint64_t allocated   = pkt_pages * 4096;
    (void)allocated;

    if (total > allocated) {
        bsp_kout << "[BUILD_HEADER] FATAL: pkt too small: need 0x"
                 << total << " but have 0x" << allocated << kendl;
        return 0;
    }

    // --- 填充 header ---
    init_to_kernel_header* h = reinterpret_cast<init_to_kernel_header*>(base);
    h->magic                         = 0x494E494B524E4C48ULL; // "INIKRNLH"
    h->self_pages_count              = pkt_pages;
    h->kmmu_interval                 = {kl->kmmu->get_self_alloc_interval().start,
                                        kl->kmmu->get_self_alloc_interval().size,
                                        PHY_MEM_TYPE::OS_PGTB_SEGS};
    h->phymem_segment_count          = seg_count;
    h->memory_map_offset             = map_off;
    h->loaded_VM_interval_count      = extra_vm_count;
    h->loaded_VM_intervals_offset    = vm_off;
    h->pass_through_device_info_count= header->pass_through_device_info_count;
    h->pass_through_devices_offset   = pt_off;
    h->logical_processor_count       = header->logical_processor_count;

    // 一等字段由 ctx 参数提供
    h->kIMG_self_window  = kl->kIMG_self_window;
    h->kIMG_self_size    = kl->kimg_file_size;
    h->kBSS_interval     = {};  // 已归入 PT_LOAD 通用处理，kernel 应扫描程序头表
    h->pages_arr         = {0, 0, 0, {}};     // Phase 4.5 填入
    h->FPA_bitmaps       = iv->FPA_bitmaps;
    h->log_buffer        = iv->log_buffer;
    h->kernel_entry_stack= {};  // 已废弃——Phase 4.5 跳转时用 BSP GS 复合体的 rsp0 栈
    h->symtable_file     = iv->symtable_file;
    h->initramfs_file    = iv->initramfs_file;
    h->Kspace_phyaddr_access_window = iv->Kspace_phyaddr_access_window;
    h->arch_specify_offset = arch_off;

    // --- 填充 memory_map ---
    if (map_sz && seg_view) {
        phymem_segment* dst = reinterpret_cast<phymem_segment*>(base + map_off);
        ksystemramcpy(seg_view, dst, map_sz);
    }

    // --- 填充额外 VM_intervals ---
    if (vm_sz && iv->extra_vm_arr) {
        loaded_VM_interval* dst = reinterpret_cast<loaded_VM_interval*>(base + vm_off);
        ksystemramcpy(iv->extra_vm_arr, dst, vm_sz);
    }

    // --- 填充 pass_through_devices ---
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        pass_through_device_info* dst_pt =
            reinterpret_cast<pass_through_device_info*>(base + pt_off);
        dst_pt[i].device_info = header->pass_through_devices[i].device_info;
        dst_pt[i].specify_data = nullptr;
    }

    {
        uint64_t cur_data_off = 0;
        for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
            if (!header->pass_through_devices[i].specify_data) continue;
            uint64_t data_sz = 0;
            switch (header->pass_through_devices[i].device_info) {
            case PASS_THROUGH_DEVICE_GRAPHICS_INFO:
                data_sz = sizeof(GlobalBasicGraphicInfoType);
                break;
            }
            if (data_sz == 0) continue;

            void* dst_data = base + pt_data_off + cur_data_off;
            ksystemramcpy(header->pass_through_devices[i].specify_data, dst_data, data_sz);

            // 写入偏移量而非指针
            pass_through_device_info* dst_pt =
                reinterpret_cast<pass_through_device_info*>(base + pt_off);
            dst_pt[i].specify_data = reinterpret_cast<void*>(pkt_pbase + pt_data_off + cur_data_off);

            cur_data_off += align_up(data_sz, 8);
        }
    }

    // --- 填充 arch_specify ---
    {
        x86_specify_init_to_kernel_info* dst_arch =
            reinterpret_cast<x86_specify_init_to_kernel_info*>(base + arch_off);
        ksystemramcpy((void*)&iv->arch_info, dst_arch, sizeof(iv->arch_info));
    }

    // --- 调试打印 ---
    dump_header(h, pkt_pbase);

    return pkt_pbase;
}
