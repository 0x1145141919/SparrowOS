#pragma once
#include <stdint.h>
#include <abi/os_error_definitions.h>
namespace MEMMODULE_LOCATIONS{
        constexpr uint8_t INVALID=0;
        constexpr uint8_t LOCATION_CODE_BASE_MEMMGR=1;
        constexpr uint8_t LOCATION_CODE_PHYMEM_ACCESSOR=2;
        constexpr uint8_t LOCATION_CODE_KSPACE_MAP_MGR=16;
        constexpr uint8_t LOCATION_CODE_ADDRESSPACE=24;
        constexpr uint8_t LOCATION_CODE_OUT_SURFACES=40;
}