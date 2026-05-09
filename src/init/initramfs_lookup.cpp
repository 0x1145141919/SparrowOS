#include "init/initramfs_lookup.h"

/**
 * initramfs_lookup — 按路径匹配查找文件
 *
 * 遍历 initramfs 文件表，对每个条目用路径末尾的相对路径做 strcmp。
 * 匹配成功则返回物理基址，失败返回 0。
 *
 * 不进行任何动态分配，不依赖 libc。
 */
uint64_t initramfs_lookup(const initramfs_header* hdr, const char* path, uint64_t* out_size) {
    if (!hdr || !path)
        return 0;

    // 校验魔数
    if (hdr->magic != INITRAMFS_MAGIC)
        return 0;

    // 元数据段基址（文件表 + 路径字符串）
    const uint8_t* meta_base = reinterpret_cast<const uint8_t*>(hdr) + hdr->metadata_seg_offset;

    // 数据段基址
    const uint8_t* data_base = reinterpret_cast<const uint8_t*>(hdr) + hdr->data_seg_offset;

    // 文件表起始
    const file_entry* entries = reinterpret_cast<const file_entry*>(meta_base);
    uint64_t count = hdr->file_entry_count;

    // 计算调用方路径长度（提前退出用）
    uint64_t path_len = 0;
    for (const char* p = path; *p; p++)
        path_len++;

    for (uint64_t i = 0; i < count; i++) {
        const char* entry_path = reinterpret_cast<const char*>(meta_base + entries[i].file_path_in_metadata_offset);

        // 长度快速过滤
        bool match = false;
        const char* ep = entry_path;
        for (uint64_t j = 0; j <= path_len; j++) {
            if (ep[j] != path[j]) {
                match = false;
                break;
            }
            if (ep[j] == '\0') {
                match = true;
                break;
            }
        }

        if (match) {
            if (out_size)
                *out_size = entries[i].file_size;
            return reinterpret_cast<uint64_t>(data_base + entries[i].file_in_dataseg_offset);
        }
    }

    return 0;
}
