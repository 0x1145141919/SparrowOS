# 调度器子系统模块
set(Scheduler_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/per_processor_scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/kthread_interfaces.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bq_system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/task_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/task.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yield.asm
)

add_library(Scheduler_module OBJECT ${Scheduler_SOURCE})

target_include_directories(Scheduler_module PRIVATE
    /usr/include/efi/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_options(Scheduler_module PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>: -nostdlib -ffreestanding -fno-use-cxa-atexit
    -fno-exceptions -fno-threadsafe-statics -fno-builtin -fno-rtti -fno-PIC
    -mgeneral-regs-only -mno-red-zone -fno-stack-protector
    -DKERNEL_MODE -DPGLV_4 -DPRINT_OUT
    $<$<CONFIG:Debug>:-g -O0 -gdwarf-4>
    $<$<CONFIG:Release>:-O2> >
)
