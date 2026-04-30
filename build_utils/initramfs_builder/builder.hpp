#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <initramfs/fs_format.h>

// Reuse kernel's fs_format.h definitions.
// INITRAMFS_MAGIC = 0x34a6346adb9e0fe2, version = 0

struct FileSource {
    std::string dest_path;
    std::string src_base;
    std::string src_relative;
};

struct Config {
    std::vector<FileSource> files;
    std::string out_base;
    std::string out_relative;
};

Config parse_config(const std::string& config_path);
std::string build_initramfs(const Config& cfg);
std::string resolve_vars(const std::string& s,
                         const std::map<std::string, std::string>& vars);
