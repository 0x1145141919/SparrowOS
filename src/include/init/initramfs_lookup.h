#pragma once
#include <stdint.h>
#include "initramfs/fs_format.h"

/**
 * @brief 在 initramfs 中按相对路径查找文件
 *
 * initramfs 内部分为 [header][元数据(文件表+路径字符串)][数据段] 三段。
 * 遍历文件表，精确匹配路径字符串，返回对应文件的物理基址。
 *
 * 此函数在 init.elf 阶段调用（identity mapping，物理地址可直接访问）。
 * 不依赖堆、不依赖文件系统、无动态分配。
 *
 * @param hdr      initramfs_header 的物理地址指针
 * @param path     相对路径（如 "/kernel.elf"），以 '\0' 结尾
 * @param out_size 输出：文件字节大小（可为 NULL）
 * @return uint64_t 文件数据的物理基址；0 表示未找到
 */
uint64_t initramfs_lookup(const initramfs_header* hdr, const char* path, uint64_t* out_size);
