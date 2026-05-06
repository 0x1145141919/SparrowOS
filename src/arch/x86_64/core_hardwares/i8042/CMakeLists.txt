# i8042 键盘控制器驱动模块
set(i8042_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/i8042.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/i8042_char_subscriber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/i8042_kshell.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/i8042_text_input_subscriber.cpp
)

add_library(i8042_module OBJECT ${i8042_SOURCE})

target_include_directories(i8042_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)

target_compile_options(i8042_module PRIVATE
    -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics
    -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2>
)
