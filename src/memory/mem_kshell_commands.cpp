/**
 * @file mem_kshell_commands.cpp
 * @brief kshell 内存操作命令实现
 *
 * 遵行 Docs/kshell_memory_operations_design.md。
 * 包含 palloc/pfree/valloc/vfree/pread/pwrite/pmap/punmap/dmap/stackalloc。
 * 零 libc/libc++ 依赖。
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "util/lock.h"
#include "util/OS_utils.h"
#include "arch/x86_64/core_hardwares/i8042.h"
#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "memory/kpoolmemmgr.h"
#include "memory/AddresSpace.h"

using namespace kio;
using namespace INFR_LOCATIONS::KSHELL_EVENTS::COMMON_FAIL_REASONS;

// ── 辅助 ────────────────────────────────────────────────────────

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

static bool tok_eq(const token_t& t, const char* s) {
    size_t n = strlen_in_kernel(s);
    return (t.len == n) && (strncmp(t.str, s, n) == 0);
}

static int parse_uint(const token_t& t, uint64_t* out) {
    if (t.len == 0 || t.len > 20) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < t.len; i++) {
        if (t.str[i] < '0' || t.str[i] > '9') return -1;
        v = v * 10 + (uint64_t)(t.str[i] - '0');
    }
    *out = v;
    return 0;
}

/** Hand-rolled hex parser for fixed-length strings */
static int parse_hex_len(const char* s, size_t len, uint64_t* out) {
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')       v = (v << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f')  v = (v << 4) | (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v = (v << 4) | (uint64_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int parse_hex(const token_t& t, uint64_t* out) {
    if (t.len == 0 || t.len > 16) return -1;
    if (t.len >= 2 && t.str[0] == '0' && (t.str[1] == 'x' || t.str[1] == 'X')) {
        return parse_hex_len(t.str + 2, t.len - 2, out);
    }
    uint64_t v = 0;
    for (size_t i = 0; i < t.len; i++) {
        char c = t.str[i];
        if (c >= '0' && c <= '9')       v = (v << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f')  v = (v << 4) | (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v = (v << 4) | (uint64_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')       v = (v << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f')  v = (v << 4) | (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v = (v << 4) | (uint64_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static bool confirm_y(const char* prompt) {
    bsp_kout << "[WARNING] " << prompt << kendl;
    bsp_kout << "Type 'y' to confirm: ";
    char c = i8042_blockable_keyboard_listening();
    bsp_kout << kendl;
    return (c == 'y' || c == 'Y');
}

static bool confirm_yes(const char* prompt) {
    bsp_kout << "[DANGEROUS] " << prompt << kendl;
    bsp_kout << "Type 'YES' to confirm: ";
    char buf[8];
    size_t pos = 0;
    for (int i = 0; i < 3; i++) {
        char c = i8042_blockable_keyboard_listening();
        if (c == '\r' || c == '\n') break;
        bsp_kout << c;
        if (pos < 7) buf[pos++] = c;
    }
    buf[pos] = '\0';
    bsp_kout << kendl;
    return (strcmp_in_kernel(buf, "YES") == 0);
}

// ── 分配历史记录 ──────────────────────────────────────────────

enum class alloc_type_t : uint8_t { PHYS, VIRT, STACK };

struct alloc_record_t {
    bool      used;
    alloc_type_t type;
    uint64_t  base;     // 物理地址或虚拟地址
    uint64_t  size;     // 字节数或页数（根据 type）
    union {
        uint8_t pages;   // 页数（valloc/stackalloc）
        struct {
            uint8_t order;      // buddy order
            uint8_t align_log2; // 对齐参数
        } phys;
    } meta;
};

static constexpr uint16_t MAX_RECORDS = 256;
static alloc_record_t g_records[MAX_RECORDS];

static int alloc_record_find(uint64_t base, alloc_type_t type) {
    for (uint16_t i = 0; i < MAX_RECORDS; i++) {
        if (g_records[i].used && g_records[i].type == type &&
            g_records[i].base == base) return (int)i;
    }
    return -1;
}

static int alloc_record_add(alloc_type_t type, uint64_t base, uint64_t size) {
    for (uint16_t i = 0; i < MAX_RECORDS; i++) {
        if (!g_records[i].used) {
            g_records[i].used = true;
            g_records[i].type = type;
            g_records[i].base = base;
            g_records[i].size = size;
            return (int)i;
        }
    }
    return -1;
}

static void alloc_record_remove(int idx) {
    if (idx >= 0 && idx < MAX_RECORDS) {
        g_records[idx].used = false;
    }
}

static bool force_flag(const line_t* line) {
    for (uint16_t i = 0; i < line->token_count; i++) {
        if (tok_eq(line->tokens[i], "-f")) return true;
    }
    return false;
}

// Helper: approximate size → order
static uint8_t size_to_order_calc(uint64_t size) {
    uint8_t order = 0;
    while ((1ULL << (order + 12)) < size) order++;
    return order;
}

// ── palloc ─────────────────────────────────────────────────────

KURD_t cmd_palloc(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: palloc <size_bytes> [align_log2] [type]" << kendl;
        return ok;
    }

    uint64_t size;
    if (parse_uint(line->tokens[1], &size) || (size & 0xFFF) != 0 || size == 0) {
        bsp_kout << "[ERROR] size must be >0 and page-aligned (4KB multiple)" << kendl;
        return ok;
    }

    uint64_t align = 0;
    if (line->token_count >= 3) {
        if (parse_uint(line->tokens[2], &align) || align < 12 || align > 30) {
            bsp_kout << "[ERROR] align_log2 must be 12-30" << kendl;
            return ok;
        }
    }

    if (!confirm_y("Allocate physical pages?")) {
        bsp_kout << "[palloc] Cancelled." << kendl;
        return ok;
    }

    buddy_alloc_params params;
    params.align_log2 = (uint8_t)align;
    params.numa = 0;
    params.try_lock_always_try = false;

    KURD_t kurd;
    phyaddr_t pa = FreePagesAllocator::alloc(size, params, page_state_t::allocable, kurd);
    if (pa == FreePagesAllocator::INVALID_ALLOC_BASE) {
        bsp_kout << "[ERROR] palloc failed (result=" << kurd.result
                 << " reason=" << kurd.reason << ")" << kendl;
        return ok;
    }

    int idx = alloc_record_add(alloc_type_t::PHYS, pa, size);
    bsp_kout << "[palloc] phyaddr=0x" << HEX << pa << DEC
             << "  order=" << (uint64_t)size_to_order_calc(size)
             << "  size=" << size << "  record_idx=" << idx << kendl;
    return ok;
}

// ── pfree ─────────────────────────────────────────────────────

KURD_t cmd_pfree(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pfree <phyaddr> <size_bytes> [-f]" << kendl;
        return ok;
    }

    uint64_t pa, size;
    if (parse_hex(line->tokens[1], &pa) || parse_uint(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid address or size" << kendl;
        return ok;
    }

    int idx = alloc_record_find(pa, alloc_type_t::PHYS);
    if (idx < 0) {
        bsp_kout << "[ERROR] Address not found in allocation history" << kendl;
        return ok;
    }

    bool ff = force_flag(line);
    if (!ff && !confirm_yes("This will FREE physical pages!")) {
        bsp_kout << "[pfree] Cancelled." << kendl;
        return ok;
    }

    KURD_t kr = FreePagesAllocator::free(pa, size);
    if (error_kurd(kr)) {
        bsp_kout << "[ERROR] pfree failed (reason=" << kr.reason << ")" << kendl;
        return ok;
    }

    alloc_record_remove(idx);
    bsp_kout << "[pfree] Freed phyaddr=0x" << HEX << pa << DEC << " size=" << size << kendl;
    return ok;
}

// ── valloc ─────────────────────────────────────────────────────

KURD_t cmd_valloc(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: valloc <pages_count> [align_log2] [type]" << kendl;
        return ok;
    }

    uint64_t pages;
    if (parse_uint(line->tokens[1], &pages) || pages == 0 || pages > 0x100000) {
        bsp_kout << "[ERROR] pages must be 1-1048576" << kendl;
        return ok;
    }

    uint64_t align = 0;
    if (line->token_count >= 3) {
        if (parse_uint(line->tokens[2], &align) || align > 30) {
            bsp_kout << "[ERROR] invalid align_log2" << kendl;
            return ok;
        }
    }

    if (!confirm_y("Allocate virtual pages?")) {
        bsp_kout << "[valloc] Cancelled." << kendl;
        return ok;
    }

    KURD_t kurd;
    void* va = __wrapped_pgs_valloc(&kurd, pages, page_state_t::allocable, (uint8_t)align);
    if (error_kurd(kurd) || va == nullptr) {
        bsp_kout << "[ERROR] valloc failed" << kendl;
        return ok;
    }

    int idx = alloc_record_add(alloc_type_t::VIRT, (uint64_t)va, pages);
    bsp_kout << "[valloc] vaddr=0x" << HEX << (uint64_t)va << DEC
             << "  pages=" << pages
             << "  record_idx=" << idx << kendl;
    return ok;
}

// ── vfree ─────────────────────────────────────────────────────

KURD_t cmd_vfree(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: vfree <vaddr> <pages_count> [-f]" << kendl;
        return ok;
    }

    uint64_t va, pages;
    if (parse_hex(line->tokens[1], &va) || parse_uint(line->tokens[2], &pages)) {
        bsp_kout << "[ERROR] Invalid address or page count" << kendl;
        return ok;
    }

    int idx = alloc_record_find(va, alloc_type_t::VIRT);
    if (idx < 0) {
        bsp_kout << "[ERROR] Address not found in allocation history" << kendl;
        return ok;
    }

    if (!force_flag(line) && !confirm_yes("This will FREE virtual pages!")) {
        bsp_kout << "[vfree] Cancelled." << kendl;
        return ok;
    }

    KURD_t kr = __wrapped_pgs_vfree((void*)va, pages);
    if (error_kurd(kr)) {
        bsp_kout << "[ERROR] vfree failed (reason=" << kr.reason << ")" << kendl;
        return ok;
    }

    alloc_record_remove(idx);
    bsp_kout << "[vfree] Freed vaddr=0x" << HEX << va << DEC << " pages=" << pages << kendl;
    return ok;
}

// ── pread ─────────────────────────────────────────────────────

KURD_t cmd_pread(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pread <phyaddr> <size_bytes> [hex/dec/ascii]" << kendl;
        return ok;
    }

    uint64_t pa, size;
    if (parse_hex(line->tokens[1], &pa) || parse_uint(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid address or size" << kendl;
        return ok;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        bsp_kout << "[ERROR] size must be 1/2/4/8 bytes" << kendl;
        return ok;
    }

    enum { HEX, DEC, ASCII } fmt = HEX;
    if (line->token_count >= 4) {
        if (tok_eq(line->tokens[3], "dec"))   fmt = DEC;
        else if (tok_eq(line->tokens[3], "ascii")) fmt = ASCII;
    }

    // 通过临时只读映射读取物理地址
    vm_interval iv;
    iv.vbase = 0;
    iv.pbase = pa & ~0xFFFULL;
    iv.size  = 0x2000; // 2 pages to cover unaligned access
    iv.access = KspacePageTable::PG_R;

    KURD_t map_kurd;
    vaddr_t mapped = phyaddr_direct_map(&iv, &map_kurd);
    if (mapped == 0 || error_kurd(map_kurd)) {
        bsp_kout << "[ERROR] Failed to map physical address" << kendl;
        return ok;
    }

    uint64_t offset = pa & 0xFFF;
    uint64_t value = 0;
    uint8_t* src = (uint8_t*)(mapped + offset);
    for (uint64_t i = 0; i < size; i++) {
        value |= ((uint64_t)src[i]) << (i * 8);
    }

    vm_interval unmap_iv;
    unmap_iv.vbase = mapped;
    unmap_iv.pbase = iv.pbase;
    unmap_iv.size  = iv.size;
    phyaddr_direct_unmap(&unmap_iv, iv.size);

    bsp_kout << "[pread] phyaddr=0x" << HEX << pa << DEC << " (size=" << size << "): ";
    if (fmt == HEX) {
        bsp_kout << "0x" << HEX << value << DEC;
    } else if (fmt == DEC) {
        bsp_kout << value;
    } else {
        for (uint64_t i = 0; i < size; i++) {
            char c = (char)((value >> (i * 8)) & 0xFF);
            if (c >= 0x20 && c < 0x7F) bsp_kout << c;
            else bsp_kout << '.';
        }
    }
    bsp_kout << kendl;
    return ok;
}

// ── pwrite ────────────────────────────────────────────────────

KURD_t cmd_pwrite(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pwrite <phyaddr> <value> [size] [-f]" << kendl;
        return ok;
    }

    uint64_t pa, value;
    if (parse_hex(line->tokens[1], &pa) || parse_hex(line->tokens[2], &value)) {
        bsp_kout << "[ERROR] Invalid address or value" << kendl;
        return ok;
    }

    uint64_t size = 4;
    if (line->token_count >= 4) {
        // Check if token is a size or -f
        if (tok_eq(line->tokens[3], "-f")) goto do_pwrite;
        uint64_t sz;
        if (parse_uint(line->tokens[3], &sz) == 0) {
            if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
                bsp_kout << "[ERROR] size must be 1/2/4/8" << kendl;
                return ok;
            }
            size = sz;
        }
    }

do_pwrite:
    if (!force_flag(line) && !confirm_yes("This will WRITE to physical memory!")) {
        bsp_kout << "[pwrite] Cancelled." << kendl;
        return ok;
    }

    vm_interval iv;
    iv.vbase = 0;
    iv.pbase = pa & ~0xFFFULL;
    iv.size  = 0x2000;
    iv.access = KspacePageTable::PG_RW;

    KURD_t mk;
    vaddr_t mapped = phyaddr_direct_map(&iv, &mk);
    if (mapped == 0 || error_kurd(mk)) {
        bsp_kout << "[ERROR] Failed to map physical address" << kendl;
        return ok;
    }

    uint64_t offset = pa & 0xFFF;
    uint8_t* dst = (uint8_t*)(mapped + offset);
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = (uint8_t)((value >> (i * 8)) & 0xFF);
    }

    vm_interval um;
    um.vbase = mapped;
    um.pbase = iv.pbase;
    um.size  = iv.size;
    phyaddr_direct_unmap(&um, iv.size);

    bsp_kout << "[pwrite] Wrote 0x" << HEX << value << DEC
             << " to phyaddr=0x" << HEX << pa << DEC
             << " (size=" << size << ")" << kendl;
    return ok;
}

// ── pmap ──────────────────────────────────────────────────────

KURD_t cmd_pmap(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pmap <phyaddr> <vaddr|0> <size_bytes> [access]" << kendl;
        return ok;
    }

    uint64_t pa, va, size;
    if (parse_hex(line->tokens[1], &pa) || parse_hex(line->tokens[2], &va) ||
        parse_uint(line->tokens[3], &size)) {
        bsp_kout << "[ERROR] Invalid parameters" << kendl;
        return ok;
    }

    pgaccess access = KspacePageTable::PG_RW;
    if (line->token_count >= 5) {
        const token_t& a = line->tokens[4];
        if (tok_eq(a, "R"))    access = KspacePageTable::PG_R;
        else if (tok_eq(a, "RX")) {
            access = KspacePageTable::PG_RW;
            access.exec = 1;
            access.write = 0;
            access.read = 1;
        }
        else if (tok_eq(a, "RW"))  access = KspacePageTable::PG_RW;
        else if (tok_eq(a, "RWX")) access = KspacePageTable::PG_RWX;
    }

    if (!confirm_y("Create physical→virtual mapping?")) {
        bsp_kout << "[pmap] Cancelled." << kendl;
        return ok;
    }

    vm_interval iv;
    iv.vbase = va;  // 0 = auto-allocate
    iv.pbase = pa;
    iv.size  = size;
    iv.access = access;

    KURD_t mk;
    vaddr_t mapped = phyaddr_direct_map(&iv, &mk);
    if (mapped == 0 || error_kurd(mk)) {
        bsp_kout << "[ERROR] pmap failed" << kendl;
        return ok;
    }

    bsp_kout << "[pmap] phyaddr=0x" << HEX << pa << DEC
             << " → vaddr=0x" << HEX << mapped << DEC
             << " size=" << size << kendl;
    return ok;
}

// ── punmap ────────────────────────────────────────────────────

KURD_t cmd_punmap(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: punmap <vaddr> <size_bytes> [-f]" << kendl;
        return ok;
    }

    uint64_t va, size;
    if (parse_hex(line->tokens[1], &va) || parse_uint(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid parameters" << kendl;
        return ok;
    }

    if (!force_flag(line) && !confirm_yes("This will UNMAP virtual pages!")) {
        bsp_kout << "[punmap] Cancelled." << kendl;
        return ok;
    }

    vm_interval iv;
    iv.vbase = va;
    iv.pbase = 0;
    iv.size  = size;
    iv.access = KspacePageTable::PG_R;

    KURD_t kr = phyaddr_direct_unmap(&iv, size);
    if (error_kurd(kr)) {
        bsp_kout << "[ERROR] punmap failed (reason=" << kr.reason << ")" << kendl;
        return ok;
    }

    bsp_kout << "[punmap] Unmapped vaddr=0x" << HEX << va << DEC
             << " size=" << size
             << " [WARNING] Accessing this address will page fault" << kendl;
    return ok;
}

// ── dmap ──────────────────────────────────────────────────────

KURD_t cmd_dmap(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: dmap <phyaddr> <size_bytes>" << kendl;
        return ok;
    }

    uint64_t pa, size;
    if (parse_hex(line->tokens[1], &pa) || parse_uint(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid parameters" << kendl;
        return ok;
    }

    if (!confirm_y("Create direct physical mapping?")) {
        bsp_kout << "[dmap] Cancelled." << kendl;
        return ok;
    }

    vm_interval iv;
    iv.vbase = 0;
    iv.pbase = pa;
    iv.size  = size;
    iv.access = KspacePageTable::PG_RW;

    KURD_t mk;
    vaddr_t mapped = phyaddr_direct_map(&iv, &mk);
    if (mapped == 0 || error_kurd(mk)) {
        bsp_kout << "[ERROR] dmap failed" << kendl;
        return ok;
    }

    bsp_kout << "[dmap] phyaddr=0x" << HEX << pa << DEC
             << " → vaddr=0x" << HEX << mapped << DEC
             << " size=" << size << kendl;
    return ok;
}

// ── stackalloc ────────────────────────────────────────────────

KURD_t cmd_stackalloc(const line_t* line) {
    KURD_t ok = make_ok();
    ok.event_code = INFR_LOCATIONS::KSHELL_EVENTS::COMMAND_EXECUTE;

    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: stackalloc <pages_count>" << kendl;
        return ok;
    }

    uint64_t pages;
    if (parse_uint(line->tokens[1], &pages) || pages == 0 || pages > 256) {
        bsp_kout << "[ERROR] pages must be 1-256" << kendl;
        return ok;
    }

    KURD_t kurd;
    vaddr_t stack_bottom = stack_alloc(&kurd, pages);
    if (error_kurd(kurd) || stack_bottom == 0) {
        bsp_kout << "[ERROR] stack_alloc failed" << kendl;
        return ok;
    }

    // stack_alloc 返回栈底（高地址），之下有 pages 页 + 1 页缓冲
    vaddr_t stack_top = stack_bottom - (pages * 4096);
    vaddr_t guard_end = stack_top - 4096;

    int idx = alloc_record_add(alloc_type_t::STACK, stack_bottom, pages);

    bsp_kout << "[stackalloc] top=0x" << HEX << stack_top << DEC
             << "  bottom=0x" << HEX << stack_bottom << DEC
             << "  guard_page=0x" << HEX << guard_end << DEC
             << "  record_idx=" << idx << kendl;
    return ok;
}
