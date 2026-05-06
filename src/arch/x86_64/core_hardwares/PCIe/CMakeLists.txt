# PCIe 枚举与配置模块
set(PCIe_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/PCIe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcie_kshell_commands.cpp
)

add_library(PCIe_module OBJECT ${PCIe_SOURCE})

target_include_directories(PCIe_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)

target_compile_options(PCIe_module PRIVATE
    -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics
    -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2>
)
