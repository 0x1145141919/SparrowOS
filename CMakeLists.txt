#CMakeLists.txt

cmake_minimum_required(VERSION 3.15)
project(Kernel LANGUAGES C CXX)

# =====================================================
# 模块化配置（拆分自根 CMakeLists）
include(cmake/toolchain.cmake)       # 工具链：NASM 发现、汇编器配置
include(cmake/compile-options.cmake) # 全局编译选项：标准、debug/release 模式
include(cmake/link-options.cmake)    # 链接选项：链接脚本、通用链接标志
include(cmake/interface-flags.cmake) # INTERFACE 库：可复用编译选项分组
# =====================================================

# 全局头文件路径（根 cmake 所有目标共享）
include_directories("${CMAKE_SOURCE_DIR}/src/include")

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

# =====================================================
# PlayOS UEFI Bootloader (EDK2)
# =====================================================
set(PLAYOSLOADER_DIR    ${CMAKE_SOURCE_DIR}/../edk2/PlayOSLoaderPkg)
set(PLAYOSLOADER_OUT
    ${PLAYOSLOADER_DIR}/BUILD/DEBUG_GCC5/X64/PlayOSLoaderPkg.efi)

# 计算 edk2 根目录相对 cmake 源目录的路径（用于 source edksetup.sh）
get_filename_component(EDK2_DIR ${CMAKE_SOURCE_DIR}/../edk2 ABSOLUTE)

# 用 add_custom_command 将 bootloader 的输出与源文件绑定
add_custom_command(
    OUTPUT  ${PLAYOSLOADER_OUT}
    COMMAND ${CMAKE_SOURCE_DIR}/build_bootloader.sh ${EDK2_DIR}

    COMMENT "Building PlayOS UEFI bootloader"
    WORKING_DIRECTORY ${EDK2_DIR}
)

add_custom_target(playos_loader
    DEPENDS ${PLAYOSLOADER_OUT}
)

add_custom_target(kernel_resources
)

add_dependencies(kernel_resources playos_loader)

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
)

# 获取gnu-efi构建目录路径
ExternalProject_Get_Property(gnu-efi SOURCE_DIR)
set(GNU_EFI_SOURCE_DIR ${SOURCE_DIR})

# gnu-efi 链接库 + 头文件接口
add_library(gnu_efi_iface INTERFACE)
target_link_libraries(gnu_efi_iface INTERFACE
    ${GNU_EFI_SOURCE_DIR}/x86_64/lib/libefi.a
    ${GNU_EFI_SOURCE_DIR}/x86_64/gnuefi/libgnuefi.a
)
target_include_directories(gnu_efi_iface INTERFACE
    "${GNU_EFI_SOURCE_DIR}/inc"
    "${GNU_EFI_SOURCE_DIR}/inc/x86_64"
)

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
    src/KImage_Introspection.cpp
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
    src/scheduler/bq_system.cpp
    src/arch/x86_64/exec_env_detect.cpp   
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
    src/arch/x86_64/boot/mem_init.cpp
    src/utils/BuddyControlBlock_foundation.cpp
    src/utils/BCB_fnd_DeepFirst.cpp 
)
add_executable(init.elf
    src/init/init_init.cpp
    src/init/kernel_mmu.cpp
    src/init/init_entry.asm
    src/init/Interrupts/Sysdef_exception_entries.asm
    src/init/Interrupts/exceptions_handler.cpp
    src/init/init_heap_v3.cpp
    src/utils/BuddyControlBlock_foundation.cpp
    src/init/core_hardwares/init_gop.cpp
    src/init/core_hardwares/PortDriver.cpp
    src/init/util/textConsole.cpp
    src/init/pages_alloc.cpp
    src/init/page_allocator.cpp
    src/init/initramfs_lookup.cpp
    src/init/panic.cpp
    src/init/util/kout.cpp
    src/utils/util.cpp
    src/utils/BCB_fnd_DeepFirst.cpp 
    src/arch/x86_64/Processor/early_bsp_resources.cpp 
    src/arch/x86_64/Processor/gs_complex_load_gdt_tss.cpp
    src/arch/x86_64/Processor/runtime_processor_regist.asm
    src/os_error_definitions.cpp
    src/init/info_fill.cpp
    src/init/mem_loads.cpp
)

# =====================================================
# kernel.elf
# =====================================================
target_link_libraries(kernel.elf PRIVATE
    gnu_efi_iface
    kspace_base_flags x86_kflags cpp_macro nasm_flags
)
target_link_options(kernel.elf PRIVATE ${KERNEL_LINK_OPTS})

# =====================================================
# init.elf
# =====================================================
target_link_libraries(init.elf PRIVATE
    gnu_efi_iface
    kspace_base_flags x86_kflags cpp_macro nasm_flags
)
target_link_options(init.elf PRIVATE ${INIT_LINK_OPTS})
