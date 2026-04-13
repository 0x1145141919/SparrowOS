#include "firmware/gSTResloveAPIs.h"
#include "memory/memory_base.h"
#include "memory/AddresSpace.h"
#include "abi/os_error_definitions.h"
#include "panic.h"
#include "memory/init_memory_info.h"

acpimgr_t gAcpiVaddrSapceMgr;
typedef uint64_t GUID_DEF[2];
bool is_guid_equal(GUID_DEF *a, GUID_DEF *b)
{
    return ((*a)[0] == (*b)[0]) && ((*a)[1] == (*b)[1]);
}

int acpimgr_t::Init(EFI_SYSTEM_TABLE *st)
{
  RSDP_struct *rsdp_phy = nullptr;
  uint8_t configtb_count=st->NumberOfTableEntries;
    bool compare_result;
    EFI_CONFIGURATION_TABLE *configtb_base = st->ConfigurationTable;
    EFI_GUID acpi_guid = ACPI_20_TABLE_GUID;
    for (int i = 0; i < configtb_count; i++)
    {
       compare_result = is_guid_equal((GUID_DEF *)&acpi_guid, (GUID_DEF *)&configtb_base[i].VendorGuid);
       if (compare_result)
       {
           rsdp_phy = (RSDP_struct *)configtb_base[i].VendorTable;
           break;
       }
    }
    
    // 检查是否找到 ACPI 表
    if (!rsdp_phy) {
        return OS_ACPI_NOT_FOUNED;
    }
    
    // 从全局 phymem_segments 中查找 XSDT 所在的内存段
    uint64_t xsdt_phy = rsdp_phy->XsdtAddress;
    phymem_segment *des = nullptr;
    // 遍历 phymem_segments 数组查找包含 XSDT 物理地址的段
    for(uint64_t i = 0; i < phymem_segments_count; i++) {
      phymem_segment& seg = phymem_segments[i];
        if(xsdt_phy >= seg.start && xsdt_phy < seg.start + seg.size) {
            des = &seg;
            break;
        }
    }
    
    if (!des || des->type != EFI_ACPI_RECLAIM_MEMORY)
    {
        return OS_INVALID_PARAMETER;
    }
    
    KURD_t kurd;
    acpi_seg_pbase = des->start;
    acpi_seg_size = des->size;

    acpi_seg_vbase=kspace_vm_table->alloc_available_space(des->size,des->start%0x40000000);
    //if(acpi_seg_vbase == 0) return kurd_get_raw(kurd);
    kurd=KspacePageTable::enable_VMentry(vm_interval{
        .vbase=acpi_seg_vbase,
        .pbase=acpi_seg_pbase,
        .size=acpi_seg_size,
        .access=KspacePageTable::PG_RW
    });
    if(error_kurd(kurd)){
        return kurd_get_raw(kurd);
    }
    XSDT_OFFSET = xsdt_phy - acpi_seg_pbase;
    XSDT_Table *vXSDT = (XSDT_Table*)(xsdt_phy);
    xsdt_entry_count = (vXSDT->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
    for (int i = 0; i < xsdt_entry_count; i++)
    {
     uint32_t Sig = *(uint32_t*)vXSDT->Entry[i];
       switch (Sig)
       {
        case FADT_SIGNATURE_UINT32:
        {
            FADT_OFFSET = (uint64_t)((FADT_Table*)vXSDT->Entry[i]) - acpi_seg_pbase;
            FADT_Table *vFADT = (FADT_Table*)(acpi_seg_pbase + FADT_OFFSET);
            DSDT_OFFSET = (uint64_t)(vFADT->Dsdt - acpi_seg_pbase);
            break;
        }
        case FACS_SIGNATURE_UINT32:
            FACS_OFFSET = (uint64_t)((FACS_Table*)vXSDT->Entry[i]) - acpi_seg_pbase;
            break;
        case MADT_SIGNATURE_UINT32:
            MADT_OFFSET = (uint64_t)((MADT_Table*)vXSDT->Entry[i]) - acpi_seg_pbase;
            break;
        case MCFG_SIGNATURE_UINT32:
            MCFG_OFFSET = (uint64_t)((MCFG_Table*)vXSDT->Entry[i]) - acpi_seg_pbase;
            break;
        case HPET_SIGNATURE_UINT32:
            HPET_OFFSET = (uint64_t)((HPET_Table*)vXSDT->Entry[i]) - acpi_seg_pbase;
            break;
        case DMAR_SIGNATURE_UINT32:
            DMAR_OFFSET = (uint64_t)((uint8_t*)vXSDT->Entry[i]) - acpi_seg_pbase;
            break;
        default:
            break;
       }
    }
    
    return OS_SUCCESS;
}
void *acpimgr_t::get_acpi_table(char *signature)
{
    // 检查acpi_seg_vbase是否已初始化
    if (!acpi_seg_vbase) {
        return nullptr;
    }
    
    // 将字符签名转换为uint32_t进行比较
    uint32_t sig = *(uint32_t*)signature;
    
    switch (sig) {
    case XSDT_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+XSDT_OFFSET);
    case FADT_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+FADT_OFFSET);
    case FACS_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+FACS_OFFSET);
    case MADT_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+MADT_OFFSET);
    case DSDT_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+DSDT_OFFSET);
    case MCFG_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+MCFG_OFFSET);
    case HPET_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+HPET_OFFSET);
    case DMAR_SIGNATURE_UINT32:
        return (void*)(acpi_seg_vbase+DMAR_OFFSET);
    default:
        // 对于未直接作为类成员的SSDT表或其他表，可以在XSDT中查找
        XSDT_Table*vXSDT = (XSDT_Table*)(acpi_seg_vbase+XSDT_OFFSET);
        for (int i = 0; i < xsdt_entry_count; i++) {
            uint32_t entry_sig = *(uint32_t*)vXSDT->Entry[i]; // 修复：直接从Entry获取签名
            if (entry_sig == sig) {
                return (void*)vXSDT->Entry[i];
            }
        }
        break;
    }
    
    return nullptr;
}