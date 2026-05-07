#pragma once 
#include "abi/os_error_definitions.h"
namespace infrastructure_location_code{ //bitmap和Ktemplate系列不能也不适合进入这个位置系统，数据结构必须实例化才有意义
    constexpr uint8_t location_code_Architecture_independent_c_functions=1;
    constexpr uint8_t location_code_Architecture_dependent_c_functions=2;//还需要再结合free_to_use字段判断架构，先规定为0x0001为amd64架构，后续如arm/riscV等自己加
    constexpr uint8_t location_code_kout=3;
}