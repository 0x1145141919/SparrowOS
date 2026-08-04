#include "init/init_asset_registry.h"
// strncmp_in_kernel 经 boot.h → memory_base.h → util/lock.h → util/OS_utils.h 传递声明
// （与 kernel_mmu.cpp / init_init.cpp 同法，不直接 include 任何 OS_utils.h）

init_asset_registry_t* g_asset_registry = nullptr;

// ── 树键 = arg0（本名，首个空格前）──
static uint64_t asset_arg0_len(const char* s)
{
    uint64_t n = 0;
    while (s[n] && s[n] != ' ') n++;
    return n;
}

// arg0 字典序：先比公共前缀，再比 arg0 长度（前缀相同则短者在前）
static int asset_arg0_cmp(const char* a, const char* b)
{
    uint64_t la = asset_arg0_len(a);
    uint64_t lb = asset_arg0_len(b);
    int c = strncmp_in_kernel(a, b, la < lb ? la : lb);
    if (c != 0) return c;
    return (la > lb) ? 1 : (la < lb) ? -1 : 0;
}

int asset_entry_name_cmp(const asset_entry_t& a, const asset_entry_t& b)
{
    return asset_arg0_cmp(a.name, b.name);
}

bool init_asset_registry_t::add(const asset_entry_t& entry)
{
    return m_tree.insert(entry);
}

asset_entry_t* init_asset_registry_t::read(const char* name)
{
    if (!name) return nullptr;
    asset_entry_t probe = {};
    probe.name = const_cast<char*>(name);
    return m_tree.find(probe);
}

const asset_entry_t* init_asset_registry_t::read(const char* name) const
{
    if (!name) return nullptr;
    asset_entry_t probe = {};
    probe.name = const_cast<char*>(name);
    return m_tree.find(probe);
}

bool init_asset_registry_t::remove(const char* name)
{
    if (!name) return false;
    asset_entry_t probe = {};
    probe.name = const_cast<char*>(name);
    return m_tree.erase(probe);
}

bool init_asset_registry_t::contains(const char* name) const
{
    if (!name) return false;
    asset_entry_t probe = {};
    probe.name = const_cast<char*>(name);
    return m_tree.contains(probe);
}
