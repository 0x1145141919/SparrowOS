# x86_64 Processor 管理模块
set(Processor_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/local_processor_manage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/APs_bringup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fixed_interrupt_resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_processor_regist.asm
)

add_library(Processor_module OBJECT ${Processor_SOURCE})

target_include_directories(Processor_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_compile_options(Processor_module PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>: -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2> >
)
