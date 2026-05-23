/**
 * @file mem_kshell_commands.cpp
 * @brief kshell 内存操作命令实现
 *
 * 遵行 Docs/kshell_memory_operations_design.md。
 * 包含 palloc/pfree/valloc/vfree/pread/pwrite/pmap/punmap/dmap/stackalloc。
 * 零 libc/libc++ 依赖。
 *
 * @note KURD 纪律：所有调用内存后端的命令直接返回后端的一手 KURD，
 *       不使用 INFR_LOCATIONS::KSHELL 封装伪造的 KURD。
 *       命令业务层不做分配释放记忆——内存业务层自洽管理。
 */
#include "util/kshell.h"
#include "util/kout.h"
#include "util/lock.h"
#include "util/OS_utils.h"
#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "memory/kpoolmemmgr.h"
#include "memory/AddresSpace.h"

using namespace kio;

// ── 辅助 ────────────────────────────────────────────────────────

static bool tok_eq(const token_t& t, const char* s) {
    size_t n = strlen_in_kernel(s);
    return (t.len == n) && (strcmp_in_kernel(t.str, s, n) == 0);
}

// ── palloc ─────────────────────────────────────────────────────

KURD_t cmd_palloc(const line_t* line) {
    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: palloc <size_bytes> [align_log2] [type]" << kendl;
        return KURD_t{};
    }

    uint64_t size;
    if (!token_to_uint64(line->tokens[1], &size) || (size & 0xFFF) != 0 || size == 0) {
        bsp_kout << "[ERROR] size must be >0 and page-aligned (4KB multiple)" << kendl;
        return KURD_t{};
    }

    uint64_t align = 0;
    if (line->token_count >= 3) {
        if (!token_to_uint64(line->tokens[2], &align) || align < 12 || align > 30) {
            bsp_kout << "[ERROR] align_log2 must be 12-30" << kendl;
            return KURD_t{};
        }
    }

    buddy_alloc_params params;
    params.align_log2 = (uint8_t)align;
    params.numa = 0;
    params.try_lock_always_try = false;

    KURD_t kurd;
    phyaddr_t pa = FreePagesAllocator::alloc(size, params, page_state_t::kernel_pinned, kurd);
    if (pa == FreePagesAllocator::INVALID_ALLOC_BASE) {
        bsp_kout << "[ERROR] palloc failed (result=" << kurd.result
                 << " reason=" << kurd.reason << ")" << kendl;
        return kurd;
    }

    bsp_kout << "[palloc] phyaddr=0x" << HEX << pa << DEC
             << "  size=" << size << kendl;
    return kurd;
}

// ── pfree ─────────────────────────────────────────────────────

KURD_t cmd_pfree(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pfree <phyaddr> <size_bytes>" << kendl;
        return KURD_t{};
    }

    uint64_t pa, size;
    if (!token_to_uint64(line->tokens[1], &pa) || !token_to_uint64(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid address or size" << kendl;
        return KURD_t{};
    }

    KURD_t kr = FreePagesAllocator::free(pa, size);
    if (error_kurd(kr)) {
        bsp_kout << "[ERROR] pfree failed (reason=" << kr.reason << ")" << kendl;
        return kr;
    }

    bsp_kout << "[pfree] Freed phyaddr=0x" << HEX << pa << DEC << " size=" << size << kendl;
    return kr;
}

// ── valloc ─────────────────────────────────────────────────────

KURD_t cmd_valloc(const line_t* line) {
    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: valloc <pages_count> [align_log2] [type]" << kendl;
        return KURD_t{};
    }

    uint64_t pages;
    if (!token_to_uint64(line->tokens[1], &pages) || pages == 0 || pages > 0x100000) {
        bsp_kout << "[ERROR] pages must be 1-1048576" << kendl;
        return KURD_t{};
    }

    uint64_t align = 0;
    if (line->token_count >= 3) {
        if (!token_to_uint64(line->tokens[2], &align) || align > 30) {
            bsp_kout << "[ERROR] invalid align_log2" << kendl;
            return KURD_t{};
        }
    }

    KURD_t kurd;
    void* va = __wrapped_pgs_valloc(&kurd, pages, page_state_t::kernel_pinned, (uint8_t)align);
    if (error_kurd(kurd) || va == nullptr) {
        bsp_kout << "[ERROR] valloc failed (result=" << kurd.result
                 << " reason=" << kurd.reason << ")" << kendl;
        return kurd;
    }

    bsp_kout << "[valloc] vaddr=0x" << HEX << (uint64_t)va << DEC
             << "  pages=" << pages << kendl;
    return kurd;
}

// ── vfree ─────────────────────────────────────────────────────

KURD_t cmd_vfree(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: vfree <vaddr> <pages_count>" << kendl;
        return KURD_t{};
    }

    uint64_t va, pages;
    if (!token_to_uint64(line->tokens[1], &va) || !token_to_uint64(line->tokens[2], &pages)) {
        bsp_kout << "[ERROR] Invalid address or page count" << kendl;
        return KURD_t{};
    }

    KURD_t kr = __wrapped_pgs_vfree((void*)va, pages);
    if (error_kurd(kr)) {
        bsp_kout << "[ERROR] vfree failed (reason=" << kr.reason << ")" << kendl;
        return kr;
    }

    bsp_kout << "[vfree] Freed vaddr=0x" << HEX << va << DEC << " pages=" << pages << kendl;
    return kr;
}

// ── pread ─────────────────────────────────────────────────────

KURD_t cmd_pread(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pread <phyaddr> <size_bytes> [hex/dec/ascii]" << kendl;
        return KURD_t{};
    }

    uint64_t pa, size;
    if (!token_to_uint64(line->tokens[1], &pa) || !token_to_uint64(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid address or size" << kendl;
        return KURD_t{};
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        bsp_kout << "[ERROR] size must be 1/2/4/8 bytes" << kendl;
        return KURD_t{};
    }

    numer_system_select fmt = HEX;
        if (tok_eq(line->tokens[3], "dec"))   fmt = DEC;


    // 通过临时只读映射读取物理地址
    pgaccess rd_acc = {1,0,1,0,1,WB};  // kernel, R, no X
    vm_interval iv = {
        .vpn = 0,
        .ppn = (pa & ~0xFFFULL) >> 12,
        .npages = 2,
        .access = rd_acc
    };
    KURD_t map_kurd;
    vaddr_t mapped = Kspace_pinterval_alloc_and_map(iv, &map_kurd);
    if (mapped == 0 || error_kurd(map_kurd)) {
        bsp_kout << "[ERROR] Failed to map physical address" << kendl;
        return map_kurd;
    }

    uint64_t offset = pa & 0xFFF;
    uint64_t value = 0;
    uint8_t* src = (uint8_t*)(mapped + offset);
    for (uint64_t i = 0; i < size; i++) {
        value |= ((uint64_t)src[i]) << (i * 8);
    }

    vm_interval unmap_iv = {
        .vpn = mapped >> 12,
        .ppn = iv.ppn,
        .npages = 2,
        .access = rd_acc
    };
    KURD_t unmap_kurd = Kspace_phyaddr_direct_unmap(unmap_iv);

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

    if (error_kurd(unmap_kurd)) return unmap_kurd;
    return map_kurd;
}

// ── pwrite ────────────────────────────────────────────────────

KURD_t cmd_pwrite(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pwrite <phyaddr> <value> [size]" << kendl;
        return KURD_t{};
    }

    uint64_t pa, value;
    if (!token_to_uint64(line->tokens[1], &pa) || !token_to_uint64(line->tokens[2], &value)) {
        bsp_kout << "[ERROR] Invalid address or value" << kendl;
        return KURD_t{};
    }

    uint64_t size = 4;
    if (line->token_count >= 4) {
        uint64_t sz;
        if (token_to_uint64(line->tokens[3], &sz)) {
            if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
                bsp_kout << "[ERROR] size must be 1/2/4/8" << kendl;
                return KURD_t{};
            }
            size = sz;
        }
    }

    pgaccess rw_acc = {1,1,1,0,1,WB};  // kernel, RW, no X
    vm_interval iv = {
        .vpn = 0,
        .ppn = (pa & ~0xFFFULL) >> 12,
        .npages = 2,
        .access = rw_acc
    };
    KURD_t mk;
    vaddr_t mapped = Kspace_pinterval_alloc_and_map(iv, &mk);
    if (mapped == 0 || error_kurd(mk)) {
        bsp_kout << "[ERROR] Failed to map physical address" << kendl;
        return mk;
    }

    uint64_t offset = pa & 0xFFF;
    uint8_t* dst = (uint8_t*)(mapped + offset);
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = (uint8_t)((value >> (i * 8)) & 0xFF);
    }

    vm_interval um = {
        .vpn = mapped >> 12,
        .ppn = iv.ppn,
        .npages = 2,
        .access = rw_acc
    };
    KURD_t umk = Kspace_phyaddr_direct_unmap(um);

    bsp_kout << "[pwrite] Wrote 0x" << HEX << value << DEC
             << " to phyaddr=0x" << HEX << pa << DEC
             << " (size=" << size << ")" << kendl;

    if (error_kurd(umk)) return umk;
    return mk;
}

// ── pmap ──────────────────────────────────────────────────────

KURD_t cmd_pmap(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: pmap <phyaddr> <vaddr|0> <size_bytes> [access]" << kendl;
        return KURD_t{};
    }

    uint64_t pa, va, size;
    if (!token_to_uint64(line->tokens[1], &pa) || !token_to_uint64(line->tokens[2], &va) ||
        !token_to_uint64(line->tokens[3], &size)) {
        bsp_kout << "[ERROR] Invalid parameters" << kendl;
        return KURD_t{};
    }

    pgaccess access = KspacePageTable::PG_RW;
    if (line->token_count >= 5) {
        const token_t& a = line->tokens[4];
        if (tok_eq(a, "R"))    access = KspacePageTable::PG_R;
        else if (tok_eq(a, "RX")) {
            access = KspacePageTable::PG_RW;
            access.is_executable = 1;
            access.is_writeable = 0;
            access.is_readable = 1;
        }
        else if (tok_eq(a, "RW"))  access = KspacePageTable::PG_RW;
        else if (tok_eq(a, "RWX")) access = KspacePageTable::PG_RWX;
    }

    vm_interval iv = {
        .vpn = va >> 12,
        .ppn = pa >> 12,
        .npages = size >> 12,
        .access = access
    };
    KURD_t mk = Kspace_phyaddr_direct_map(iv);
    if (error_kurd(mk)) {
        bsp_kout << "[ERROR] pmap failed" << kendl;
        return mk;
    }

    bsp_kout << "[pmap] phyaddr=0x" << HEX << pa << DEC
             << " → vaddr=0x" << HEX << va << DEC
             << " size=" << size << kendl;
    return mk;
}

// ── punmap ────────────────────────────────────────────────────

KURD_t cmd_punmap(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: punmap <vaddr> <size_bytes>" << kendl;
        return KURD_t{};
    }

    uint64_t va, size;
    if (!token_to_uint64(line->tokens[1], &va) || !token_to_uint64(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid parameters" << kendl;
        return KURD_t{};
    }

    pgaccess rd_acc = {1,0,1,0,1,WB};
    vm_interval iv = {
        .vpn = va >> 12,
        .ppn = 0,
        .npages = size >> 12,
        .access = rd_acc
    };
    KURD_t kr = Kspace_phyaddr_direct_unmap(iv);
    if (error_kurd(kr)) {
        bsp_kout << "[ERROR] punmap failed (reason=" << kr.reason << ")" << kendl;
        return kr;
    }

    bsp_kout << "[punmap] Unmapped vaddr=0x" << HEX << va << DEC
             << " size=" << size
             << " [WARNING] Accessing this address will page fault" << kendl;
    return kr;
}

// ── dmap ──────────────────────────────────────────────────────

KURD_t cmd_dmap(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: dmap <phyaddr> <size_bytes>" << kendl;
        return KURD_t{};
    }

    uint64_t pa, size;
    if (!token_to_uint64(line->tokens[1], &pa) || !token_to_uint64(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid parameters" << kendl;
        return KURD_t{};
    }

    pgaccess rw_acc = {1,1,1,0,1,WB};
    vm_interval iv = {
        .vpn = 0,
        .ppn = pa >> 12,
        .npages = size >> 12,
        .access = rw_acc
    };
    KURD_t mk;
    vaddr_t mapped = Kspace_pinterval_alloc_and_map(iv, &mk);
    if (mapped == 0 || error_kurd(mk)) {
        bsp_kout << "[ERROR] dmap failed" << kendl;
        return mk;
    }

    bsp_kout << "[dmap] phyaddr=0x" << HEX << pa << DEC
             << " → vaddr=0x" << HEX << mapped << DEC
             << " size=" << size << kendl;
    return mk;
}

// ── stackalloc ────────────────────────────────────────────────

KURD_t cmd_stackalloc(const line_t* line) {
    if (line->token_count < 2) {
        bsp_kout << "[ERROR] Usage: stackalloc <pages_count>" << kendl;
        return KURD_t{};
    }

    uint64_t pages;
    if (!token_to_uint64(line->tokens[1], &pages) || pages == 0 || pages > 256) {
        bsp_kout << "[ERROR] pages must be 1-256" << kendl;
        return KURD_t{};
    }

    KURD_t kurd;
    vaddr_t stack_bottom = stack_alloc(&kurd, pages);
    if (error_kurd(kurd) || stack_bottom == 0) {
        bsp_kout << "[ERROR] stack_alloc failed (result=" << kurd.result
                 << " reason=" << kurd.reason << ")" << kendl;
        return kurd;
    }

    // stack_alloc 返回栈底（高地址），之下有 pages 页 + 1 页缓冲
    vaddr_t stack_top = stack_bottom - (pages * 4096);
    vaddr_t guard_end = stack_top - 4096;

    bsp_kout << "[stackalloc] top=0x" << HEX << stack_top << DEC
             << "  bottom=0x" << HEX << stack_bottom << DEC
             << "  guard_page=0x" << HEX << guard_end << DEC << kendl;
    return kurd;
}

// ── kvread ─────────────────────────────────────────────────────

KURD_t cmd_kvread(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: kvread <vaddr> <size> [hex/dec/ascii]" << kendl;
        return KURD_t{};
    }

    uint64_t va, size;
    if (!token_to_uint64(line->tokens[1], &va) || !token_to_uint64(line->tokens[2], &size)) {
        bsp_kout << "[ERROR] Invalid address or size" << kendl;
        return KURD_t{};
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        bsp_kout << "[ERROR] size must be 1/2/4/8 bytes" << kendl;
        return KURD_t{};
    }

    enum { HEX, DEC, ASCII } fmt = HEX;
    if (line->token_count >= 4) {
        if (tok_eq(line->tokens[3], "dec"))   fmt = DEC;
        else if (tok_eq(line->tokens[3], "ascii")) fmt = ASCII;
    }

    uint8_t* src = (uint8_t*)(uint64_t)va;
    uint64_t value = 0;
    for (uint64_t i = 0; i < size; i++) value |= ((uint64_t)src[i]) << (i * 8);

    bsp_kout << "[kvread] vaddr=0x" << HEX << va << DEC << " (size=" << size << "): ";
    if (fmt == HEX)      bsp_kout << "0x" << HEX << value << DEC;
    else if (fmt == DEC) bsp_kout << value;
    else { for (uint64_t i = 0; i < size; i++) {
        char c = (char)((value >> (i*8)) & 0xFF);
        bsp_kout << (c >= 0x20 && c < 0x7F ? c : '.');
    }}
    bsp_kout << kendl;
    return KURD_t{};
}

// ── kvwrite ────────────────────────────────────────────────────

KURD_t cmd_kvwrite(const line_t* line) {
    if (line->token_count < 3) {
        bsp_kout << "[ERROR] Usage: kvwrite <vaddr> <value> [size]" << kendl;
        return KURD_t{};
    }

    uint64_t va, value;
    if (!token_to_uint64(line->tokens[1], &va) || !token_to_uint64(line->tokens[2], &value)) {
        bsp_kout << "[ERROR] Invalid address or value" << kendl;
        return KURD_t{};
    }

    uint64_t size = 4;
    if (line->token_count >= 4) {
        uint64_t sz;
        if (token_to_uint64(line->tokens[3], &sz)) {
            if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
                bsp_kout << "[ERROR] size must be 1/2/4/8" << kendl;
                return KURD_t{};
            }
            size = sz;
        }
    }

    uint8_t* dst = (uint8_t*)(uint64_t)va;
    for (uint64_t i = 0; i < size; i++) dst[i] = (uint8_t)((value >> (i*8)) & 0xFF);

    bsp_kout << "[kvwrite] Wrote 0x" << HEX << value << DEC
             << " to vaddr=0x" << HEX << va << DEC
             << " (size=" << size << ")" << kendl;
    return KURD_t{};
}

// ── phymem ─────────────────────────────────────────────────────

KURD_t cmd_phymem(const line_t* line) {
    (void)line;

    bsp_kout << "=== Physical Memory Segments (" << phymem_segments_count << ") ===" << kendl;
    for (uint64_t i = 0; i < phymem_segments_count; i++) {
        const auto& s = phymem_segments[i];
        const char* type_str = "?";
        switch (s.type) {
            case freeSystemRam: type_str = "Free RAM"; break;
            case OS_ALLOCATABLE_MEMORY: type_str = "Allocatable"; break;
            case OS_KERNEL_DATA: type_str = "Kernel Data"; break;
            case OS_KERNEL_CODE: type_str = "Kernel Code"; break;
            case OS_KERNEL_STACK: type_str = "Kernel Stack"; break;
            case OS_HARDWARE_GRAPHIC_BUFFER: type_str = "Graphic Buf"; break;
            case OS_PGTB_SEGS: type_str = "Page Tables"; break;
            case OS_RESERVED_MEMORY: type_str = "Reserved"; break;
            case OS_MEMSEG_HOLE: type_str = "Hole"; break;
            case EFI_BOOT_SERVICES_CODE: type_str = "EFI BSC"; break;
            case EFI_BOOT_SERVICES_DATA: type_str = "EFI BSD"; break;
            case EFI_RUNTIME_SERVICES_CODE: type_str = "EFI RTC"; break;
            case EFI_RUNTIME_SERVICES_DATA: type_str = "EFI RTD"; break;
            case EFI_ACPI_RECLAIM_MEMORY: type_str = "ACPI Recl"; break;
            case EFI_ACPI_MEMORY_NVS: type_str = "ACPI NVS"; break;
            case EFI_MEMORY_MAPPED_IO: type_str = "MMIO"; break;
            case EFI_LOADER_CODE: type_str = "Loader Code"; break;
            case EFI_LOADER_DATA: type_str = "Loader Data"; break;
            default: type_str = "Other"; break;
        }
        bsp_kout << "  [" << i << "]  base=0x" << HEX << s.start << DEC
                 << "  size=" << s.size << "  " << type_str << kendl;
    }
    return KURD_t{};
}

// ── vminfo ─────────────────────────────────────────────────────

KURD_t cmd_vminfo(const line_t* line) {
    (void)line;

    bsp_kout << "=== VM Intervals (" << VM_intervals_count << ") ===" << kendl;
    for (uint64_t i = 0; i < VM_intervals_count; i++) {
        const auto& iv = VM_intervals[i];
        bsp_kout << "  [" << i << "]  vbase=0x" << HEX << iv.vbase << DEC
                 << "  pbase=0x" << HEX << iv.pbase << DEC
                 << "  size=" << iv.size
                 << "  id=" << iv.VM_interval_specifyid;
        bsp_kout << "  acc=";
        bsp_kout << (iv.access.is_kernel?"K":"U");
        bsp_kout << (iv.access.is_readable?"R":"-");
        bsp_kout << (iv.access.is_writeable?"W":"-");
        bsp_kout << (iv.access.is_executable?"X":"-");
        bsp_kout << kendl;
    }
    return KURD_t{};
}
