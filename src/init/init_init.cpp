#include "abi/boot.h"
#include "init/load_kernel.h"
#include "init/page_allocator.h"
#include "init/pages_alloc.h"
#include "init/util/textConsole.h"
#include "init/util/kout.h"
#include "init/core_hardwares/PortDriver.h"
#include "init/panic.h"
#include "init/init_linker_symbols.h"
#include "16x32AsciiCharacterBitmapSet.h"
#include "arch/x86_64/core_hardwares/primitive_gop.h"
#include "arch/x86_64/boot.h"          // x86_specify_init_to_kernel_info
#include "firmware/gSTResloveAPIs.h"
#include "init/initramfs_lookup.h"      // initramfs_lookup, RSDP_struct, ACPI_20_TABLE_GUID
#include "initramfs/fs_format.h"        // initramfs_header
#include "arch/x86_64/init/page_table.h"// modify_access
#include "arch/x86_64/core_hardwares/HPET.h"
#include "memory/memory_base.h"
#include <elf.h>

// ============================================================================
// 前向声明 & 常量
// ============================================================================
#define OS_INVALID_PARAMETER (-5)
#define OS_ACPI_NOT_FOUNED   (-6)
#define OS_OUT_OF_MEMORY     (-7)
#define OS_SUCCESS           0

extern "C" void shift_kernel(phyaddr_t info_pbase, vaddr_t stack_bottom, vaddr_t entry_vaddr);

// 全局 magic
static constexpr uint64_t INIT_TO_KERNEL_MAGIC = 0x494E494B524E4C48ULL; // "INIKRNLH"

// ============================================================================
// 跨阶段全局暂存
// ============================================================================
static phyaddr_t g_xsdt_base      = 0;
static phyaddr_t g_ramfs_base     = 0;
static uint64_t  g_ramfs_size     = 0;

// Phase 3a 产物
static phyaddr_t  g_kimg_pbase;
static uint64_t   g_kimg_file_size;
vm_interval g_kIMG_self_window;
vm_interval g_kBSS_interval;
static vaddr_t    g_entry_vaddr;
static uint64_t   g_kernel_vaddr_top;

// Phase 3b 产物
vm_interval g_FPA_bitmaps;
vm_interval g_log_buffer;
vm_interval g_kernel_entry_stack;
vm_interval g_symtable_file;
vm_interval g_initramfs_file;
vm_interval g_identity_map_window;  // [0x4000, dram_top) 恒等
static vaddr_t     g_pages_arr_vbase;

// 无一等 header 字段的额外 VM (FIRST_HEAP / FIRST_HEAP_BITMAP / UP_KSPACE_PDPT)
static loaded_VM_interval* g_extra_vm_arr;
static uint64_t            g_extra_vm_count;

// x86 架构特定信息
static x86_specify_init_to_kernel_info g_arch_info;

// ============================================================================
// Phase 1: 输出器 + 堆 (保持不动)
// ============================================================================
static int init_io_and_heap(BootInfoHeader* header) {
    bsp_kout.Init();
    heap.first_linekd_heap_Init();
    bsp_kout.shift_hex();
    // pass-through: 初始化 GOP
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].device_info == PASS_THROUGH_DEVICE_GRAPHICS_INFO) {
            auto* gfx = (GlobalBasicGraphicInfoType*)header->pass_through_devices[i].specify_data;
            InitGop::Init(gfx);
            break;
        }
    }
    init_textconsole::Init(
        (const unsigned char*)ter16x32_data, {16, 32}, 0xFFFFFFFF, 0xFF000000);
    serial_init_stage1();
    bsp_kout << "[INIT] Phase 1: I/O + heap ready" << kendl;
    return 0;
}

// ============================================================================
// Phase 2: 内存准备
// ============================================================================
static int init_memory_early(BootInfoHeader* header) {
    // 2a. 解析 XSDT (UEFI memory 尚可访问)
    g_xsdt_base = 0;
    if (header->gST_ptr) {
        auto* st = (EFI_SYSTEM_TABLE*)header->gST_ptr;
        auto* ct  = st->ConfigurationTable;
        UINTN n   = st->NumberOfTableEntries;
        const EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
        const uint8_t* g1 = (const uint8_t*)&acpi20;
        for (uint64_t i = 0; i < n; i++) {
            const uint8_t* g2 = (const uint8_t*)&ct[i].VendorGuid;
            bool match = true;
            for (size_t b = 0; b < sizeof(EFI_GUID); b++)
                if (g1[b] != g2[b]) { match = false; break; }
            if (match) {
                auto* rsdp = (RSDP_struct*)ct[i].VendorTable;
                if (rsdp && rsdp->Revision >= 2) {
                    g_xsdt_base = rsdp->XsdtAddress;
                    break;
                }
            }
        }
    }
    if (g_xsdt_base)
        bsp_kout << "[INIT] XSDT at phys 0x" << g_xsdt_base << kendl;
    else
        bsp_kout << "[WARN] ACPI 2.0 XSDT not found" << kendl;

    // 2b. basic_allocator 自举
    int r = basic_allocator::Init(header->memory_map_ptr, header->memory_map_entry_count);
    if (r != 0) { bsp_kout << "[INIT] basic_allocator::Init failed: " << r << kendl; return r; }

    // 2c. 标记 init 自身映像
    uint64_t init_img_sz = (uint64_t)&__init_heap_end - (uint64_t)&__init_text_start;
    basic_allocator::pages_set({(uint64_t)&__init_text_start, align_up(init_img_sz, 4096)},
                                PHY_MEM_TYPE::OS_KERNEL_DATA);
    basic_allocator::pages_set({(uint64_t)header, (uint64_t)header->total_pages_count * 4096},
                                PHY_MEM_TYPE::OS_KERNEL_DATA);
    for (uint64_t i = 0; i < header->loaded_file_count; i++) {
        if (header->loaded_files[i].file_type == LOADED_FILE_ENTRY_TYPE_ELF_REAL_LOAD) continue;
        basic_allocator::pages_set(
            {(uint64_t)header->loaded_files[i].raw_data,
             align_up(header->loaded_files[i].file_size, 4096)},
            PHY_MEM_TYPE::OS_KERNEL_DATA);
    }

    // 2d. page_allocator
    r = page_allocator::init();
    if (r != 0) { bsp_kout << "[INIT] page_allocator::init failed: " << r << kendl; return r; }
    bsp_kout << "[INIT] Phase 2: memory ready, free pages="
             << page_allocator::free_page_count() << kendl;
    return 0;
}

// ============================================================================
// Phase 2.5: initramfs 高位搬迁
// ============================================================================
static void relocate_initramfs(BootInfoHeader* header) {
    loaded_file_entry* ramfs = nullptr;
    for (uint64_t i = 0; i < header->loaded_file_count; i++) {
        if (strcmp_in_kernel(header->loaded_files[i].file_name, "\\initramfs.img") == 0) {
            ramfs = &header->loaded_files[i];
            break;
        }
    }
    if (!ramfs || ramfs->raw_data == 0) {
        bsp_kout << "[INIT] initramfs not loaded, skip relocation" << kendl;
        return;
    }

    uint64_t sz   = ramfs->file_size;
    uint64_t asz  = align_up(sz, 4096);
    uint64_t pcnt = asz >> 12;
    uint64_t newb = page_allocator::available_meminterval_probe(pcnt, 12);
    if (newb == 0) {
        bsp_kout << "[WARN] initramfs relocation: no space, keeping at 0x"
                 << (uint64_t)ramfs->raw_data << kendl;
        g_ramfs_base = (uint64_t)ramfs->raw_data;
        g_ramfs_size = asz;
        return;
    }
    newb -= (pcnt << 12);  // top → base
    ksystemramcpy(ramfs->raw_data, (void*)newb, sz);
    bsp_kout << "[INIT] initramfs: 0x" << (uint64_t)ramfs->raw_data
             << " -> 0x" << newb << " (" << sz << " bytes)" << kendl;

    page_allocator::pages_set({(uint64_t)ramfs->raw_data, asz}, page_state_t::free);
    page_allocator::pages_set({newb, asz}, page_state_t::kernel_persisit);

    ramfs->raw_data = (void*)newb;
    g_ramfs_base = newb;
    g_ramfs_size = asz;
}
// ============================================================================
// va_alloc v2 — 从内核虚地址顶开始向上分配
// ============================================================================
// Phase 3a 完成后解禁：va_alloc_base = kernel_vaddr_top 对齐到 2MB
// 后续 Phase 3b 用此分配器获取虚拟地址。
extern uint64_t g_va_alloc_base;
// ============================================================================
// Phase 3a (串行) — 全局填充分支
// ============================================================================
// ============================================================================
// Phase 3a (串行): kernel.elf 基础加载
// ============================================================================
// Phase 3a (串行): kernel.elf 基础加载
// ============================================================================
//
// 时序:
//   1. 从重定位后的 initramfs 通过 initramfs_lookup 提取 kernel.elf
//   2. probe_keep 分配专用物理区间, 拷贝整个 kernel.elf
//   3. ELF 程序头校验:
//      a. 至少一个 PT_LOAD
//      b. 所有 PT_LOAD: filesz/memsz/offset/vaddr 全部 4KB 对齐
//         (非 BSS 物理地址 = kimg_pbase + p_offset)
//      c. 最多一个 fsize==0 && msize>0 的段 (BSS)
//   4. BSS: probe_keep 另分配物理页, 清零; 其它: 在 kimg_pbase 内原地
//   5. KMMU 高半映射所有段
//   6. 解禁 va_alloc: base = kernel_vaddr_top, 向上分配
//   7. 分配 kIMG_self_window 的 vaddr, KMMU 映射, 记录
//      (整个 kernel.elf 文件的虚拟地址窗口)
//
// 输出: 填充全局 g_kimg_pbase / g_kIMG_self_window / g_kBSS_interval / g_entry_vaddr
//
static void phase_3a_load_kernel(kernel_mmu* kmmu, BootInfoHeader* header) {
    // ---- 1. 从 initramfs 定位 kernel.elf ----
    if (g_ramfs_base == 0) {
        bsp_kout << "[Phase3a] initramfs not relocated" << kendl; asm volatile("hlt");
    }
    const initramfs_header* rh = (const initramfs_header*)(uint64_t)g_ramfs_base;
    uint64_t kelf_sz = 0;
    phyaddr_t kelf_in_ramfs = initramfs_lookup(rh, "/kernel.elf", &kelf_sz);
    if (kelf_in_ramfs == 0 || kelf_sz == 0) {
        bsp_kout << "[Phase3a] initramfs_lookup failed" << kendl; asm volatile("hlt");
    }
    uint64_t kelf_pages = align_up(kelf_sz, 4096) >> 12;
    g_kimg_pbase = page_allocator::available_meminterval_probe_keep(kelf_pages, 12);
    if (g_kimg_pbase == 0) {
        bsp_kout << "[Phase3a] probe_keep OOM: " << kelf_pages << " pages" << kendl; asm volatile("hlt");
    }
    page_allocator::pages_set({g_kimg_pbase, kelf_pages << 12}, page_state_t::kernel_persisit);
    ksystemramcpy((void*)(uint64_t)kelf_in_ramfs, (void*)(uint64_t)g_kimg_pbase, kelf_sz);
    ksetmem_8((void*)(uint64_t)(g_kimg_pbase + kelf_sz), 0, (kelf_pages << 12) - kelf_sz);
    g_kimg_file_size = kelf_sz;
    bsp_kout << "[Phase3a] kernel.elf at 0x" << g_kimg_pbase << " size=" << kelf_sz << kendl;

    // ---- 2. ELF header + PT_LOAD 校验 ----
    uint8_t* elf_base = (uint8_t*)(uint64_t)g_kimg_pbase;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[EI_MAG0]!=ELFMAG0||ehdr->e_ident[EI_MAG1]!=ELFMAG1||
        ehdr->e_ident[EI_MAG2]!=ELFMAG2||ehdr->e_ident[EI_MAG3]!=ELFMAG3) {
        bsp_kout << "[Phase3a] bad magic" << kendl; asm volatile("hlt");
    }
    g_entry_vaddr = ehdr->e_entry;
    bsp_kout << "[Phase3a] phnum=" << ehdr->e_phnum << " entry=0x" << g_entry_vaddr << kendl;

    uint8_t* ptbl = elf_base + ehdr->e_phoff;
    uint64_t ptcnt = 0; int bss_idx = -1;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        ptcnt++;
        if ((ph->p_offset & 0xFFF) || (ph->p_filesz & 0xFFF) ||
            (ph->p_memsz & 0xFFF) || (ph->p_vaddr & 0xFFF)) {
            bsp_kout << "[Phase3a] align fail seg " << i << kendl; asm volatile("hlt"); }
        phyaddr_t seg_p = g_kimg_pbase + ph->p_offset;
        if ((seg_p & 0xFFF) || (seg_p+ph->p_filesz > g_kimg_pbase+kelf_pages*4096ULL)) {
            bsp_kout << "[Phase3a] range fail seg " << i << kendl; asm volatile("hlt"); }
        if (ph->p_filesz == 0 && ph->p_memsz > 0) {
            if (bss_idx >= 0) { bsp_kout << "[Phase3a] multi BSS" << kendl; asm volatile("hlt"); }
            bss_idx = i;
        }
    }
    if (ptcnt == 0) { bsp_kout << "[Phase3a] no PT_LOAD" << kendl; asm volatile("hlt"); }

    // ---- 3. 加载 + KMMU 高半映射 ----
    g_kernel_vaddr_top = 0;
    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint64_t msz = ph->p_memsz; vaddr_t va = ph->p_vaddr; phyaddr_t pa;
        if ((int)i == bss_idx) {
            uint64_t pg = msz >> 12;
            pa = page_allocator::available_meminterval_probe_keep(pg, 12);
            if (!pa) { bsp_kout << "[Phase3a] BSS OOM" << kendl; asm volatile("hlt"); }
            page_allocator::pages_set({pa, msz}, page_state_t::kernel_persisit);
            ksetmem_8((void*)(uint64_t)pa, 0, msz);
            g_kBSS_interval = {va, pa, msz, {1,1,1,0,1,WB}};
        } else { pa = g_kimg_pbase + ph->p_offset; }
        uint64_t vend = va + msz;
        if (vend > g_kernel_vaddr_top) g_kernel_vaddr_top = vend;
        pgaccess acc = {1, (uint8_t)((ph->p_flags&PF_W)?1:0), 1,
                        (uint8_t)((ph->p_flags&PF_X)?1:0), 1, WB};
        if (kmmu->map({pa, va, align_up(msz,4096)}, acc)) {
            bsp_kout << "[Phase3a] map fail seg " << i << kendl; asm volatile("hlt"); }
    }

    // ---- 4. 解禁 va_alloc ----
    g_va_alloc_base = align_up(g_kernel_vaddr_top, 0x200000);

    // ---- 5. kIMG_self_window: 整个 kernel.elf 文件映射窗口 ----
    uint64_t ws = align_up(kelf_sz, 4096);
    vaddr_t wv = va_alloc(ws, 21);
    pgaccess wa = {1,1,1,0,1,WB};
    if (kmmu->map({g_kimg_pbase, wv, ws}, wa)) {
        bsp_kout << "[Phase3a] kIMG_window map fail" << kendl; asm volatile("hlt"); }
    g_kIMG_self_window = {wv, g_kimg_pbase, kelf_sz, wa};

    bsp_kout << "[Phase3a] done: kIMG 0x" << g_kimg_pbase << "->0x" << wv
             << " kBSS p=0x" << g_kBSS_interval.pbase
             << " vaddr_top=0x" << g_kernel_vaddr_top << kendl;
}





static uint64_t va_alloc_up(uint64_t size, uint8_t align_log2) {
    uint64_t align = 1ULL << align_log2;
    if (align < 4096) align = 4096;
    size = align_up(size, align);
    uint64_t ret = align_up(g_va_alloc_base, align);
    g_va_alloc_base = ret + size;
    return ret;
}


static void phase_3b_identity_map(kernel_mmu* kmmu, BootInfoHeader* header) {
    // 覆盖 [16KB, dram_top) 做 WB+RWX 恒等映射
    phyaddr_t top = page_allocator::dram_top();
    uint64_t sz   = top - 0x4000;
    pgaccess id_a = {1,1,1,1,0,WB};
    kmmu->map({0x4000, 0x4000, sz}, id_a);
    bsp_kout << "[Phase3b] identity: [0x4000, 0x" << top << ") WB+RWX" << kendl;
}

static void phase_3b_load_intervals(kernel_mmu* kmmu, BootInfoHeader* header) {
    // 清空 extra VM 数组，待会填充
    g_extra_vm_arr   = new loaded_VM_interval[8];
    g_extra_vm_count = 0;

    auto add_extra = [&](phyaddr_t p, vaddr_t v, uint64_t sz, uint32_t id, pgaccess a) {
        g_extra_vm_arr[g_extra_vm_count++] = {p, v, sz, id, a};
    };

    bsp_kout << "[Phase3b] start..." << kendl;

    // ---- FPA_bitmaps (probe_keep) ----
    {
        uint64_t tp = page_allocator::total_page_count();
        uint64_t sz = align_up(tp / 4, 4096);
        phyaddr_t p = page_allocator::available_meminterval_probe_keep(sz >> 12, 12);
        if (!p) { bsp_kout << "FPA OOM" << kendl; asm volatile("hlt"); }
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, sz);
        vaddr_t v = va_alloc_up(sz, 12);
        kmmu->map({p, v, sz}, {1,1,1,0,1,WB});
        g_FPA_bitmaps = {v, p, sz, {1,1,1,0,1,WB}};
        bsp_kout << "[Phase3b] FPA_bitmaps: p=0x" << p << " v=0x" << v << " sz=0x" << sz << kendl;
    }

    // ---- log_buffer (probe) ----
    {
        uint64_t sz = LOGBUFFER_SIZE;
        phyaddr_t p = page_allocator::available_meminterval_probe(sz >> 12, 21);
        if (!p) { bsp_kout << "log OOM" << kendl; asm volatile("hlt"); }
        p -= sz;  // top → base
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, sz);
        vaddr_t v = va_alloc_up(sz, 21);
        kmmu->map({p, v, sz}, {1,1,1,0,1,WB});
        g_log_buffer = {v, p, sz, {1,1,1,0,1,WB}};
        bsp_kout << "[Phase3b] log_buffer: p=0x" << p << " v=0x" << v << kendl;
    }

    // ---- kernel_entry_stack (probe) ----
    {
        uint64_t sz = BSP_INIT_STACK_SIZE;
        phyaddr_t p = page_allocator::available_meminterval_probe(sz >> 12, 12);
        if (!p) { bsp_kout << "stack OOM" << kendl; asm volatile("hlt"); }
        p -= sz;  // top → base
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        vaddr_t v = va_alloc_up(sz, 12);
        kmmu->map({p, v, sz}, {1,1,1,0,1,WB});
        g_kernel_entry_stack = {v, p, sz, {1,1,1,0,1,WB}};
        bsp_kout << "[Phase3b] stack: p=0x" << p << " v=0x" << v << kendl;
    }

    // ---- symtable_file (probe + initramfs_lookup) ----
    {
        phyaddr_t sym_in_ramfs = 0; uint64_t sym_sz = 0;
        if (g_ramfs_base) {
            auto* rh = (const initramfs_header*)(uint64_t)g_ramfs_base;
            sym_in_ramfs = initramfs_lookup(rh, "/ksymbols.bin", &sym_sz);
        }
        if (sym_in_ramfs == 0 || sym_sz == 0) {
            bsp_kout << "[Phase3b] ksymbols.bin not found" << kendl;
        } else {
            uint64_t sz = align_up(sym_sz, 4096);
            phyaddr_t p = page_allocator::available_meminterval_probe(sz >> 12, 21);
            if (!p) { bsp_kout << "sym OOM" << kendl; asm volatile("hlt"); }
            p -= sz;  // top → base
            page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
            ksystemramcpy((void*)(uint64_t)sym_in_ramfs, (void*)(uint64_t)p, sym_sz);
            vaddr_t v = va_alloc_up(sz, 21);
            kmmu->map({p, v, sz}, {1,1,0,0,1,WB});  // RW, no X
            g_symtable_file = {v, p, sym_sz, {1,1,0,0,1,WB}};
            bsp_kout << "[Phase3b] symtable: p=0x" << p << " v=0x" << v << kendl;
        }
    }

    // ---- initramfs_file (va_alloc + KMMU) ----
    {
        if (g_ramfs_base && g_ramfs_size) {
            uint64_t sz = align_up(g_ramfs_size, 4096);
            vaddr_t v = va_alloc_up(sz, 21);
            kmmu->map({g_ramfs_base, v, sz}, {1,1,0,0,1,WB});
            g_initramfs_file = {v, g_ramfs_base, sz, {1,1,0,0,1,WB}};
            bsp_kout << "[Phase3b] initramfs: p=0x" << g_ramfs_base << " v=0x" << v << kendl;
        }
    }

    // ---- pages_arr 预分配 vaddr ----
    {
        uint64_t tp = page_allocator::total_page_count();
        uint64_t sz = align_up(tp, 4096);
        g_pages_arr_vbase = va_alloc_up(sz, 21);
        bsp_kout << "[Phase3b] pages_arr vaddr: 0x" << g_pages_arr_vbase << kendl;
    }

    // ---- [0, dram_top) va_alloc 窗口 ----
    {
        phyaddr_t top = page_allocator::dram_top();
        uint64_t sz  = align_up(top, 0x40000000ULL);
        vaddr_t  v   = va_alloc_up(sz, 30);
        kmmu->map({0, v, sz}, {1,1,1,0,1,WB});  // flat RW window, no X
        bsp_kout << "[Phase3b] identity_va_window: [0, 0x" << top << ") at 0x" << v << kendl;
        (void)v;
    }

    // ---- identity_map_window 记录 ----
    {
        phyaddr_t top = page_allocator::dram_top();
        g_identity_map_window = {0x4000, 0x4000, align_up(top - 0x4000, 0x40000000ULL),
                                 {1,1,1,1,0,WB}};
        bsp_kout << "[Phase3b] identity_window: [0x4000, 0x" << top << ")" << kendl;
    }
    // ---- x86 arch_specify ----
    // GOP
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].device_info != PASS_THROUGH_DEVICE_GRAPHICS_INFO) continue;
        auto* gfx = (GlobalBasicGraphicInfoType*)header->pass_through_devices[i].specify_data;
        if (!gfx) break;
        ksystemramcpy(gfx, &g_arch_info.gop_info, sizeof(*gfx));
        uint64_t fb_sz = align_up(gfx->FrameBufferSize, 4096);
        phyaddr_t fb_p = (phyaddr_t)gfx->FrameBufferBase;
        vaddr_t fb_v = va_alloc_up(fb_sz, 21);
        kmmu->map({fb_p, fb_v, fb_sz}, {1,1,1,0,1,WC});
        g_arch_info.Gop_vbase = fb_v;
        bsp_kout <<HEX<< "[Phase3b] GOP fb: p=0x" << fb_p << " v=0x" << fb_v << kendl;
        break;
    }
    // HPET
    if (g_xsdt_base) {
        auto* xsdt = (const XSDT_Table*)(uint64_t)g_xsdt_base;
        uint64_t ec = (xsdt->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
        for (uint64_t i = 0; i < ec; i++) {
            auto* hdr = (const ACPI_Table_Header*)(uint64_t)xsdt->Entry[i];
            if (!hdr) continue;
            if (*(const uint32_t*)hdr->Signature == HPET_SIGNATURE_UINT32) {
                auto* ht = (const HPET_Table*)(uint64_t)xsdt->Entry[i];
                phyaddr_t hp = ht->Base_Address;
                if (hp) {
                    vaddr_t hv = va_alloc_up(0x1000, 12);
                    kmmu->map({hp, hv, 0x1000}, {1,1,0,0,1,UC});
                    g_arch_info.hpet_mmio = {hp, hv, 0x1000, {1,1,0,0,1,UC}};
                    bsp_kout <<HEX<< "[Phase3b] HPET MMIO: p=0x" << hp << " v=0x" << hv << kendl;
                }
                break;
            }
        }
    }
    g_arch_info.XSDT_base = g_xsdt_base;

    bsp_kout << "[Phase3b] done: " << g_extra_vm_count << " extra VM entries" << kendl;
}

// ============================================================================
// Phase 4 (串行): 构建 init_to_kernel_header
// ============================================================================
extern phyaddr_t build_init_to_kernel_header(
    phyaddr_t                pkt_pbase,
    uint64_t                 pkt_pages,
    kernel_mmu*              kmmu,
    BootInfoHeader*          header,
    loaded_VM_interval*      extra_vm_arr,
    uint64_t                 extra_vm_count,
    phymem_segment*          seg_view,
    uint64_t                 seg_count,
    x86_specify_init_to_kernel_info* arch);

// ============================================================================
// Phase 4.5 (自裁 → CR3 切换 → shift_kernel)
// ============================================================================
static void phase_45_finalize(kernel_mmu* kmmu, phyaddr_t info_pbase, vaddr_t entry_vaddr) {
    // 4.5-1: relinquish
    phyaddr_t mm_pb; uint64_t mm_pc;
    page_allocator::relinquish_mem_map(&mm_pb, &mm_pc);

    // 4.5-2: pages_arr
    uint64_t mm_sz = mm_pc * sizeof(page);
    init_to_kernel_header* h = (init_to_kernel_header*)(uint64_t)info_pbase;
    h->pages_arr = {mm_pb, g_pages_arr_vbase, mm_sz, {1,1,1,0,1,WB}};
    kmmu->map({mm_pb, g_pages_arr_vbase, align_up(mm_sz, 4096)}, {1,1,1,0,1,WB});
    bsp_kout << "[Phase4.5] pages_arr: p=0x" << mm_pb << " v=0x" << g_pages_arr_vbase << kendl;

    // 4.5-3: CR3
    phyaddr_t root = kmmu->get_root_table_base();
    bsp_kout << "[Phase4.5] CR3 <- 0x" << root << kendl;
    asm volatile("sfence");
    asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");

    // 4.5-4: stack_bottom from g_kernel_entry_stack
    vaddr_t stack_bottom = g_kernel_entry_stack.vbase + g_kernel_entry_stack.size-0x1000;
    bsp_kout << "[Phase4.5] shift_kernel: info=0x" << info_pbase
             << " stack=0x" << stack_bottom << " entry=0x" << entry_vaddr << kendl;
    shift_kernel(info_pbase, stack_bottom, entry_vaddr);
}

// ============================================================================
// init — 主入口
// ============================================================================
extern "C" void init(BootInfoHeader* header) {
    if (init_io_and_heap(header) != 0) asm volatile("hlt");
    if (init_memory_early(header) != 0) asm volatile("hlt");
    relocate_initramfs(header);

    // Phase 3a
    kernel_mmu* kmmu = new kernel_mmu(arch_enums::x86_64_PGLV4);
    phase_3a_load_kernel(kmmu, header);

    // Phase 3b
    phase_3b_identity_map(kmmu, header);
    phase_3b_load_intervals(kmmu, header);

    // Phase 4
    uint64_t segcnt = 0;
    phymem_segment* pure_view = basic_allocator::get_pure_memory_view(&segcnt);
    constexpr uint64_t PKT_PAGES = 4;
    phyaddr_t pkt = page_allocator::available_meminterval_probe(PKT_PAGES, 12);
    if (!pkt) { bsp_kout << "pkt OOM" << kendl; asm volatile("hlt"); }
    pkt -= (PKT_PAGES << 12);
    ksetmem_8((void*)(uint64_t)pkt, 0, PKT_PAGES * 4096);
    page_allocator::pages_set({pkt, PKT_PAGES * 4096}, page_state_t::kernel_persisit);

    if (!build_init_to_kernel_header(pkt, PKT_PAGES, kmmu, header,
                                     g_extra_vm_arr, g_extra_vm_count,
                                     pure_view, segcnt, &g_arch_info)) {
        bsp_kout << "build_init_to_kernel_header failed" << kendl; asm volatile("hlt");
    }

    // Phase 4.5
    phase_45_finalize(kmmu, pkt, g_entry_vaddr);
    asm volatile("hlt");
}
