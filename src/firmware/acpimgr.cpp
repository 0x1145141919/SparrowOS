#include "firmware/gSTResloveAPIs.h"
#include "memory/memory_base.h"
#include "memory/AddresSpace.h"
#include "memory/init_memory_info.h"
#include "abi/os_error_definitions.h"
#include "panic.h"
#include "util/kout.h"

acpimgr_t gAcpiVaddrSapceMgr;

int acpimgr_t::Init(phyaddr_t xsdt_pa)
{
    // ---- 1. 从 phymem_segments 中找到包含 XSDT 的 ACPI 段 ----
    phymem_segment* acpi_seg = nullptr;
    for (uint64_t i = 0; i < phymem_segments_count; i++) {
        phymem_segment& seg = phymem_segments[i];
        if (xsdt_pa >= seg.start && xsdt_pa < seg.start + seg.size) {
            acpi_seg = &seg;
            break;
        }
    }
    if (!acpi_seg || acpi_seg->type != EFI_ACPI_RECLAIM_MEMORY) {
        bsp_kout << "[ACPI] XSDT not in ACPI reclaim segment" << kendl;
        return -1;
    }

    // ---- 2. 映射整个 ACPI 段到内核 VA ----
    acpi_seg_pbase = acpi_seg->start;
    acpi_seg_size  = acpi_seg->size;

    vm_interval acpi_iv = {
        .vpn    = 0,
        .ppn    = acpi_seg_pbase >> 12,
        .npages = acpi_seg_size >> 12,
        .access = {1,1,0,0,1,WB}     // kernel, RW, no X
    };
    KURD_t kurd;
    acpi_seg_vbase = Kspace_pinterval_alloc_and_map(acpi_iv, &kurd);
    if (acpi_seg_vbase == 0 || error_kurd(kurd)) {
        bsp_kout << "[ACPI] Kspace_pinterval_alloc_and_map failed" << kendl;
        return -1;
    }

    // ---- 3. 通过映射后的 VA 访问 XSDT，计算各表偏移 ----
    uint64_t xsdt_off = xsdt_pa - acpi_seg_pbase;
    XSDT_OFFSET       = xsdt_off;

    XSDT_Table* vXSDT = (XSDT_Table*)(acpi_seg_vbase + xsdt_off);
    xsdt_entry_count  = (vXSDT->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);

    for (uint32_t i = 0; i < xsdt_entry_count; i++) {
        phyaddr_t entry_pa = vXSDT->Entry[i];
        if (entry_pa == 0) continue;

        uint64_t entry_off = entry_pa - acpi_seg_pbase;
        if (entry_off >= acpi_seg_size) {
            // 表不在本段内，跳过（设计上所有 ACPI 表应在同一段）
            bsp_kout << "[ACPI] table " << i << " outside segment, skip" << kendl;
            continue;
        }

        uint32_t sig = *(uint32_t*)(acpi_seg_vbase + entry_off);
        switch (sig) {
        case FADT_SIGNATURE_UINT32:
            FADT_OFFSET = entry_off;
            {
                FADT_Table* vFADT = (FADT_Table*)(acpi_seg_vbase + entry_off);
                DSDT_OFFSET = (uint64_t)vFADT->Dsdt - acpi_seg_pbase;
            }
            break;
        case FACS_SIGNATURE_UINT32:
            FACS_OFFSET = entry_off;
            break;
        case MADT_SIGNATURE_UINT32:
            MADT_OFFSET = entry_off;
            break;
        case MCFG_SIGNATURE_UINT32:
            MCFG_OFFSET = entry_off;
            break;
        case HPET_SIGNATURE_UINT32:
            HPET_OFFSET = entry_off;
            break;
        case DMAR_SIGNATURE_UINT32:
            DMAR_OFFSET = entry_off;
            break;
        default:
            // SSDT 或其他表：全局 offset 数组记录
            for (int j = 0; j < 40; j++) {
                if (vSSDT_OFFSET[j] == 0) {
                    vSSDT_OFFSET[j] = (uint32_t)entry_off;
                    break;
                }
            }
            break;
        }
    }

    bsp_kout << "[ACPI] Init done: seg=0x" << HEX << acpi_seg_pbase
             << " ->0x" << acpi_seg_vbase
             << " sz=0x" << acpi_seg_size << kendl;
    return 0;
}

void* acpimgr_t::get_acpi_table(char* signature)
{
    if (!acpi_seg_vbase) return nullptr;

    uint32_t sig = *(uint32_t*)signature;
    switch (sig) {
    case XSDT_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + XSDT_OFFSET);
    case FADT_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + FADT_OFFSET);
    case FACS_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + FACS_OFFSET);
    case MADT_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + MADT_OFFSET);
    case DSDT_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + DSDT_OFFSET);
    case MCFG_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + MCFG_OFFSET);
    case HPET_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + HPET_OFFSET);
    case DMAR_SIGNATURE_UINT32: return (void*)(acpi_seg_vbase + DMAR_OFFSET);
    default:
        // SSDT 或其他表：XSDT 中查找
        XSDT_Table* vXSDT = (XSDT_Table*)(acpi_seg_vbase + XSDT_OFFSET);
        for (uint32_t i = 0; i < xsdt_entry_count; i++) {
            phyaddr_t entry_pa = vXSDT->Entry[i];
            if (entry_pa == 0) continue;
            uint64_t off = entry_pa - acpi_seg_pbase;
            if (off >= acpi_seg_size) continue;
            if (*(uint32_t*)(acpi_seg_vbase + off) == sig)
                return (void*)(acpi_seg_vbase + off);
        }
        break;
    }
    return nullptr;
}
