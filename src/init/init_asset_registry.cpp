#include "init/init_asset_registry.h"
#include "arch/x86_64/abi/asset_route.h"
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

// ── 路由解析 + desc 大小（通用表 + arch 表合并，供 info_fill 序列化）──

// arg1 = 首个空格后第一个 token；arg2 = 第二个空格后
static const char* asset_arg1(const char* name) {
    if (!name) return nullptr;
    while (*name && *name != ' ') ++name;
    if (*name != ' ') return nullptr;
    return name + 1;
}

static const char* asset_arg2(const char* name) {
    const char* a1 = asset_arg1(name);
    if (!a1) return nullptr;
    while (*a1 && *a1 != ' ') ++a1;
    if (*a1 != ' ') return nullptr;
    return a1 + 1;
}

// 解析十六进制 u64（blob 的 arg2 大小；允许 "0x" 前缀）
static uint64_t parse_hex_u64(const char* s) {
    if (!s) return 0;
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; ++s) {
        const char c = *s;
        uint64_t d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d;
    }
    return v;
}

uint64_t asset_desc_size(const asset_entry_t& entry)
{
    if (!entry.name || !entry.data) return 0;
    const asset_route_entry_t* r = resolve_asset_route(asset_arg1(entry.name));
    if (!r) return 0;
    switch (r->kind) {
    case ASSET_KIND_MEM_INTERVAL: return sizeof(vm_interval);
    case ASSET_KIND_MOVABLE_FILE: return sizeof(movable_file_entry_t);
    case ASSET_KIND_PINTERVAL:    return sizeof(p_interval);
    case ASSET_KIND_SCALAR:       return sizeof(uint64_t);
    case ASSET_KIND_PHYMEM_BLOB:  return r->desc_size ? r->desc_size : parse_hex_u64(asset_arg2(entry.name));
    case ASSET_KIND_ARCH:         return 0;   // 无当前消费方
    default:                      return 0;
    }
}
