#include "KImage_Introspection.h"
#include "arch/x86_64/mem_init.h"
#include "util/kout.h"

// ============================================================================
// 内部状态：仅缓存 BSS 程序头指针
// ============================================================================
// BSS 段识别条件：p_filesz == 0 && p_memsz != 0 && vaddr 在核内地址空间
static const Elf64_Phdr* sg_bss_phdr = nullptr;

// ============================================================================
// init — 解析 ELF 程序头表，缓存 BSS 段
// ============================================================================
void self_introspection_init() {
    sg_bss_phdr = nullptr;

    uint64_t file_base = kIMG_self_window.vbase();
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file_base);

    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        bsp_kout << "[KImage] ERROR: bad ELF magic" << kendl;
        return;
    }

    const auto* ptbl = reinterpret_cast<const Elf64_Phdr*>(file_base + ehdr->e_phoff);
    bsp_kout << "[KImage] phnum=" << ehdr->e_phnum << kendl;

    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        const auto& ph = ptbl[i];

        // 识别 BSS：filesz==0, memsz!=0, 内核地址
        if (ph.p_type == PT_LOAD &&
            ph.p_filesz == 0 && ph.p_memsz > 0) {
            vm_interval iv = {.vpn = ph.p_vaddr >> 12};
            if (is_addr_kernel_address((void*)(iv.vpn<<12))) {
                sg_bss_phdr = &ptbl[i];
                bsp_kout << "[KImage] BSS phdr[" << i << "] v=0x" << ph.p_vaddr
                         << " p=0x" << ph.p_paddr
                         << " sz=0x" << ph.p_memsz << kendl;
            }
        }
    }

    if (!sg_bss_phdr)
        bsp_kout << "[KImage] WARNING: no BSS segment found" << kendl;
}

// ============================================================================
// BSS 虚拟地址 → 物理地址转换
// ============================================================================
// 使用缓存的 BSS 程序头读 p_paddr（init.elf 已填入真实物理地址），
// 替代旧的 kBSS_interval 依赖。
extern "C" phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr) {
    if (!sg_bss_phdr) return 0;
    vaddr_t bss_va = sg_bss_phdr->p_vaddr;
    if (vaddr < bss_va || vaddr >= bss_va + sg_bss_phdr->p_memsz) return 0;
    return sg_bss_phdr->p_paddr + (vaddr - bss_va);
}
