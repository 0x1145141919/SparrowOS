#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "abi/boot.h"
// ════════════════════════════════════════════════════════════════
// asset_route — 多arg name 的 arg1 路由表（纯数据字典，init/kernel 两世界共用）
//
// 多arg 语法："<arg0> <arg1> <arg2...>"
//   arg0 = 资产本名 → 红黑树键（锚点）
//   arg1 = void* 解释类型 → 查本表得 asset_kind / desc_size
//   arg2+ = 由 arg1 类型自行定义
//
// 路由表只决定"解释类型"（kind / desc_size）；具体解释逻辑留在各消费模块
// 按 kind 自处理——避免 abi 头反向依赖模块实现。
//
// inline constexpr 数组：编译期常量，零运行时构造（不违反无全局构造纪律），
// 跨 TU ODR 安全，落在 .rodata。
// ════════════════════════════════════════════════════════════════

enum asset_kind : uint8_t {
    ASSET_KIND_MEM_INTERVAL = 0,   // data → vm_interval
    ASSET_KIND_MOVABLE_FILE = 1,   // data → movable_file_entry_t
    ASSET_KIND_PHYMEM_BLOB  = 2,   // data → 裸 blob（大小看后续 arg）
    ASSET_KIND_ARCH         = 3,   // 架构复合（GS/hdstacks 等）
    ASSET_KIND_SCALAR       = 4,   // data → uint64 标量
    ASSET_KIND_PINTERVAL    = 5,   // data → p_interval（物理区间：ppn<<12 + 0 即资产本体；hd_stacks 用）
};
struct asset_entry_t {          // 是init.elf,kernel.elf以及中间transfer_pages共用的资产格式，虽然都是指针，在init.elf是物理指针，在transfer_pages存放的是偏移量，不过可以“链接”，也就是根据那个下面的基址重算后的可访问虚拟地址进行访问，kernel.elf处是内核虚拟地址
    char*     name;            //多arg语法，arg0是资产本名，arg1是void*解释类型，由一张专用的路由表决定，后续arg自定 
    void*     data;
};
struct asset_route_entry_t {
    const char* type_name;    // 多arg name 的 arg1
    uint8_t     kind;
    uint16_t    desc_size;    // 该类型 desc blob 字节数（包内重定位/校验用）
};

inline constexpr asset_route_entry_t asset_route_table[] = {
    { "mem",     ASSET_KIND_MEM_INTERVAL, sizeof(vm_interval) },
    { "movable", ASSET_KIND_MOVABLE_FILE, sizeof(movable_file_entry_t) },
    { "blob",    ASSET_KIND_PHYMEM_BLOB,  0 },
    { "arch",    ASSET_KIND_ARCH,         0 },
    { "scalar",  ASSET_KIND_SCALAR,       sizeof(uint64_t) },
    { "pint",    ASSET_KIND_PINTERVAL,    sizeof(p_interval) },
};

inline constexpr uint32_t asset_route_table_count() {
    return sizeof(asset_route_table) / sizeof(asset_route_table[0]);
}
