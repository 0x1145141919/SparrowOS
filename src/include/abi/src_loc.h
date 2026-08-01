#pragma once
// ════════════════════════════════════════════════════════════════
// src_loc — 源码位置定位码（迭代开发期的错误极速定位工具）
//
// 设计意图：
//   开发期一边构思代码一边定义错误树（KURD）既不人类友好、更不 AI 友好。
//   因此引入一个轻量"位置戳"：把 (编译期路径 hash, 行号) 压进一个 u64，
//   出错点直接 SRC_LOC() 返回，不需要预先编排任何语义错误树。
//
//   u64 布局：
//     [0:19]   = 行号（20 bit，上限 1,048,575 行）
//     [20:63]  = 路径 hash（44 bit，FNV-1a 64 掩码）
//     全 0     = 保留为"无错误/成功"（行号从 1 起，天然不会撞）
//
//   使用方：init.elf 与 kernel.elf 共用。decode 只发生在宿主端：
//   出错时 halt 打印魔法值 → 宿主脚本经 srcloc.json 二分查表还原 path:line。
//   srcloc.json 是纯宿主构建产物，绝不进入 initramfs / 映像。
// ════════════════════════════════════════════════════════════════
#include <stdint.h>

using loc_code_t = uint64_t;

namespace src_loc {

// 常量：行号掩码（20 bit）、hash 掩码（44 bit）
constexpr uint64_t LINE_MASK   = 0xFFFFFull;          // [0:19]
constexpr uint64_t LINE_SHIFT  = 20;
constexpr uint64_t HASH_MASK   = 0xFFFFFFFFFFFull;    // [20:63]，44 bit

// FNV-1a 64 位，掩到 44 bit。确定性、可离线复算。
// 注意：输入必须是编译器看到的 __FILE__ 字符串（当前 CMake 传绝对路径），
// 宿主 dumper 用 PJ_ROOT + "/" + 相对路径 复算同一字符串。
constexpr uint64_t hash44(const char* s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ull;
    }
    return h & HASH_MASK;
}

// 编码：路径 hash << 20 | 行号
constexpr loc_code_t make(const char* file, uint32_t line)
{
    return ((loc_code_t)hash44(file) << LINE_SHIFT) | ((loc_code_t)line & LINE_MASK);
}

// 解码：行号
constexpr uint32_t line_of(loc_code_t loc)
{
    return (uint32_t)(loc & LINE_MASK);
}

// 解码：路径 hash
constexpr uint64_t hash_of(loc_code_t loc)
{
    return loc >> LINE_SHIFT;
}

} // namespace src_loc

// 出错点盖戳宏：取当前文件 __FILE__（编译器看到的路径）+ 当前行号
#define SRC_LOC() (src_loc::make(__FILE__, __LINE__))
