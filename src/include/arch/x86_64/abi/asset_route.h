#pragma once
#include "abi/asset_route.h"
#include "arch/x86_64/core_hardwares/primitive_gop_types.h"

// ════════════════════════════════════════════════════════════════
// x86_64 架构专属资产路由扩展
//
// 与 abi/asset_route.h 通用表合并解析（先通用后 arch）：
//   arg1 = "gop" → ASSET_KIND_PHYMEM_BLOB，desc_size = sizeof(GlobalBasicGraphicInfoType)
//
// 动机：GlobalBasicGraphicInfoType 是 x86_64 架构类型，塞进通用表会让 ABI 层
// 反向依赖 arch 类型；由 arch 表补充，保持通用表纯净。
//
// inline constexpr / inline：编译期常量，零运行时构造，跨 TU ODR 安全，落 .rodata。
// ════════════════════════════════════════════════════════════════

inline constexpr asset_route_entry_t x86_64_asset_route_table[] = {
    { "gop", ASSET_KIND_PHYMEM_BLOB, sizeof(GlobalBasicGraphicInfoType) },
};

inline constexpr uint32_t x86_64_asset_route_table_count() {
    return sizeof(x86_64_asset_route_table) / sizeof(x86_64_asset_route_table[0]);
}

// arg1 与类型名精确匹配（token 级，不带空格；arg1_len = 首个空格前长度）
inline bool asset_route_token_match(const char* arg1, uint64_t arg1_len, const char* type_name) {
    uint64_t tl = 0;
    while (type_name[tl]) ++tl;
    if (tl != arg1_len) return false;
    for (uint64_t k = 0; k < arg1_len; ++k)
        if (arg1[k] != type_name[k]) return false;
    return true;
}

// 合并解析：arg1（多arg name 首个空格后的 token）→ 路由条目；先通用表后 arch 表
inline const asset_route_entry_t* resolve_asset_route(const char* arg1) {
    if (!arg1) return nullptr;
    uint64_t l = 0;
    while (arg1[l] && arg1[l] != ' ') ++l;

    for (uint32_t i = 0; i < asset_route_table_count(); ++i)
        if (asset_route_token_match(arg1, l, asset_route_table[i].type_name))
            return &asset_route_table[i];
    for (uint32_t i = 0; i < x86_64_asset_route_table_count(); ++i)
        if (asset_route_token_match(arg1, l, x86_64_asset_route_table[i].type_name))
            return &x86_64_asset_route_table[i];
    return nullptr;
}
