#include "KImage_Introspection.h"
#include "util/kout.h"
#include <cstring>

// ============================================================================
// 最小化 ELF64 结构体（无 elf.h，自给自足）
// ============================================================================
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Shdr {
    uint32_t sh_name;       // .shstrtab 中的偏移
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;       // 虚拟地址（非 ALLOC 则为 0）
    uint64_t sh_offset;     // 文件偏移
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

// ELF 常量
static const uint32_t SHT_NULL     = 0;
static const uint32_t SHT_PROGBITS = 1;
static const uint32_t SHT_NOBITS   = 8;
static const uint32_t SHT_NOTE     = 7;
static const uint32_t SHF_ALLOC    = 2;

// NOTE 条目头部
struct Elf64_Nhdr {
    uint32_t n_namesz;      // 名字长度（含结尾 \0，对齐到 4 字节）
    uint32_t n_descsz;      // 描述大小（对齐到 4 字节）
    uint32_t n_type;        // 类型
};

// NOTE 类型
static const uint32_t NT_GNU_BUILD_ID = 3;

// ============================================================================
// 内部状态
// ============================================================================
vm_interval KImage, Kbss;

// 最大支持的节数（kernel.elf 目前 22 个节）
static const uint64_t MAX_SECTIONS = 64;
static kimg_section   sg_sections[MAX_SECTIONS];
static uint64_t        sg_section_count = 0;
static const char*    sg_shstrtab      = nullptr;  // .shstrtab 在 vaddr 空间的地址

// ============================================================================
// init — 解析 ELF 节头表
// ============================================================================
void self_introspection_init(vm_interval KImage_, vm_interval Kbss_) {
    KImage = KImage_;
    Kbss   = Kbss_;

    sg_section_count = 0;

    // KImage.vbase = kIMG_self_window 的 vaddr（覆盖整个 kernel.elf 文件）
    uint64_t file_base = KImage.vbase;
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file_base);

    // 魔数校验
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        bsp_kout << "[KImage] ERROR: bad ELF magic" << kendl;
        return;
    }

    uint64_t shoff     = ehdr->e_shoff;
    uint16_t shnum     = ehdr->e_shnum;
    uint16_t shentsize = ehdr->e_shentsize;
    uint16_t shstrndx  = ehdr->e_shstrndx;

    if (shnum > MAX_SECTIONS) shnum = MAX_SECTIONS;

    // 先解析 .shstrtab 自身
    const auto* shdr_arr = reinterpret_cast<const Elf64_Shdr*>(file_base + shoff);
    const auto& shstr_sh = shdr_arr[shstrndx];
    sg_shstrtab = reinterpret_cast<const char*>(file_base + shstr_sh.sh_offset);

    // 遍历所有节
    for (uint16_t i = 0; i < shnum; i++) {
        const auto& sh = shdr_arr[i];
        auto& out = sg_sections[sg_section_count];
        out.name    = (sh.sh_type != SHT_NULL) ? (sg_shstrtab + sh.sh_name) : "";
        out.vaddr   = sh.sh_addr;
        out.size    = sh.sh_size;
        out.file_off = sh.sh_offset;
        out.flags   = sh.sh_flags;
        sg_section_count++;

        if (sh.sh_type != SHT_NULL) {
            bsp_kout << "[KImage] section " << i << ": "
                     << out.name << " vaddr=0x" << out.vaddr
                     << " off=0x" << out.file_off
                     << " size=0x" << out.size << kendl;
        }
    }
}

// ============================================================================
// get_KImage_sections — 返回所有节信息
// ============================================================================
const kimg_section* get_KImage_sections(uint64_t* out_count) {
    if (out_count) *out_count = sg_section_count;
    return sg_sections;
}

// ============================================================================
// 按名字查找节
// ============================================================================
static const kimg_section* find_section(const char* name) {
    for (uint64_t i = 0; i < sg_section_count; i++) {
        if (std::strcmp(sg_sections[i].name, name) == 0)
            return &sg_sections[i];
    }
    return nullptr;
}

// ============================================================================
// get_KImage_comment — 返回 .comment 节字符串
// ============================================================================
const char* get_KImage_comment() {
    auto* sh = find_section(".comment");
    if (!sh || sh->size == 0) return nullptr;
    return reinterpret_cast<const char*>(KImage.vbase + sh->file_off);
}

// ============================================================================
// get_KImage_note — 按名字在 NOTE 节中查找条目
//
// NOTE 节格式：
//   [Elf64_Nhdr][name 对齐到 4B][desc 对齐到 4B]
//   ...
// ============================================================================
const void* get_KImage_note(const char* name, uint64_t* out_size) {
    auto* sh = find_section(".note");
    if (!sh || sh->size == 0) return nullptr;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(KImage.vbase + sh->file_off);
    uint64_t pos = 0;

    while (pos + sizeof(Elf64_Nhdr) <= sh->size) {
        const auto* nhdr = reinterpret_cast<const Elf64_Nhdr*>(base + pos);
        uint32_t namesz = (nhdr->n_namesz + 3) & ~3;   // 对齐到 4
        uint32_t descsz = (nhdr->n_descsz + 3) & ~3;
        uint64_t next   = pos + sizeof(Elf64_Nhdr) + namesz + descsz;

        if (next > sh->size) break;

        const char* entry_name = reinterpret_cast<const char*>(base + pos + sizeof(Elf64_Nhdr));
        if (nhdr->n_namesz > 0 && std::strcmp(entry_name, name) == 0) {
            if (out_size) *out_size = nhdr->n_descsz;
            return base + pos + sizeof(Elf64_Nhdr) + namesz;
        }
        pos = next;
    }
    return nullptr;
}

// ============================================================================
// get_KImage_build_id — 获取 .note.gnu.build-id
// ============================================================================
const void* get_KImage_build_id(uint64_t* out_size) {
    return get_KImage_note("GNU", out_size);
}

// ============================================================================
// 以下为已有接口（保持不变）
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
    if (Kbss.pbase != 0)
        return Kbss.pbase + offset;
    return 0;
}
