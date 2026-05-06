# x86_64 中断子系统模块
set(Interrupts_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/exceptions_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_vecs_deliver_mgr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interrupt_kshell_commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Sysdef_exception_entries.asm
)

add_library(Interrupts_module OBJECT ${Interrupts_SOURCE})

target_include_directories(Interrupts_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_compile_options(Interrupts_module PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>: -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2> >
)
