# firmware / ACPI / UEFI 模块
set(firmware_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/acpimgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/APIC_convert_functions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/APIC_table_analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UefiRunTimeServices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/uefi_kshell_commands.cpp
)

add_library(firmware_module OBJECT ${firmware_SOURCE})

target_include_directories(firmware_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_options(firmware_module PRIVATE
    -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics
    -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2>
)
