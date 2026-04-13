#CMakeLists.txt

cmake_minimum_required(VERSION 3.15)  # 推荐使用较新版本以支持现代特性
project(Kernel LANGUAGES C CXX)       # 显式声明支持 C 和 C++

# 设置构建类型变量，默认为Debug
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build (Debug or Release)" FORCE)
endif()

# 查找 NASM 汇编器
find_program(NASM_EXECUTABLE nasm)
if(NOT NASM_EXECUTABLE)
    message(FATAL_ERROR "NASM assembler not found!")
endif()


    add_definitions(-D_GNU_SOURCE)  # 有时候需要这个

# 设置 NASM 编译选项（例如：-f elf64 或 -f win32）
set(NASM_FLAGS "-f elf64")
# 定义汇编文件编译规则
enable_language(ASM_NASM)  # CMake 3.8+ 支持
set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS asm)  # 关联 .asm 后缀

# 设置 NASM 编译器和选项
set(CMAKE_ASM_NASM_COMPILER ${NASM_EXECUTABLE})
set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> <FLAGS> -I${CMAKE_CURRENT_SOURCE_DIR}/src/ -I${CMAKE_CURRENT_SOURCE_DIR}/src/arch/x86_64/boot/ -gdwarf -o <OBJECT> <SOURCE>")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcmodel=large")
# 根据构建类型设置不同的编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Building in DEBUG mode")
    # 在Debug模式下添加调试信息并禁用优化
    add_compile_options(-g -O0 -gdwarf-4)
    set(DEBUG_LINK_FLAGS "-g")
else()
    message(STATUS "Building in RELEASE mode")
    add_compile_options(-O2)
endif()

# 语言特定的选项（可选）
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c23")          # C 标准
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++23")    # C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_KEEP_BUILD_FILES ON)
# 定义通用源文件
set(SRC_COMMON
 #USERMODE需要替换成对应的标准库
    src/utils/util.cpp
    src/utils/kintrin.cpp
    src/os_error_definitions.cpp
    src/utils/bitmap.cpp
    src/utils/lock.cpp
    src/utils/RB_btree.cpp
    src/utils/kout.cpp
    src/arch/x86_64/panic.cpp
    src/utils/kptrace.cpp
    src/utils/tmp_buff.cpp
    src/utils/arch/x86_64/intel_cpuid.cpp
)

# 添加gnu-efi作为外部项目
include(ExternalProject)
ExternalProject_Add(
    gnu-efi
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/outsbmodules/gnu-efi
    CONFIGURE_COMMAND ""  # 跳过 configure 阶段
    BUILD_COMMAND make    # 调用 make 编译
    INSTALL_COMMAND ""     # 不安装
    BUILD_IN_SOURCE TRUE   # 在源目录构建
)
add_custom_target(kernel_resources
)
add_custom_target(ksymbols
    COMMAND ./symbols_dumper
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "运行自定义目标"
)
add_dependencies(ksymbols symbols_dumper init.elf kernel.elf)

add_dependencies(kernel_resources 
    ksymbols
) 
add_library(json_praser 
    user_utils/json_configs_prase.cpp
)
target_include_directories(json_praser PRIVATE
     "/usr/include"
     "${CMAKE_SOURCE_DIR}/src/include"
)

# 获取gnu-efi构建目录路径
ExternalProject_Get_Property(gnu-efi SOURCE_DIR)
set(GNU_EFI_SOURCE_DIR ${SOURCE_DIR})
add_subdirectory(src/memory)
add_subdirectory(outsbmodules/tinySTL)
add_subdirectory(src/fs)
add_executable(symbols_dumper
    build_utils/ksymbols_dumper.cpp
)
# 内核可执行文件
add_executable(kernel.elf
    src/arch/x86_64/panic.cpp
    src/firmware/UefiRunTimeServices.cpp
    src/arch/x86_64/boot/kinit.cpp
    src/utils/kcirclebufflogMgr.cpp
    src/arch/x86_64/boot/kernel_entry.asm
    src/arch/x86_64/boot/ap_init_longmode_exceptions.asm
    src/arch/x86_64/boot/ap_init_pemode_exceptions.asm
    src/arch/x86_64/boot/ap_init_realmode_exceptions.asm
    src/arch/x86_64/panic_processor_resources_shift.asm
    src/utils/int128_div.cpp
    src/arch/x86_64/core_hardwares/lapic.cpp
    src/arch/x86_64/core_hardwares/tsc.cpp
    src/arch/x86_64/Interrupts/exceptions_handler.cpp
    src/arch/x86_64/Processor/local_processor_manage.cpp
    src/firmware/APIC_convert_functions.cpp
    src/firmware/APIC_table_analyzer.cpp
    src/arch/x86_64/Interrupts/Sysdef_exception_entries.asm
    src/arch/x86_64/core_hardwares/HPET.cpp
    src/arch/x86_64/time.cpp
    src/utils/textConsole.cpp
    src/arch/x86_64/core_hardwares/primitive_gop.cpp
    src/arch/x86_64/core_hardwares/DMAR.cpp
    src/arch/x86_64/core_hardwares/i8042.cpp
    src/arch/x86_64/core_hardwares/ioapic.cpp
    src/arch/x86_64/core_hardwares/device_interrupt_handlers_bare_enter.asm
    src/firmware/acpimgr.cpp
    src/arch/x86_64/Processor/APs_bringup.cpp  
    src/arch/x86_64/Processor/early_bsp_resources.cpp  
    src/arch/x86_64/Processor/fixed_interrupt_resources.cpp  
    src/arch/x86_64/Processor/runtime_processor_regist.asm
    src/arch/x86_64/core_hardwares/PortDriver.cpp
    src/scheduler/per_processor_scheduler.cpp
    src/scheduler/kthread_interfaces.cpp
    src/scheduler/yield.asm
    $<TARGET_OBJECTS:Mem_module>
    ${SRC_COMMON}
)
add_executable(init.elf
    src/init/init_init.cpp
    src/init/kernel_mmu.cpp
    src/init/kernel_load.cpp
    src/init/init_entry.asm
    src/init/Interrupts/Sysdef_exception_entries.asm
    src/init/Interrupts/exceptions_handler.cpp
    src/init/heap_alloc.cpp
    src/init/core_hardwares/init_gop.cpp
    src/init/core_hardwares/PortDriver.cpp
    src/init/util/textConsole.cpp
    src/init/pages_alloc.cpp
    src/init/panic.cpp
    src/init/util/kout.cpp
    src/utils/util.cpp
    src/init/util/bitmap.cpp
    src/utils/lock.cpp
    src/arch/x86_64/Processor/early_bsp_resources.cpp 
    src/os_error_definitions.cpp
    src/init/info_fill.cpp
    src/init/mem_loads.cpp
    src/utils/arch/x86_64/intel_cpuid.cpp
)
add_executable(BCB_test
    src/tests/BCB_test.cpp
     ${SRC_COMMON}
    src/memory/mixed_bitmap.cpp
    src/memory/FreePagesAllocator_BCB.cpp
    #src/memory/FreePagesAllocator.cpp
)
target_include_directories(
    BCB_test PRIVATE
    "/usr/include/efi"
     "${CMAKE_SOURCE_DIR}/src/include"
)
target_compile_options(
    BCB_test PRIVATE
    -DUSER_MODE
)

add_executable(test_set_dump
    src/tests/test_set_dump.cpp
)

add_executable(operation_log_dump
    src/tests/operation_log_dump.cpp
)

add_executable(kspace_vm_table_test
    src/tests/kspace_vm_table_test.cpp
)
target_include_directories(
    kspace_vm_table_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
)
target_compile_options(
    kspace_vm_table_test PRIVATE
    -DUSER_MODE
)

add_executable(bcb_replay
    src/tests/bcb_replay.cpp
     ${SRC_COMMON}
    src/memory/mixed_bitmap.cpp
    src/memory/FreePagesAllocator_BCB.cpp
)
target_include_directories(
    bcb_replay PRIVATE
    "/usr/include/efi"
     "${CMAKE_SOURCE_DIR}/src/include"
)
target_compile_options(
    bcb_replay PRIVATE
    -DUSER_MODE
    #-DREPALY_MODE
)


add_executable(test_mem
    src/tests/test_memmodule.cpp
    src/memory/all_pages_arr.cpp
    src/memory/mixed_bitmap.cpp
    src/memory/FreePagesAllocator_BCB.cpp
    src/memory/FreePagesAllocator.cpp
    src/memory/out_surfaces.cpp
     ${SRC_COMMON}
)
target_include_directories(
    test_mem PRIVATE
    "/usr/include/efi"
     "${CMAKE_SOURCE_DIR}/src/include"
)
target_compile_options(
    test_mem PRIVATE
    -DUSER_MODE
    -fno-omit-frame-pointer
    -rdynamic
    -no-pie
)
target_link_libraries(
    test_mem PRIVATE
    json_praser
    -rdynamic
    -no-pie
)

add_executable(multhread
    src/tests/multithread_test.cpp
    src/utils/lock.cpp
    src/utils/bitmap.cpp
    src/utils/util.cpp
)
target_include_directories(
    multhread PRIVATE
     "${CMAKE_SOURCE_DIR}/src/include"
     "/usr/include/efi"
)
target_compile_options(
    multhread PRIVATE
    -DUSER_MODE
    -fsanitize=address
)
target_link_options(multhread PRIVATE -fsanitize=address)
target_link_libraries(
    multhread PRIVATE
    pthread
)

add_executable(pcie_enumeration
    src/tests/pcie_enumeration.cpp
    src/arch/x86_64/core_hardwares/PCIe.cpp
    src/memory/out_surfaces.cpp
    ${SRC_COMMON}
)
target_include_directories(
    pcie_enumeration PRIVATE
    "/usr/include/efi"
    "${CMAKE_SOURCE_DIR}/src/include"
)
target_compile_options(
    pcie_enumeration PRIVATE
    -DUSER_MODE
)
target_link_libraries(
    pcie_enumeration PRIVATE
    tinySTL
)

# AArch64 static cross-build target for multhread
set(MULTHREAD_AARCH64_OUT "${CMAKE_BINARY_DIR}/bin/multhread_aarch64")
set(MULTHREAD_AARCH64_CXX "aarch64-linux-gnu-g++" CACHE STRING "AArch64 C++ cross compiler for multhread_aarch64 target")
set(MULTHREAD_AARCH64_EFI_INC "${CMAKE_SOURCE_DIR}/outsbmodules/gnu-efi/inc" CACHE PATH "gnu-efi include path for AArch64 multhread target")
add_executable(fs_test
    src/tests/fs_test_main.cpp

     ${SRC_COMMON}
)

target_include_directories(
    fs_test PRIVATE
    "/usr/include/efi"
     "${CMAKE_SOURCE_DIR}/src/include/memory"
     "${CMAKE_SOURCE_DIR}/src/include"
)
target_compile_options(
    fs_test PRIVATE
    -DUSER_MODE
    -fsanitize=address
)

# 内核/引导阶段共享链接选项
set(COMMON_ELF_LINK_OPTS
-nostdlib
    -ffreestanding
    -fno-use-cxa-atexit
    -fno-exceptions
    -fno-builtin
    -fno-rtti
    -fno-omit-frame-pointer
    -fno-asynchronous-unwind-tables
    #-ftls-model=local-exec
    -mno-red-zone
    -static
    -lgcc
    -Wl,--wrap=__stack_chk_fail
)

set(KERNEL_LINK_OPTS
    ${COMMON_ELF_LINK_OPTS}
    -T${CMAKE_SOURCE_DIR}/kld.ld
)

set(INIT_LINK_OPTS
    ${COMMON_ELF_LINK_OPTS}
    -T${CMAKE_SOURCE_DIR}/init.ld
)

# 如果在Debug模式下，添加调试链接标志
if(DEBUG_LINK_FLAGS)
    list(APPEND KERNEL_LINK_OPTS ${DEBUG_LINK_FLAGS})
    list(APPEND INIT_LINK_OPTS ${DEBUG_LINK_FLAGS})
endif()

function(configure_kernel_like_elf target_name link_opts)
    target_link_libraries(${target_name} PRIVATE
        ${GNU_EFI_SOURCE_DIR}/x86_64/lib/libefi.a
        ${GNU_EFI_SOURCE_DIR}/x86_64/gnuefi/libgnuefi.a
    )

    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra
        -mgeneral-regs-only
        -ffreestanding
        -mno-red-zone
        -fno-stack-protector
        -fno-exceptions
        -fno-builtin
        -fno-rtti
        -static
        -mgeneral-regs-only
        -fno-lto
        -fpermissive
        -fno-PIC
        -DKERNEL_MODE>
        -DPGLV_4>
    $<$<COMPILE_LANGUAGE:ASM_NASM>:-felf64>
    )

    target_link_options(${target_name} PRIVATE ${link_opts}
        -fno-exceptions
        -fno-builtin
        -fno-rtti
        -static
        -mgeneral-regs-only
        -fno-lto
        -fpermissive
        -fno-PIC
        -DKERNEL_MODE>
        -DPGLV_4>
    $<$<COMPILE_LANGUAGE:ASM_NASM>:-felf64>
    )
endfunction()

configure_kernel_like_elf(kernel.elf "${KERNEL_LINK_OPTS}")
configure_kernel_like_elf(init.elf "${INIT_LINK_OPTS}")

target_include_directories(kernel.elf PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${GNU_EFI_SOURCE_DIR}/inc"
    "${GNU_EFI_SOURCE_DIR}/inc/x86_64"
)

target_include_directories(init.elf PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${GNU_EFI_SOURCE_DIR}/inc"
    "${GNU_EFI_SOURCE_DIR}/inc/x86_64"
)
