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
#include "arch/x86_64/abi/GS_Slots_index_definitions.h"
#include "arch/x86_64/abi/GS_complex.h"
#include "arch/x86_64/abi/msr_offsets_definitions.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "arch/x86_64/abi/pt_regs.h"
#include "arch/x86_64/boot.h"          // x86_specify_init_to_kernel_info
#include "firmware/gSTResloveAPIs.h"
#include "init/initramfs_lookup.h"      // initramfs_lookup, RSDP_struct, ACPI_20_TABLE_GUID
#include "initramfs/fs_format.h"        // initramfs_header
#include "arch/x86_64/init/page_table.h"// modify_access
#include "arch/x86_64/core_hardwares/HPET.h"
#include "memory/memory_base.h"
#include "init/init_phase_ctx.h"
#include "init/init_heap_v3.h"

#include <elf.h>
extern "C" void init_jump_to_kernel(x64_standard_context* ctx);
// ============================================================================
// 前向声明 & 常量
// ============================================================================
#define OS_INVALID_PARAMETER (-5)
#define OS_ACPI_NOT_FOUNED   (-6)
#define OS_OUT_OF_MEMORY     (-7)
#define OS_SUCCESS           0

// 入口在 gs_complex_load_gdt_tss + iretq 中实现（替代 shift_kernel）
// 初始 RFLAGS（与 create_kthread 一致）：IF=1, IOPL=0, 保留位 1
static constexpr uint64_t KERNEL_INIT_RFLAGS = 0x202;

// 全局 magic
static constexpr uint64_t INIT_TO_KERNEL_MAGIC = 0x494E494B524E4C48ULL; // "INIKRNLH"

// ============================================================================
// VA 分配器状态（唯一保留的文件级全局）
// ============================================================================
// 定义在 kernel_load.cpp，这里使用相同的全局变量
extern uint64_t g_va_alloc_base;

// ── BSS: BCB 位图 ──
static constexpr uint64_t HEAP_BITMAP_BITS  = 3ull << 16;
static constexpr uint64_t HEAP_BITMAP_BYTES = ((HEAP_BITMAP_BITS + 63) >> 6) * 8;
alignas(64) static uint8_t s_heap_bitmap[HEAP_BITMAP_BYTES];
uint64_t va_alloc(uint64_t size,uint8_t align_log2){
    if (align_log2 < 12) {
        align_log2 = 12;
    }
    if(g_va_alloc_base==0)return 0;
    uint64_t alignment = 1ULL << align_log2;
    uint64_t res=align_down(g_va_alloc_base, alignment);
    g_va_alloc_base += size;
    return res;
}
// ============================================================================
// Phase 1: 输出器 + 堆
// ============================================================================
static int init_io_and_heap(BootInfoHeader* header) {
    bsp_kout.Init();
    // 初始化 V3 伴侣堆 (BCB-based, 单线程, 无锁)
    uint64_t heap_sz = (uint64_t)&__init_heap_end - (uint64_t)&__init_heap_start;
    g_init_heap.linktime_init((vaddr_t)&__init_heap_start,
                              (uint32_t)heap_sz,
                              (vaddr_t)s_heap_bitmap);
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
// Phase 2: 内存准备 → 返回 ctx_early_mem
// ============================================================================
static ctx_early_mem init_memory_early(BootInfoHeader* header) {
    ctx_early_mem em = {};

    // 2a. 解析 XSDT (UEFI memory 尚可访问)
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
                    em.xsdt_base = rsdp->XsdtAddress;
                    break;
                }
            }
        }
    }
    if (em.xsdt_base)
        bsp_kout << "[INIT] XSDT at phys 0x" << em.xsdt_base << kendl;
    else
        bsp_kout << "[WARN] ACPI 2.0 XSDT not found" << kendl;

    // 2b. basic_allocator 自举
    int r = basic_allocator::Init(header->memory_map_ptr, header->memory_map_entry_count);
    if (r != 0) { bsp_kout << "[INIT] basic_allocator::Init failed: " << r << kendl; return em; }

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
    if (r != 0) { bsp_kout << "[INIT] page_allocator::init failed: " << r << kendl; return em; }
    bsp_kout << "[INIT] Phase 2: memory ready, free pages="
             << page_allocator::free_page_count() << kendl;
    return em;
}

// ============================================================================
// Phase 2.5: initramfs 高位搬迁（原地更新 ctx_early_mem）
// ============================================================================
static void relocate_initramfs(BootInfoHeader* header, ctx_early_mem* em) {
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
        em->ramfs_base = (uint64_t)ramfs->raw_data;
        em->ramfs_size = asz;
        return;
    }
    newb -= (pcnt << 12);  // top → base
    ksystemramcpy(ramfs->raw_data, (void*)newb, sz);
    bsp_kout << "[INIT] initramfs: 0x" << (uint64_t)ramfs->raw_data
             << " -> 0x" << newb << " (" << sz << " bytes)" << kendl;

    page_allocator::pages_set({(uint64_t)ramfs->raw_data, asz}, page_state_t::free);
    page_allocator::pages_set({newb, asz}, page_state_t::kernel_persisit);

    ramfs->raw_data = (void*)newb;
    em->ramfs_base  = newb;
    em->ramfs_size  = asz;
}
uint64_t g_va_alloc_base=0;
// ============================================================================
// va_alloc_up — 从 g_va_alloc_base 向上分配 VA（Phase 3b 专用）
// ============================================================================
static uint64_t va_alloc_up(uint64_t size, uint8_t align_log2) {
    uint64_t align = 1ULL << align_log2;
    if (align < 4096) align = 4096;
    size = align_up(size, align);
    uint64_t ret = align_up(g_va_alloc_base, align);
    g_va_alloc_base = ret + size;
    return ret;
}

// ============================================================================
// Phase 3a (串行): kernel.elf 基础加载 → 返回 ctx_kernel_loaded
// ============================================================================
//
// 设计：
//   1. kernel.elf 完整文件 → 瞬态端分配（仅供自省，不用于执行）
//   2. 每个 PT_LOAD 段独立处理：
//      a. p_flags & 0x100 → probe_keep 独立分配物理页，拷贝文件内容，
//         清零 BSS 余部，KMMU 映射，并将实际 PA 写回 ELF 的 p_paddr
//      b. 否则 → 直接按 p_paddr 映射（PA 由链接脚本指定）
//   3. kIMG_self_window 映射瞬态端文件映像（程序头表 p_paddr 已改写）
//   4. kernel.elf 自省时通过 kIMG_self_window 读程序头表，p_paddr 即真实 PA
//
// 变更（相对于旧版非破坏性加载）：
//   - 文件映像从 keep-end → transient
//   - BSS 无特殊路径，统一纳入 PT_LOAD 循环
//   - 0x100 段独立 PA 使大页映射成为可能
//
static ctx_kernel_loaded phase_3a_load_kernel(kernel_mmu* kmmu, const ctx_early_mem* em,
                                               BootInfoHeader* /*header*/) {
    ctx_kernel_loaded kl = {};
    kl.kmmu = kmmu;

    // ---- 1. 从 initramfs 定位 kernel.elf，分配瞬态端 ----
    if (em->ramfs_base == 0) {
        bsp_kout << "[Phase3a] initramfs not relocated" << kendl; asm volatile("hlt");
    }
    const initramfs_header* rh = (const initramfs_header*)(uint64_t)em->ramfs_base;
    uint64_t kelf_sz = 0;
    phyaddr_t kelf_in_ramfs = initramfs_lookup(rh, "/kernel.elf", &kelf_sz);
    if (kelf_in_ramfs == 0 || kelf_sz == 0) {
        bsp_kout << "[Phase3a] initramfs_lookup failed" << kendl; asm volatile("hlt");
    }
    uint64_t kelf_pages = align_up(kelf_sz, 4096) >> 12;
    phyaddr_t kelf_top   = page_allocator::available_meminterval_probe(kelf_pages, 12);
    if (kelf_top == 0) {
        bsp_kout << "[Phase3a] transient OOM: " << kelf_pages << " pages" << kendl; asm volatile("hlt");
    }
    kl.kimg_pbase = kelf_top - (kelf_pages << 12);  // top → base (瞬态端高→低)
    page_allocator::pages_set({kl.kimg_pbase, kelf_pages << 12}, page_state_t::kernel_persisit);
    ksystemramcpy((void*)(uint64_t)kelf_in_ramfs, (void*)(uint64_t)kl.kimg_pbase, kelf_sz);
    ksetmem_8((void*)(uint64_t)(kl.kimg_pbase + kelf_sz), 0, (kelf_pages << 12) - kelf_sz);
    kl.kimg_file_size = kelf_sz;
    bsp_kout << "[Phase3a] kernel.elf (transient) at 0x" << kl.kimg_pbase
             << " size=" << kelf_sz << kendl;

    // ---- 2. ELF header 校验 ----
    uint8_t* elf_base = (uint8_t*)(uint64_t)kl.kimg_pbase;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_base;
    if (ehdr->e_ident[EI_MAG0]!=ELFMAG0||ehdr->e_ident[EI_MAG1]!=ELFMAG1||
        ehdr->e_ident[EI_MAG2]!=ELFMAG2||ehdr->e_ident[EI_MAG3]!=ELFMAG3) {
        bsp_kout << "[Phase3a] bad magic" << kendl; asm volatile("hlt");
    }
    kl.entry_vaddr = ehdr->e_entry;
    bsp_kout << "[Phase3a] phnum=" << ehdr->e_phnum << " entry=0x" << kl.entry_vaddr << kendl;

    // ---- 3. 校验 + 加载所有 PT_LOAD 段 ----
    //     遍历两轮：首轮校验，次轮加载
    uint8_t* ptbl = elf_base + ehdr->e_phoff;
    uint64_t     phent = ehdr->e_phentsize;
    uint64_t     ptcnt = 0;
    Elf64_Half   phnum = ehdr->e_phnum;

    // 3a. 校验：至少一个 PT_LOAD，所有文件偏移/大小/地址页对齐
    for (Elf64_Half i = 0; i < phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * phent);
        if (ph->p_type != PT_LOAD) continue;
        ptcnt++;
        // p_paddr 也需要页对齐（0x100 段由 init.elf 保障，非 0x100 段由链接脚本保障）
        if ((ph->p_offset & 0xFFF) || (ph->p_filesz & 0xFFF) ||
            (ph->p_memsz  & 0xFFF) || (ph->p_vaddr & 0xFFF) ||
            (ph->p_paddr  & 0xFFF)) {
            bsp_kout << "[Phase3a] align fail seg " << i << kendl; asm volatile("hlt");
        }
        // 校验文件数据不超出文件映像
        if (ph->p_filesz) {
            phyaddr_t seg_p = kl.kimg_pbase + ph->p_offset;
            if (seg_p + ph->p_filesz > kl.kimg_pbase + kelf_pages * 4096ULL) {
                bsp_kout << "[Phase3a] range fail seg " << i << kendl; asm volatile("hlt");
            }
        }
    }
    if (ptcnt == 0) { bsp_kout << "[Phase3a] no PT_LOAD" << kendl; asm volatile("hlt"); }

    // 3b. 加载：遍历 PT_LOAD，按 0x100 标志决定分配策略
    uint64_t kernel_vaddr_top = 0;
    for (Elf64_Half i = 0; i < phnum; i++) {
        Elf64_Phdr* ph = (Elf64_Phdr*)(ptbl + i * phent);
        if (ph->p_type != PT_LOAD) continue;

        uint64_t  msz   = ph->p_memsz;
        uint64_t  fsz   = ph->p_filesz;
        vaddr_t   va    = ph->p_vaddr;
        uint64_t  flags = ph->p_flags;
        uint64_t  npg   = align_up(msz, 4096) >> 12;
        phyaddr_t pa;

        if (flags & 0x100) {
            // ── 0x100 段：probe_keep 独立分配 ──
            pa = page_allocator::available_meminterval_probe_keep(npg, 12);
            if (!pa) {
                bsp_kout << "[Phase3a] keep OOM seg " << i << " (0x" << HEX << va << ")" << DEC << kendl;
                asm volatile("hlt");
            }
            page_allocator::pages_set({pa, npg << 12}, page_state_t::kernel_persisit);

            // 拷贝文件内容（p_filesz 字节）
            if (fsz) {
                ksystemramcpy((void*)(uint64_t)(kl.kimg_pbase + ph->p_offset),
                              (void*)(uint64_t)pa, fsz);
            }
            // 清零 BSS 余部（p_memsz - p_filesz 字节）
            if (msz > fsz) {
                ksetmem_8((void*)(uint64_t)(pa + fsz), 0, msz - fsz);
            }

            // 将实际 PA 写回 ELF 程序头表的 p_paddr
            ph->p_paddr = pa;

            bsp_kout << "[Phase3a]  seg[" << i << "] keep: v=0x" << HEX << va
                     << " p=0x" << pa << " sz=0x" << msz << DEC << kendl;
        } else {
            // ── 非 0x100 段：按 p_paddr 指定的物理地址加载 ──
            pa = ph->p_paddr;
            // 拷贝文件内容到目标 PA
            if (fsz) {
                ksystemramcpy((void*)(uint64_t)(kl.kimg_pbase + ph->p_offset),
                              (void*)(uint64_t)pa, fsz);
            }
            // 清零 BSS 余部
            if (msz > fsz) {
                ksetmem_8((void*)(uint64_t)(pa + fsz), 0, msz - fsz);
            }
            bsp_kout << "[Phase3a]  seg[" << i << "] paddr: v=0x" << HEX << va
                     << " p=0x" << pa << " sz=0x" << msz << DEC << kendl;
        }

        // 跟踪最高 vaddr
        uint64_t vend = va + msz;
        if (vend > kernel_vaddr_top) kernel_vaddr_top = vend;

        // KMMU 映射
        pgaccess acc = {1, (uint8_t)((flags & PF_W) ? 1 : 0), 1,
                        (uint8_t)((flags & PF_X) ? 1 : 0), 1, WB};
        uint64_t seg_sz = align_up(msz, 4096);
        if (kl.kmmu->map({pa, va, seg_sz}, acc)) {
            bsp_kout << "[Phase3a] map fail seg " << i << kendl; asm volatile("hlt");
        }
    }

    // ---- 4. 解禁 va_alloc ----
    g_va_alloc_base = align_up(kernel_vaddr_top, 0x200000);

    // ---- 5. kIMG_self_window: 瞬态端文件映像窗口（已含改写后的 p_paddr） ----
    uint64_t ws   = align_up(kelf_sz, 4096);
    uint64_t wpgs = ws >> 12;
    vaddr_t  wv   = va_alloc(ws, 21);
    pgaccess wa   = KSPACE_RW_ACCESS;
    if (kl.kmmu->map({kl.kimg_pbase, wv, ws}, wa)) {
        bsp_kout << "[Phase3a] kIMG_window map fail" << kendl; asm volatile("hlt");
    }
    kl.kIMG_self_window = {.vpn = wv >> 12, .ppn = kl.kimg_pbase >> 12,
                           .npages = wpgs, .access = wa};

    bsp_kout << "[Phase3a] done: kIMG(transient) " << (void*)kl.kimg_pbase
             << "->" << (void*)wv << " vaddr_top=" << (void*)kernel_vaddr_top << kendl;

    bsp_kout << "[Phase3a] kIMG_self_window: vaddr=" << (void*)(uint64_t)kl.kIMG_self_window.vbase()
             << " paddr=" << (void*)(uint64_t)kl.kIMG_self_window.pbase()
             << " size=0x" << (uint64_t)(kl.kIMG_self_window.npages << 12)
             << " entry=" << (void*)(uint64_t)kl.entry_vaddr << kendl;
    return kl;
}

// ============================================================================
// Phase 3b (串行): 恒等映射 + 区间分配 + 架构信息收集 → 返回 ctx_intervals
// ============================================================================
static ctx_intervals phase_3b(kernel_mmu* kmmu, BootInfoHeader* header, const ctx_early_mem* em) {
    ctx_intervals iv = {};

    // --- 清空 extra VM 数组 ---
    iv.extra_vm_arr   = new loaded_VM_interval[8];
    iv.extra_vm_count = 0;

    auto add_extra = [&](phyaddr_t p, vaddr_t v, uint64_t sz, uint32_t id, pgaccess a) {
        iv.extra_vm_arr[iv.extra_vm_count++] = {p, v, sz, id, a};
    };

    bsp_kout << "[Phase3b] start..." << kendl;

    // ---- 恒等映射: [4KB, dram_top) WB+RWX（短暂存在，不进 info header） ----
    //     仅用于 init.elf 自身 CR3 切换的极小窗口 + 跳转 kernel.elf 后访问信息包。
    //     kernel.elf 接手后通过 Kspace_phyaddr_access_window 访问物理地址。
    {
        phyaddr_t top = page_allocator::dram_top();
        uint64_t  sz  = top - 0x1000;
        pgaccess id_a = KSPACE_RWX_NG_ACCESS;
        kmmu->map({0x1000, 0x1000, sz}, id_a);
        bsp_kout << "[Phase3b] identity: [0x1000, 0x" << top << ") WB+RWX (transient)" << kendl;
    }

    // ---- FPA_bitmaps (probe_keep) ----
    {
        uint64_t tp  = page_allocator::total_page_count();
        uint64_t sz  = align_up(tp / 4, 4096);
        uint64_t npg = sz >> 12;
        phyaddr_t p  = page_allocator::available_meminterval_probe_keep(npg, 12);
        if (!p) { bsp_kout << "FPA OOM" << kendl; asm volatile("hlt"); }
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, sz);
        vaddr_t v = va_alloc_up(sz, 12);
        kmmu->map({p, v, sz}, KSPACE_RW_ACCESS);
        iv.FPA_bitmaps = {.vpn = v >> 12, .ppn = p >> 12,
                          .npages = npg, .access = KSPACE_RW_ACCESS};
        bsp_kout << "[Phase3b] FPA_bitmaps: p=0x" << p << " v=" << (void*)v << " sz=" << (void*)sz << kendl;
    }

    // ---- log_buffer (probe) ----
    {
        uint64_t sz  = LOGBUFFER_SIZE;
        uint64_t npg = sz >> 12;
        phyaddr_t p  = page_allocator::available_meminterval_probe(npg, 21);
        if (!p) { bsp_kout << "log OOM" << kendl; asm volatile("hlt"); }
        p -= sz;  // top → base
        page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
        ksetmem_8((void*)(uint64_t)p, 0, sz);
        vaddr_t v = va_alloc_up(sz, 21);
        kmmu->map({p, v, sz}, KSPACE_RW_ACCESS);
        iv.log_buffer = {.vpn = v >> 12, .ppn = p >> 12,
                         .npages = npg, .access = KSPACE_RW_ACCESS};
        bsp_kout << "[Phase3b] log_buffer: p=0x" << p << " v=" << (void*)v << kendl;
    }
    // （kernel_entry_stack 已废弃——Phase 4.5 跳转时直接用 BSP 的 rsp0 栈）

    // ---- symtable_file (probe + initramfs_lookup) → movable_file_entry_t ----
    {
        phyaddr_t sym_in_ramfs = 0; uint64_t sym_sz = 0;
        if (em->ramfs_base) {
            auto* rh = (const initramfs_header*)(uint64_t)em->ramfs_base;
            sym_in_ramfs = initramfs_lookup(rh, "/ksymbols.bin", &sym_sz);
        }
        if (sym_in_ramfs == 0 || sym_sz == 0) {
            bsp_kout << "[Phase3b] ksymbols.bin not found" << kendl;
        } else {
            uint64_t sz  = align_up(sym_sz, 4096);
            uint64_t npg = sz >> 12;
            phyaddr_t p  = page_allocator::available_meminterval_probe(npg, 21);
            if (!p) { bsp_kout << "sym OOM" << kendl; asm volatile("hlt"); }
            p -= sz;
            page_allocator::pages_set({p, sz}, page_state_t::kernel_persisit);
            ksystemramcpy((void*)(uint64_t)sym_in_ramfs, (void*)(uint64_t)p, sym_sz);
            vaddr_t v = va_alloc_up(sz, 21);
            kmmu->map({p, v, sz}, KSPACE_RW_ACCESS);  // RW, no X
            iv.symtable_file = {
                .interval = {.vpn = v >> 12, .ppn = p >> 12,
                             .npages = npg, .access = KSPACE_RW_ACCESS},
                .offset = 0,
                .size   = sym_sz
            };
            bsp_kout << "[Phase3b] symtable: p=0x" << p << " v=" << (void*)v << kendl;
        }
    }

    // ---- initramfs_file (va_alloc + KMMU) → movable_file_entry_t ----
    {
        if (em->ramfs_base && em->ramfs_size) {
            uint64_t sz  = align_up(em->ramfs_size, 4096);
            uint64_t npg = sz >> 12;
            vaddr_t  v   = va_alloc_up(sz, 21);
            kmmu->map({em->ramfs_base, v, sz}, KSPACE_RW_ACCESS);
            iv.initramfs_file = {
                .interval = {.vpn = v >> 12, .ppn = em->ramfs_base >> 12,
                             .npages = npg, .access = KSPACE_RW_ACCESS},
                .offset = 0,
                .size   = em->ramfs_size
            };
            bsp_kout << "[Phase3b] initramfs: p=" << (void*)em->ramfs_base << " v=" << (void*)v << kendl;
        }
    }

    // ---- pages_arr 预分配 vaddr ----
    {
        uint64_t tp  = page_allocator::total_page_count();
        uint64_t sz  = align_up(tp, 4096);
        iv.pages_arr_vbase = va_alloc_up(sz, 21);
        bsp_kout << "[Phase3b] pages_arr vaddr=" << (void*)(uint64_t)iv.pages_arr_vbase << kendl;
    }

    // ---- [0, dram_top) va_alloc 窗口 ----
    {
        phyaddr_t top = page_allocator::dram_top();
        uint64_t  sz  = align_up(top, 0x40000000ULL);
        vaddr_t   v   = va_alloc_up(sz, 30);
        kmmu->map({0, v, sz}, KSPACE_RW_ACCESS);  // flat RW window, no X
        bsp_kout << "[Phase3b] identity_va_window: [0," << (void*)top << ") at" << (void*)v << kendl;
        (void)v;
    }

    // ---- x86 arch_specify ----
    // GOP
    for (uint64_t i = 0; i < header->pass_through_device_info_count; i++) {
        if (header->pass_through_devices[i].device_info != PASS_THROUGH_DEVICE_GRAPHICS_INFO) continue;
        auto* gfx = (GlobalBasicGraphicInfoType*)header->pass_through_devices[i].specify_data;
        if (!gfx) break;
        ksystemramcpy(gfx, &iv.arch_info.gop_info, sizeof(*gfx));
        uint64_t fb_sz  = align_up(gfx->FrameBufferSize, 4096);
        uint64_t fb_npg = fb_sz >> 12;
        phyaddr_t fb_p  = (phyaddr_t)gfx->FrameBufferBase;
        vaddr_t fb_v    = va_alloc_up(fb_sz, 21);
        kmmu->map({fb_p, fb_v, fb_sz}, KSPACE_RW_WC_ACCESS);
        iv.arch_info.Gop_vbase = fb_v;
        bsp_kout << HEX << "[Phase3b] GOP fb: p=0x" << fb_p << " v=0x" << fb_v << kendl;
        break;
    }

    // HPET
    if (em->xsdt_base) {
        auto* xsdt = (const XSDT_Table*)(uint64_t)em->xsdt_base;
        uint64_t ec = (xsdt->Header.Length - sizeof(ACPI_Table_Header)) / sizeof(uint64_t);
        for (uint64_t i = 0; i < ec; i++) {
            auto* hdr = (const ACPI_Table_Header*)(uint64_t)xsdt->Entry[i];
            if (!hdr) continue;
            if (*(const uint32_t*)hdr->Signature == HPET_SIGNATURE_UINT32) {
                auto* ht = (const HPET_Table*)(uint64_t)xsdt->Entry[i];
                phyaddr_t hp = ht->Base_Address;
                if (hp) {
                    vaddr_t hv = va_alloc_up(0x1000, 12);
                    kmmu->map({hp, hv, 0x1000}, KSPACE_RW_UC_ACCESS);
                    iv.arch_info.hpet_mmio = {.vpn = hv >> 12, .ppn = hp >> 12,
                                              .npages = 1, .access = KSPACE_RW_UC_ACCESS};
                    bsp_kout  << "[Phase3b] HPET MMIO: p=" << (void*)hp << " v=" << (void*)hv << kendl;
                }
                break;
            }
        }
    }
    iv.arch_info.XSDT_base = em->xsdt_base;

    // conjunc_GSs: 每个处理器分配一个 gs_complex_t
    {
        uint64_t total_bytes    = header->logical_processor_count * GS_COMPLEX_STRIDE;
        uint64_t npg            = total_bytes >> 12;
        phyaddr_t pbase         = page_allocator::available_meminterval_probe_keep(npg, 12);
        vaddr_t  vbase          = va_alloc_up(total_bytes, 12);
        ksetmem_8((void*)(uint64_t)pbase, 0, total_bytes);
        iv.arch_info.conjunc_GSs = {
            .vpn    = vbase >> 12,
            .ppn    = pbase >> 12,
            .npages = npg,
            .access = KSPACE_RW_ACCESS
        };
        vinterval v;
        v.vbase    = vbase;
        v.size     = total_bytes;
        v.phybase  = pbase;
        kmmu->map(v, KSPACE_RW_ACCESS);
        bsp_kout << "[Phase3b] conjunc_GSs: vaddr=" << (void*)(uint64_t)vbase
                 << " paddr=" << (void*)(uint64_t)pbase
                 << " size=0x" << (uint64_t)(npg << 12) << kendl;
    }

    // hardware stacks: 每处理器栈区含 guard 页，各处理器固定硬件栈
    {
        uint64_t stack_stride   = sizeof(per_processor_hardware_stack_t);  // 含 5 guard 页
        uint64_t total_phys     = header->logical_processor_count * stack_stride + 4096;  // + 尾 guard
        uint64_t total_virt     = total_phys;
        iv.arch_info.hdstacks_interval_pbase  = page_allocator::available_meminterval_probe_keep(total_phys >> 12, 12);
        iv.arch_info.hdstacks_interval_vbase  = va_alloc_up(total_virt, 12);
        iv.arch_info.hdstacks_4kbpgs_count    = total_phys >> 12;
        bsp_kout << "[Phase3b] hdstacks:   vaddr=" << (void*)(uint64_t)iv.arch_info.hdstacks_interval_vbase
                 << " paddr=" << (void*)(uint64_t)iv.arch_info.hdstacks_interval_pbase
                 << " pages=" << (uint64_t)iv.arch_info.hdstacks_4kbpgs_count << kendl;

        // 逐处理器映射栈页（跳过 guard 页），并设置 GS slot 0 和 stacks_ptr
        phyaddr_t gs_pbase = iv.arch_info.conjunc_GSs.pbase();
        for (uint32_t p = 0; p < header->logical_processor_count; p++) {
            // 本处理器的栈区在物理/虚拟区间的偏移
            uint64_t proc_off   = p * stack_stride;
            phyaddr_t proc_pphy = iv.arch_info.hdstacks_interval_pbase + proc_off;
            vaddr_t   proc_pvir = iv.arch_info.hdstacks_interval_vbase + proc_off;

            // 映射 stack_rsp0 (跳过 guard1)
            kmmu->map({proc_pphy + RSP0_BASE_OFF,
                       proc_pvir + RSP0_BASE_OFF,
                       RSP0_STACKSIZE},
                       KSPACE_RW_ACCESS);
            // 映射 stack_ist1 (跳过 guard2)
            kmmu->map({proc_pphy + IST1_BASE_OFF,
                       proc_pvir + IST1_BASE_OFF,
                       DF_STACKSIZE},
                       KSPACE_RW_ACCESS);
            // 映射 stack_ist2 (跳过 guard3)
            kmmu->map({proc_pphy + IST2_BASE_OFF,
                       proc_pvir + IST2_BASE_OFF,
                       MC_STACKSIZE},
                       KSPACE_RW_ACCESS);
            // 映射 stack_ist3 (跳过 guard4)
            kmmu->map({proc_pphy + IST3_BASE_OFF,
                       proc_pvir + IST3_BASE_OFF,
                       NMI_STACKSIZE},
                       KSPACE_RW_ACCESS);
            // 映射 stack_ist4 (跳过 guard5)
            kmmu->map({proc_pphy + IST4_BASE_OFF,
                       proc_pvir + IST4_BASE_OFF,
                       BP_DBG_STACKSIZE},
                       KSPACE_RW_ACCESS);

            // 在 GS 复合体中设置 slot 0 = rsp0 栈顶（syscall 快速入口）和 stacks_ptr
            gs_complex_t* complex = (gs_complex_t*)(uint64_t)(gs_pbase + p * GS_COMPLEX_STRIDE);
            complex->slots[PROCESSOR_RSP0_STACK_BTM_IDX] = proc_pvir+RSP0_BOTTOM_OFF;
            complex->stacks_ptr = (per_processor_hardware_stack_t*)(uint64_t)proc_pvir;
        }
    }
    // ---- Kspace_phyaddr_access_window: [0, dram_top) → Kspace VA ----
    //     不是恒等映射！将整个物理地址区间映射到内核高位空间（1GB 对齐），
    //     供 kernel.elf 的 PhyAddrAccessor / 页表重建时访问任意物理地址。
    {
        phyaddr_t top = page_allocator::dram_top();
        uint64_t  sz  = align_up(top, 0x40000000ULL);           // 1GB 对齐
        uint64_t  npg = sz >> 12;
        vaddr_t   vbase = va_alloc_up(sz, 30);                  // 1GB 对齐分配
        pgaccess wb_rw_ng = KSPACE_RW_ACCESS;
        kmmu->map({0, vbase, sz}, wb_rw_ng);
        iv.Kspace_phyaddr_access_window = {
            .vpn    = vbase >> 12,
            .ppn    = 0,
            .npages = npg,
            .access = wb_rw_ng
        };
        bsp_kout << "[Phase3b] Kspace_phyaddr_access_window: vaddr="
                 << (void*)(uint64_t)vbase << " size=0x" << sz << kendl;
    }
    bsp_kout << "[Phase3b] done: " << iv.extra_vm_count << " extra VM entries" << kendl;
    return iv;
}

// ============================================================================
// Phase 4 (串行): 构建 init_to_kernel_header（extern，定义在 info_fill.cpp）
// ============================================================================
// 签名变更：ctx 参数替代全局变量
extern phyaddr_t build_init_to_kernel_header(
    phyaddr_t                pkt_pbase,
    uint64_t                 pkt_pages,
    BootInfoHeader*          header,
    const ctx_kernel_loaded* kl,
    const ctx_intervals*     iv,
    phymem_segment*          seg_view,
    uint64_t                 seg_count);

// ============================================================================
// Phase 4.5 (自裁 → CR3 切换 → gs_complex_load_gdt_tss → iretq)
// ============================================================================
static void phase_45_finalize(kernel_mmu* kmmu, phyaddr_t info_pbase,
                              vaddr_t entry_vaddr, const ctx_intervals* iv) {
    // 4.5-1: relinquish
    phyaddr_t mm_pb; uint64_t mm_pc;
    page_allocator::relinquish_mem_map(&mm_pb, &mm_pc);

    // 4.5-2: pages_arr
    uint64_t mm_sz   = mm_pc * sizeof(page);
    uint64_t mm_npg  = align_up(mm_sz, 4096) >> 12;
    init_to_kernel_header* h = (init_to_kernel_header*)(uint64_t)info_pbase;
    h->pages_arr = {.vpn = iv->pages_arr_vbase >> 12, .ppn = mm_pb >> 12,
                    .npages = mm_npg, .access = KSPACE_RW_ACCESS};
    kmmu->map({mm_pb, iv->pages_arr_vbase, mm_sz}, KSPACE_RW_ACCESS);
    bsp_kout << "[Phase4.5] pages_arr: paddr=" << (void*)(uint64_t)mm_pb
             << " vaddr=" << (void*)(uint64_t)iv->pages_arr_vbase
             << " size=0x" << (uint64_t)mm_sz << kendl;
    
    // 4.5-3: CR3
    phyaddr_t root = kmmu->get_root_table_base();
    bsp_kout << "[Phase4.5] CR3 <- 0x" << root << kendl;
    asm volatile("sfence");
    asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");

    // 4.5-4: 构建所有处理器的 GDT/TSS 到 GS 复合体（恒等映射，pbase == vbase）
    {
        vaddr_t gs_base  = iv->arch_info.conjunc_GSs.vbase();
        wrmsr_func(msr::syscall::IA32_GS_BASE,gs_base);//提前给bsp加载好gs
        wrmsr_func(msr::syscall::IA32_KERNEL_GS_BASE,gs_base);//提前给bsp加载好gs
        vaddr_t hw_base  = iv->arch_info.hdstacks_interval_vbase;
        uint32_t  pcount   = h->logical_processor_count;
        constexpr TSSDescriptorEntry ktss = {
            .limit = sizeof(TSSentry), .base0 = 0, .base1 = 0,
            .type = 0x9, .zero = 0, .dpl = 0, .p = 1,
            .limit1 = (sizeof(TSSentry) - 1) >> 16,
            .avl = 0, .reserved = 0, .g = 0, .base2 = 0, .base3 = 0, .reserved2 = 0
        };
        for (uint32_t p = 0; p < pcount; p++) {
            gs_complex_t* cx = (gs_complex_t*)(uint64_t)(gs_base + p * GS_COMPLEX_STRIDE);
            per_processor_hardware_stack_t* st = cx->stacks_ptr;
            vaddr_t st_va=reinterpret_cast<vaddr_t>(st);
            cx->slots[PROCESSOR_ID_GS_INDEX]=p;
            // GDT 条目
            cx->gdt[K_cs_idx]    = kspace_CS_entry;
            cx->gdt[K_ds_ss_idx] = kspace_DS_SS_entry;
            cx->gdt[U_cs_idx]    = userspace_CS_entry;
            cx->gdt[U_ds_ss_idx] = userspace_DS_SS_entry;
            // TSS 描述符 + 基址 → 恒等映射下 &cx->tss
            vaddr_t tss_va = reinterpret_cast<vaddr_t>(&cx->tss);
            cx->tss_descriptor = ktss;
            cx->tss_descriptor.base0 = (uint32_t)tss_va & 0xFFFF;
            cx->tss_descriptor.base1 = (tss_va >> 16) & 0xFF;
            cx->tss_descriptor.base2 = (tss_va >> 24) & 0xFF;
            cx->tss_descriptor.base3 = tss_va >> 32;
            // TSS 栈指针 → hdstacks 内嵌栈顶部
            cx->tss.rsp0   = st_va+RSP0_BOTTOM_OFF;
            cx->tss.ist[0] = 0;
            cx->tss.ist[1] = st_va+IST1_BOTTOM_OFF;
            cx->tss.ist[2] = st_va+IST2_BOTTOM_OFF;
            cx->tss.ist[3] = st_va+IST3_BOTTOM_OFF;
            cx->tss.ist[4] = st_va+IST4_BOTTOM_OFF;
            // stacks_ptr + slot[0]
            cx->stacks_ptr = st;
            cx->slots[PROCESSOR_RSP0_STACK_BTM_IDX] = cx->tss.rsp0 ;
        }
        bsp_kout << "[Phase4.5] prepare " << pcount << " GS complexes" << kendl;
    }

    // 4.5-5: 加载 BSP 的 GDT + TSS（上一步已完全构建，此步仅 LGDT+LTR）
    {
        gs_complex_t* bsp_cx = (gs_complex_t*)(uint64_t)(iv->arch_info.conjunc_GSs.vbase());
        bsp_kout << "[Phase4.5] LGDT+LTR: complex @ 0x" << HEX << (uint64_t)bsp_cx << kendl;
        gs_complex_load_gdt_tss(bsp_cx);
    }

    // 4.5-6: init_jump_to_kernel — 用 BSP 的 rsp0 栈构建 x64_standard_context 后跳入 kernel.elf
    {
        // kernel_entry_stack 已废弃，改用 BSP GS 复合体内嵌的 rsp0 栈
        gs_complex_t* bsp = (gs_complex_t*)(uint64_t)iv->arch_info.conjunc_GSs.vbase();
        vaddr_t bsp_rsp0  = bsp->tss.rsp0;
        // 与 create_kthread 同款帧，区别：RDI=info_pbase, RFLAGS=KERNEL_INIT_RFLAGS
        x64_standard_context ctx = {};
        ctx.rdi                     = info_pbase;
        ctx.iret_complex.rip        = entry_vaddr;
        ctx.iret_complex.cs         = K_cs_idx << 3;
        ctx.iret_complex.rflags     = KERNEL_INIT_RFLAGS;
        ctx.iret_complex.rsp        = bsp_rsp0;
        ctx.iret_complex.ss         = K_ds_ss_idx << 3;
        bsp_kout << "[Phase4.5] init_jump_to_kernel: entry=" << (void*)(uint64_t)entry_vaddr
                 << " rsp=" << (void*)(uint64_t)bsp_rsp0
                 << " rdi=" << (void*)(uint64_t)info_pbase << kendl;
        init_jump_to_kernel(&ctx);
    }
}

// ============================================================================
// init — 主入口
// ============================================================================
extern "C" void init(BootInfoHeader* header) {
    if (init_io_and_heap(header) != 0) asm volatile("hlt");

    auto em = init_memory_early(header);
    if (!em.xsdt_base && /* memory early 出错检测 */ 0) asm volatile("hlt");
    // 注意: init_memory_early 返回空 struct 时 xsdt_base=0 属于正常（ACPI 找不到），
    // 不 halt。只有 basic_allocator/page_allocator 失败才会内部 halt。

    relocate_initramfs(header, &em);

    // Phase 3a + 3b 共享同一 KMMU，由 Phase 3a 初始化，Phase 3b 使用
    kernel_mmu* kmmu = new kernel_mmu(arch_enums::x86_64_PGLV4);
    auto kl = phase_3a_load_kernel(kmmu, &em, header);
    auto iv = phase_3b(kl.kmmu, header, &em);

    // Phase 4: 构造信息包
    uint64_t segcnt = 0;
    phymem_segment* pure_view = basic_allocator::get_pure_memory_view(&segcnt);
    constexpr uint64_t PKT_PAGES = 4;
    phyaddr_t pkt = page_allocator::available_meminterval_probe(PKT_PAGES, 12);
    if (!pkt) { bsp_kout << "pkt OOM" << kendl; asm volatile("hlt"); }
    pkt -= (PKT_PAGES << 12);
    ksetmem_8((void*)(uint64_t)pkt, 0, PKT_PAGES * 4096);
    page_allocator::pages_set({pkt, PKT_PAGES * 4096}, page_state_t::kernel_persisit);

    if (!build_init_to_kernel_header(pkt, PKT_PAGES, header,
                                     &kl, &iv, pure_view, segcnt)) {
        bsp_kout << "build_init_to_kernel_header failed" << kendl; asm volatile("hlt");
    }

    bsp_kout << "[Phase4] info_pkt: paddr=" << (void*)(uint64_t)pkt
             << " pages=" << (uint32_t)PKT_PAGES
             << " phymem_segments=" << (uint64_t)segcnt
             << " processors=" << (uint32_t)header->logical_processor_count << kendl;

    // Phase 4.5
    phase_45_finalize(kmmu, pkt, kl.entry_vaddr, &iv);
    asm volatile("hlt");
}
