#pragma once
#include <stdint.h>
#include "memory/memory_base.h"

void self_introspection_init(vm_interval KImage, vm_interval Kbss);

extern "C" bool is_bss_vaddr(vaddr_t vaddr);
extern "C" phyaddr_t get_KImage_base();
extern "C" phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr);

// ---- 元信息查询 ----

/** @brief 返回 .comment 节字符串（工具链版本信息），无 comment 返回 nullptr */
const char* get_KImage_comment();

/** @brief 返回 .note.gnu.build-id（SHA1 二进制），*out_size 为字节数 */
const void* get_KImage_build_id(uint64_t* out_size);

/** @brief 按名字查找 NOTE 条目，返回 payload 指针，*out_size 为大小 */
const void* get_KImage_note(const char* name, uint64_t* out_size);

/** @brief 按节名查找 Shdr.p_shdr 中解析出来的节头直接返回其虚拟地址与大小 */
struct kimg_section {
    const char* name;       // .shstrtab 中的名字
    uint64_t    vaddr;      // 虚拟地址（只读/非分配节为 0）
    uint64_t    size;       // 节大小
    uint64_t    file_off;   // 文件偏移
    uint32_t    flags;      // SHF_* 标志
};
const kimg_section* get_KImage_sections(uint64_t* out_count);
