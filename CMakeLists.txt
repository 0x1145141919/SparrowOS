#CMakeLists.txt

cmake_minimum_required(VERSION 3.15)
project(Kernel LANGUAGES C CXX)

# =====================================================
# 模块化配置（拆分自根 CMakeLists）
include(cmake/toolchain.cmake)       # 工具链：NASM 发现、汇编器配置
include(cmake/compile-options.cmake) # 全局编译选项：标准、debug/release 模式
include(cmake/link-options.cmake)    # 链接选项：链接脚本、通用链接标志
# =====================================================

# Initramfs builder target
add_subdirectory(build_utils/initramfs_builder)

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
    COMMAND ${CMAKE_BINARY_DIR}/symbols_dumper
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${CMAKE_BINARY_DIR}/symbols_dumper
    COMMENT "Generating kernel symbols table"
)
add_custom_target(initramfs 
    COMMAND ${CMAKE_BINARY_DIR}/build_utils/initramfs_builder/initramfs-builder initramfs_config.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${CMAKE_BINARY_DIR}/build_utils/initramfs_builder/initramfs-builder
    COMMENT "Building initramfs image"
)
add_dependencies(ksymbols symbols_dumper init.elf kernel.elf)

add_dependencies(kernel_resources initramfs) 
add_dependencies(initramfs ksymbols)

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
add_subdirectory(src/arch/x86_64/core_hardwares)
add_subdirectory(src/arch/x86_64/Processor)
add_subdirectory(src/arch/x86_64/Interrupts)
add_subdirectory(src/firmware)
add_subdirectory(src/fs)

add_executable(symbols_dumper
    build_utils/ksymbols_dumper.cpp
)

# 为 symbols_dumper 设置项目根路径编译宏
target_compile_definitions(symbols_dumper PRIVATE
    PJ_ROOT_PATH="${CMAKE_SOURCE_DIR}"
)

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
    src/utils/kshell.cpp
)

# 内核可执行文件
add_executable(kernel.elf
    src/utils/kshell_commands_regist.cpp
    src/arch/x86_64/panic.cpp
    src/arch/x86_64/boot/kinit.cpp
    src/utils/kcirclebufflogMgr.cpp
    src/arch/x86_64/boot/kernel_entry.asm
    src/arch/x86_64/boot/ap_init_pemode_exceptions.asm
    src/arch/x86_64/panic_processor_resources_shift.asm
    src/utils/int128_div.cpp
    src/arch/x86_64/time.cpp
    src/utils/textConsole.cpp
    src/memory/mem_kshell_commands.cpp
    src/arch/x86_64/Processor/early_bsp_resources.cpp
    src/scheduler/per_processor_scheduler.cpp
    src/scheduler/kthread_interfaces.cpp
    src/scheduler/yield.asm
    $<TARGET_OBJECTS:i8042_module>
    $<TARGET_OBJECTS:PCIe_module>
    $<TARGET_OBJECTS:x86_arch_module>
    $<TARGET_OBJECTS:Processor_module>
    $<TARGET_OBJECTS:Interrupts_module>
    $<TARGET_OBJECTS:firmware_module>
    $<TARGET_OBJECTS:Mem_module>
    ${SRC_COMMON}
    src/tests/kthreads/key_board_listen.cpp
    $<TARGET_OBJECTS:NVMe_module>
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

# =====================================================
# 内核/init 编译链接配置函数
# =====================================================
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
        -fno-rtti
        -static
        -mgeneral-regs-only
        -fno-lto
        -fpermissive
        -fno-PIC
        -fno-builtin-ctzll
        -DKERNEL_MODE>
        -DPGLV_4>
    $<$<COMPILE_LANGUAGE:ASM_NASM>:-felf64>
    )

    target_link_options(${target_name} PRIVATE ${link_opts}
        -fno-exceptions
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
