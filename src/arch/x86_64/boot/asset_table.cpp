#include "boot/asset_table.h"
#include "memory/kpoolmemmgr.h"
#include "arch/x86_64/abi/asset_route.h"
#include "util/OS_utils.h"

// ════════════════════════════════════════════════════════════════
// asset_table 实现 — kernel.elf 侧资产表（死简单：扁平数组 + 位图 + 单向状态机）
//
// pour 消费的是"已链接"的 analyzed 视图（info_pkg_link 产出）：pkg->properties_table
// 每条 name/data 已是包内真实线性地址，无需再按包基址重算。此处只做堆深拷贝。
// ════════════════════════════════════════════════════════════════

asset_table_t* g_asset_table = nullptr;

// ── 多arg name 辅助（kernel 侧独立实现，不跨世界依赖 init 代码）──

namespace {

uint64_t arg0_len_of(const char* s)
{
    uint64_t n = 0;
    while (s[n] && s[n] != ' ') n++;
    return n;
}

// full 是完整多arg 串，arg0 是本名 token：full 的 arg0 部分精确等于 arg0
bool arg0_eq(const char* full, const char* arg0)
{
    if (!full || !arg0) return false;
    const uint64_t al = arg0_len_of(arg0);
    if (arg0_len_of(full) != al) return false;
    return strncmp_in_kernel(full, arg0, al) == 0;
}

const char* asset_arg1(const char* name)
{
    if (!name) return nullptr;
    while (*name && *name != ' ') ++name;
    if (*name != ' ') return nullptr;
    return name + 1;
}

const char* asset_arg2(const char* name)
{
    const char* a1 = asset_arg1(name);
    if (!a1) return nullptr;
    while (*a1 && *a1 != ' ') ++a1;
    if (*a1 != ' ') return nullptr;
    return a1 + 1;
}

uint64_t parse_hex_u64(const char* s)
{
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

// 按 arg1 路由解析 desc blob 大小（与 init 侧 asset_desc_size 同语义）
uint64_t desc_size_of(const char* name, const asset_route_entry_t* r)
{
    if (!r) return 0;
    switch (r->kind) {
    case ASSET_KIND_MEM_INTERVAL: return sizeof(vm_interval);
    case ASSET_KIND_MOVABLE_FILE: return sizeof(movable_file_entry_t);
    case ASSET_KIND_PINTERVAL:    return sizeof(p_interval);
    case ASSET_KIND_SCALAR:       return sizeof(uint64_t);
    case ASSET_KIND_PHYMEM_BLOB:  return r->desc_size ? r->desc_size : parse_hex_u64(asset_arg2(name));
    case ASSET_KIND_ARCH:         return 0;   // 无当前消费方，允许空 desc
    default:                      return 0;
    }
}

} // namespace

// ── 生命周期 ────────────────────────────────────────────────────

asset_table_t* asset_table_t::create()
{
    // 第一堆未就绪时 kalloc 返回 nullptr（不走全局 operator new——其失败会 panic）
    KURD_t kurd;
    void*  mem = kpoolmemmgr_t::kalloc(sizeof(asset_table_t), kurd);
    if (!mem) return nullptr;
    return new (mem) asset_table_t();
}

loc_code_t asset_table_t::pour(const init_to_kernel_header_analyzed* pkg)
{
    if (!pkg || !pkg->properties_table || pkg->properties_count == 0)
        return SRC_LOC();

    const uint64_t n        = pkg->properties_count;
    const uint64_t bits_cnt = (n + 63) / 64;

    asset_table_entry* new_entries = new asset_table_entry[n];
    uint64_t*          new_bits    = new uint64_t[bits_cnt]();

    uint64_t inserted = 0;

    // 失败回滚：释放已插入条目的 name/data 与两数组
    auto rollback = [&](void) -> loc_code_t {
        for (uint64_t i = 0; i < inserted; ++i) {
            delete[] new_entries[i].name;
            delete[] static_cast<uint8_t*>(new_entries[i].data);
        }
        delete[] new_entries;
        delete[] new_bits;
        return SRC_LOC();
    };

    const asset_entry_t* props = pkg->properties_table;

    for (uint64_t i = 0; i < n; ++i) {
        const asset_entry_t& src = props[i];
        if (!src.name || !src.data) return rollback();

        const asset_route_entry_t* r = resolve_asset_route(asset_arg1(src.name));
        if (!r) return rollback();

        const uint64_t dsz = desc_size_of(src.name, r);
        if (dsz == 0 && r->kind != ASSET_KIND_ARCH) return rollback();
        if (dsz > 0xFFFFull) return rollback();   // desc_size 字段是 uint16_t

        // arg0 唯一性检查（真名任意两两不同）
        for (uint64_t j = 0; j < inserted; ++j)
            if (arg0_eq(new_entries[j].name, src.name)) return rollback();

        const uint64_t name_len = strlen_in_kernel(src.name);
        char* name_cp = new char[name_len + 1];
        ksystemramcpy(const_cast<char*>(src.name), name_cp, name_len + 1);

        void* data_cp = nullptr;
        if (dsz) {
            data_cp = new uint8_t[dsz];
            ksystemramcpy(src.data, data_cp, dsz);
        }

        new_entries[i] = asset_table_entry{
            name_cp,
            data_cp,
            r->kind,
            static_cast<uint16_t>(dsz),
            ASSET_STATE_PENDING,
        };
        new_bits[i / 64] |= (1ull << (i % 64));
        ++inserted;
    }

    entries      = new_entries;
    valid_bits   = new_bits;
    count        = n;
    pending_left = n;
    return 0;
}

loc_code_t asset_table_t::dispose()
{
    if (pending_left != 0) return SRC_LOC();
    delete[] entries;
    delete[] valid_bits;
    delete this;
    return 0;
}

// ── 消费 ────────────────────────────────────────────────────────

const asset_table_entry* asset_table_t::read(const char* arg0) const
{
    if (!arg0) return nullptr;
    for (uint64_t i = 0; i < count; ++i) {
        if (!(valid_bits[i / 64] & (1ull << (i % 64)))) continue;  // 已 deal，跳过
        if (arg0_eq(entries[i].name, arg0)) return &entries[i];
    }
    return nullptr;
}

loc_code_t asset_table_t::deal(const char* full_name)
{
    if (!full_name) return SRC_LOC();
    for (uint64_t i = 0; i < count; ++i) {
        if (!(valid_bits[i / 64] & (1ull << (i % 64)))) continue;  // 已 deal
        if (strcmp_in_kernel(entries[i].name, full_name) != 0) continue;

        delete[] entries[i].name;
        delete[] static_cast<uint8_t*>(entries[i].data);
        entries[i].name  = nullptr;
        entries[i].data  = nullptr;
        entries[i].state = ASSET_STATE_DEALED;
        valid_bits[i / 64] &= ~(1ull << (i % 64));
        --pending_left;
        return 0;
    }
    return SRC_LOC();
}
