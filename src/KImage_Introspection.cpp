#include "KImage_Introspection.h"
#include "util/kout.h"
#include <cstring>

// ============================================================================
// 内部状态
// ============================================================================
vm_interval KImage, Kbss;

// Shdr 表在文件镜像中的原始指针（init 时记录，无需深拷贝）
static const Elf64_Shdr* sg_shdr_table  = nullptr;
static uint64_t          sg_shdr_count  = 0;
static const char*       sg_shstrtab    = nullptr;  // .shstrtab 在 vaddr 空间的地址

// Phdr 表
static const Elf64_Phdr* sg_phdr_table  = nullptr;
static uint64_t          sg_phdr_count  = 0;

// ============================================================================
// phdr_type_str / phdr_type_from_name — 程序头类型名 ↔ 数值
// ============================================================================
static const char* phdr_type_str(uint32_t p_type) {
    switch (p_type) {
        case PT_NULL:    return "PT_NULL";
        case PT_LOAD:    return "PT_LOAD";
        case PT_DYNAMIC: return "PT_DYNAMIC";
        case PT_INTERP:  return "PT_INTERP";
        case PT_NOTE:    return "PT_NOTE";
        case PT_SHLIB:   return "PT_SHLIB";
        case PT_PHDR:    return "PT_PHDR";
        case PT_TLS:     return "PT_TLS";
        case PT_GNU_EH_FRAME: return "PT_GNU_EH_FRAME";
        case PT_GNU_STACK:    return "PT_GNU_STACK";
        case PT_GNU_RELRO: return "PT_GNU_RELRO";
        default:          return "PT_UNKNOWN";
    }
}

static uint32_t phdr_type_from_name(const char* name) {
    if (!name) return ~0U;
    if (strcmp(name, "PT_LOAD") == 0)    return PT_LOAD;
    if (strcmp(name, "PT_DYNAMIC") == 0) return PT_DYNAMIC;
    if (strcmp(name, "PT_INTERP") == 0)  return PT_INTERP;
    if (strcmp(name, "PT_NOTE") == 0)    return PT_NOTE;
    if (strcmp(name, "PT_TLS") == 0)     return PT_TLS;
    if (strcmp(name, "PT_NULL") == 0)    return PT_NULL;
    if (strcmp(name, "PT_GNU_EH_FRAME") == 0) return PT_GNU_EH_FRAME;
    if (strcmp(name, "PT_GNU_STACK") == 0)    return PT_GNU_STACK;
    return ~0U;
}

// ============================================================================
// init — 解析 ELF 节头表和程序头表
// ============================================================================
void self_introspection_init(vm_interval KImage_, vm_interval Kbss_) {
    KImage = KImage_;
    Kbss   = Kbss_;

    sg_shdr_table = nullptr;
    sg_shdr_count = 0;
    sg_shstrtab   = nullptr;
    sg_phdr_table = nullptr;
    sg_phdr_count = 0;

    uint64_t file_base = KImage.vbase;
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file_base);

    // 魔数校验
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        bsp_kout << "[KImage] ERROR: bad ELF magic" << kendl;
        return;
    }

    bsp_kout << "[KImage] ELF entry=0x" << ehdr->e_entry
             << " phoff=0x" << ehdr->e_phoff << " phnum=" << ehdr->e_phnum
             << " shoff=0x" << ehdr->e_shoff << " shnum=" << ehdr->e_shnum << kendl;

    // ---- 1. 程序头表 ----
    {
        sg_phdr_table = reinterpret_cast<const Elf64_Phdr*>(file_base + ehdr->e_phoff);
        sg_phdr_count = ehdr->e_phnum;

        for (uint64_t i = 0; i < sg_phdr_count; i++) {
            const auto& ph = sg_phdr_table[i];
            if (ph.p_type == PT_NULL && ph.p_memsz == 0) continue;

            bsp_kout << "[KImage] phdr " << i << ": "
                     << phdr_type_str(ph.p_type)
                     << " v=0x" << ph.p_vaddr
                     << " p=0x" << ph.p_paddr
                     << " filesz=0x" << ph.p_filesz
                     << " memsz=0x" << ph.p_memsz << kendl;
        }
    }

    // ---- 2. 节头表 ----
    {
        sg_shdr_table = reinterpret_cast<const Elf64_Shdr*>(file_base + ehdr->e_shoff);
        sg_shdr_count = ehdr->e_shnum;

        // 定位 .shstrtab
        if (ehdr->e_shstrndx < sg_shdr_count) {
            const auto& strtab_sh = sg_shdr_table[ehdr->e_shstrndx];
            sg_shstrtab = reinterpret_cast<const char*>(file_base + strtab_sh.sh_offset);
        }

        for (uint64_t i = 0; i < sg_shdr_count; i++) {
            const auto& sh = sg_shdr_table[i];
            if (sh.sh_type == SHT_NULL && sh.sh_size == 0) continue;

            const char* sh_name = (sg_shstrtab) ? (sg_shstrtab + sh.sh_name) : "";

            bsp_kout << "[KImage] section " << i << ": "
                     << sh_name << " vaddr=0x" << sh.sh_addr
                     << " off=0x" << sh.sh_offset
                     << " size=0x" << sh.sh_size << kendl;
        }
    }
}

// ============================================================================
// 节头表查询
// ============================================================================
const Elf64_Shdr* get_KImage_sections(uint64_t* out_count) {
    if (out_count) *out_count = sg_shdr_count;
    return sg_shdr_table;
}

const Elf64_Shdr* get_KImage_section(const char* name) {
    if (!name || !sg_shstrtab) return nullptr;
    for (uint64_t i = 0; i < sg_shdr_count; i++) {
        const auto& sh = sg_shdr_table[i];
        if (sh.sh_type == SHT_NULL) continue;
        if (strcmp(sg_shstrtab + sh.sh_name, name) == 0)
            return &sg_shdr_table[i];
    }
    return nullptr;
}

// ============================================================================
// 程序头表查询
// ============================================================================
const Elf64_Phdr* get_KImage_phdrs(uint64_t* out_count) {
    if (out_count) *out_count = sg_phdr_count;
    return sg_phdr_table;
}

const Elf64_Phdr* get_KImage_phdr_by_type(uint32_t p_type) {
    for (uint64_t i = 0; i < sg_phdr_count; i++) {
        if (sg_phdr_table[i].p_type == p_type)
            return &sg_phdr_table[i];
    }
    return nullptr;
}

const Elf64_Phdr* get_KImage_phdr_by_name(const char* type_name) {
    uint32_t ptype = phdr_type_from_name(type_name);
    if (ptype == ~0U) return nullptr;
    return get_KImage_phdr_by_type(ptype);
}

// ============================================================================
// .comment 节
// ============================================================================
const char* get_KImage_comment() {
    auto* sh = get_KImage_section(".comment");
    if (!sh || sh->sh_size == 0) return nullptr;
    return reinterpret_cast<const char*>(KImage.vbase + sh->sh_offset);
}

// ============================================================================
// NOTE 条目遍历
// ============================================================================
const void* get_KImage_note(const char* name, uint64_t* out_size) {
    auto* sh = get_KImage_section(".note");
    if (!sh || sh->sh_size == 0) return nullptr;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(KImage.vbase + sh->sh_offset);
    uint64_t pos = 0;

    while (pos + sizeof(Elf64_Nhdr) <= sh->sh_size) {
        const auto* nhdr = reinterpret_cast<const Elf64_Nhdr*>(base + pos);
        uint32_t namesz = (nhdr->n_namesz + 3) & ~3;
        uint32_t descsz = (nhdr->n_descsz + 3) & ~3;
        uint64_t next   = pos + sizeof(Elf64_Nhdr) + namesz + descsz;

        if (next > sh->sh_size) break;

        const char* entry_name = reinterpret_cast<const char*>(base + pos + sizeof(Elf64_Nhdr));
        if (nhdr->n_namesz > 0 && strcmp(entry_name, name) == 0) {
            if (out_size) *out_size = nhdr->n_descsz;
            return base + pos + sizeof(Elf64_Nhdr) + namesz;
        }
        pos = next;
    }
    return nullptr;
}

const void* get_KImage_build_id(uint64_t* out_size) {
    return get_KImage_note("GNU", out_size);
}

// ============================================================================
// 已有接口保留
// ============================================================================
bool is_bss_vaddr(vaddr_t vaddr) {
    return (vaddr >= Kbss.vbase && vaddr < Kbss.vbase + Kbss.size);
}

phyaddr_t get_KImage_base() {
    return KImage.pbase;
}

phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr) {
    if (!is_bss_vaddr(vaddr)) return 0;
    uint64_t offset = vaddr - Kbss.vbase;
    return (Kbss.pbase != 0) ? (Kbss.pbase + offset) : 0;
}
