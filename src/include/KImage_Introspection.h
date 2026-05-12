#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "elf.h"

void self_introspection_init(vm_interval KImage, vm_interval Kbss);
extern "C" bool is_bss_vaddr(vaddr_t vaddr);
extern "C" phyaddr_t get_KImage_base();
extern "C" phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr);

// ---- 节头表查询 ----

/** @brief 返回 Shdr 表起始地址，*out_count 为条目数 */
const Elf64_Shdr* get_KImage_sections(uint64_t* out_count);

/** @brief 按节名查找 Shdr，返回 nullptr 表示不存在 */
const Elf64_Shdr* get_KImage_section(const char* name);

// ---- 程序头表查询 ----

/** @brief 返回 Phdr 表起始地址，*out_count 为条目数 */
const Elf64_Phdr* get_KImage_phdrs(uint64_t* out_count);

/** @brief 按类型值查找首匹配的 Phdr */
const Elf64_Phdr* get_KImage_phdr_by_type(uint32_t p_type);

/** @brief 按类型名字符串查找，如 "PT_LOAD"、"PT_NOTE" */
const Elf64_Phdr* get_KImage_phdr_by_name(const char* type_name);

// ---- 元信息查询 ----

/** @brief 返回 .comment 节字符串（工具链版本），无 comment 返回 nullptr */
const char* get_KImage_comment();

/** @brief 返回 .note.gnu.build-id 内容，*out_size 为字节数 */
const void* get_KImage_build_id(uint64_t* out_size);

/** @brief 按名字在 NOTE 段中查找条目，返回 payload 指针 */
const void* get_KImage_note(const char* name, uint64_t* out_size);
