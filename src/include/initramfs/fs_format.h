#pragma once 
#include <stdint.h>
constexpr uint64_t INITRAMFS_MAGIC = 0x34a6346adb9e0fe2;
constexpr uint64_t version=0;
struct file_entry{
    uint64_t file_size;
    uint64_t file_path_in_metadata_offset;
    uint64_t file_in_dataseg_offset;
};
struct initramfs_header{
    uint64_t magic;
    uint64_t version;
    uint64_t flags;
    uint64_t file_entry_count;
    uint64_t metadata_seg_offset;
    uint64_t metadata_seg_size;
    uint64_t data_seg_offset;
    uint64_t data_seg_size;
};