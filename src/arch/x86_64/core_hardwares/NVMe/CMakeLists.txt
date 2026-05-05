# NVMe 驱动模块
# 包含 NVMe 控制器初始化、命令提交、中断处理、关机下线
set(NVMe_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/NVMe_init_and_shutdown.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NVMe_init_thread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NVMe_interrupts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NVMe_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NVMe_kshell.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Identify_cmd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/get_set_features.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/io_queue_cmd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/io_rwc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/io_fdw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PRPs.cpp
)

add_library(NVMe_module OBJECT ${NVMe_SOURCE})

target_include_directories(NVMe_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)

target_compile_options(NVMe_module PRIVATE
    -nostdlib
    -ffreestanding
    -fno-use-cxa-atexit
    -fno-exceptions
    -fno-threadsafe-statics
    -fno-builtin
    -fno-rtti
    -fno-PIC
    -mgeneral-regs-only
    -mno-red-zone
    -fno-stack-protector
    -DKERNEL_MODE
    -DPGLV_4
    -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2>
)
