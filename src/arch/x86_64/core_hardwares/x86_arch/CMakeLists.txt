# x86 架构绑定设备模块（IOAPIC/LAPIC/HPET/DMAR/TSC/RTC/PortDriver/GOP）
set(x86_arch_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/DMAR.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HPET.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PortDriver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ioapic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lapic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tsc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rtc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/primitive_gop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_kshell_commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/device_interrupt_handlers_bare_enter.asm
)

add_library(x86_arch_module OBJECT ${x86_arch_SOURCE})

target_include_directories(x86_arch_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)

target_compile_options(x86_arch_module PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>: -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2> >
)
