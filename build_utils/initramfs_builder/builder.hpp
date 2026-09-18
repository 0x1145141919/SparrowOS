#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <initramfs/fs_format.h>
#include <boot/boot_cfg.h>

// Reuse kernel's fs_format.h definitions, and boot/boot_cfg.h as the
// single source of the boot.cfg wire contract (key ids / names / blob layout).
// INITRAMFS_MAGIC = 0x34a6346adb9e0fe2, version = 0

struct FileSource {
    std::string dest_path;
    std::string src_base;
    std::string src_relative;
};

// boot_cfg 单条（构建期 JSON → /boot.cfg blob）
struct BootCfgEntry {
    uint16_t key;
    uint64_t value;
};

struct Config {
    std::vector<FileSource> files;
    std::vector<BootCfgEntry> boot_cfg;   // 可选：为空则不产出 /boot.cfg
    std::string out_base;
    std::string out_relative;
};

Config parse_config(const std::string& config_path);
std::string build_initramfs(const Config& cfg);
std::string resolve_vars(const std::string& s,
                         const std::map<std::string, std::string>& vars);
