#pragma once
#include "abi/boot.h"
#include "abi/asset_route.h"
#include "util/Ktemplats.h"

// ════════════════════════════════════════════════════════════════
// init_asset_registry_t — init.elf 侧资产树（handoff 清单）
//
// 与 kernel_mmu 树相互独立：
//   kernel_mmu 树管"我映射了什么"（映射台账，kmmu_entry_t）
//   资产树    管"我要交给 kernel.elf 什么"（handoff 清单，asset_entry_t）
// 二者可覆盖同一物理资产（如 GS 复合体），但语义不同、互不依赖。
// 同名资产两树同名——交叉溯源按 arg0（本名）匹配。
//
// 树键 = name 的 arg0（首个空格前），字典序。多arg 语法下 arg1+ 不参与排序。
// name/data 指针须在条目生命周期内有效（字符串字面量 / init 堆 desc）。
// ════════════════════════════════════════════════════════════════

// 比较器：仅按 arg0（本名）字典序，定义在 init_asset_registry.cpp
int asset_entry_name_cmp(const asset_entry_t& a, const asset_entry_t& b);

class init_asset_registry_t {
public:
    // 登记：同名（arg0）失败返回 false
    bool add(const asset_entry_t& entry);

    asset_entry_t*        read(const char* name);
    const asset_entry_t*  read(const char* name) const;

    // 按 arg0 删除
    bool remove(const char* name);
    bool contains(const char* name) const;

    size_t size() const { return m_tree.size(); }
    bool   empty() const { return m_tree.empty(); }

    using iterator       = Ktemplats::RBTree<asset_entry_t, asset_entry_name_cmp>::iterator;
    using const_iterator = Ktemplats::RBTree<asset_entry_t, asset_entry_name_cmp>::const_iterator;
    iterator       begin()       { return m_tree.begin(); }
    iterator       end()         { return m_tree.end(); }
    const_iterator begin() const { return m_tree.begin(); }
    const_iterator end()   const { return m_tree.end(); }

private:
    // 名字锚点红黑树：键 = arg0（本名），值存完整 name + data
    Ktemplats::RBTree<asset_entry_t, asset_entry_name_cmp> m_tree;
};

// 全局单例指针（init.elf 侧）。在 init_main 用 new 显式构造——全局裸指针
// 零动态初始化，不违反"禁止全局 C++ 构造"纪律。
extern init_asset_registry_t* g_asset_registry;

// 资产 desc blob 字节数（info_fill 序列化用）。
// 路由 = arg1 查表（通用表 abi/asset_route.h + arch 表 arch/x86_64/abi/asset_route.h 合并），
// blob 类型的大小看路由 desc_size，缺失时看 arg2（hex）。
// 未知路由 / data 空 / blob 缺大小 → 返回 0。
uint64_t asset_desc_size(const asset_entry_t& entry);
