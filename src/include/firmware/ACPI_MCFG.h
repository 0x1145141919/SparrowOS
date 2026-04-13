#pragma once 
#include "gSTResloveAPIs.h"
namespace ACPI_MCFG
{
     struct MCFG_Table_Entry {
        uint64_t BaseAddress;
        uint16_t PciSegmentGroupNumber;
        uint8_t StartBusNumber;
        uint8_t EndBusNumber;
        uint8_t Reserved[4];
    };
}
