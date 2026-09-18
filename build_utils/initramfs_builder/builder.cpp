#include "builder.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── boot_cfg 键名 → id：直接用头文件里的注册表（boot/boot_cfg.h，唯一来源）──
// 未知键名在构建期直接报错（防笔误进运行时）。
namespace {

uint16_t boot_cfg_key_id(const std::string& name) {
    for (const auto& k : BOOT_CFG_KEY_TABLE)
        if (name == k.name) return k.id;
    std::string valid;
    for (const auto& k : BOOT_CFG_KEY_TABLE) { valid += " "; valid += k.name; }
    throw std::runtime_error("boot_cfg: unknown key '" + name + "' (valid:" + valid + ")");
}
}  // namespace

Config parse_config(const std::string& config_path) {
    std::ifstream ifs(config_path);
    if (!ifs.is_open())
        throw std::runtime_error("cannot open config: " + config_path);
    auto root = json::parse(ifs);

    // --- Variables ---
    std::map<std::string, std::string> vars;
    if (root.contains("variables") && root["variables"].is_object()) {
        for (auto& [k, v] : root["variables"].items())
            vars[k] = v.get<std::string>();
    }

    // --- Files ---
    Config cfg;
    if (!root.contains("files") || !root["files"].is_array())
        throw std::runtime_error("config must have a 'files' array");

    for (const auto& entry : root["files"]) {
        FileSource fsrc;
        if (!entry.contains("dest_path"))
            throw std::runtime_error("file entry missing 'dest_path'");
        if (!entry.contains("src_relative_path"))
            throw std::runtime_error("file entry missing 'src_relative_path'");

        fsrc.dest_path    = resolve_vars(entry["dest_path"].get<std::string>(), vars);
        fsrc.src_relative = resolve_vars(entry["src_relative_path"].get<std::string>(), vars);
        fsrc.src_base     = entry.contains("src_base")
            ? resolve_vars(entry["src_base"].get<std::string>(), vars) : "";
        cfg.files.push_back(std::move(fsrc));
    }

    // --- boot_cfg（可选）：键名 → id + 值（bool/int）---
    if (root.contains("boot_cfg")) {
        if (!root["boot_cfg"].is_object())
            throw std::runtime_error("'boot_cfg' must be an object");
        for (auto& [k, v] : root["boot_cfg"].items()) {
            uint64_t val;
            if (v.is_boolean())              val = v.get<bool>() ? 1u : 0u;
            else if (v.is_number_unsigned()) val = v.get<uint64_t>();
            else if (v.is_number_integer())  val = (uint64_t)v.get<int64_t>();
            else throw std::runtime_error("boot_cfg['" + k + "'] must be bool/integer");
            cfg.boot_cfg.push_back({ boot_cfg_key_id(k), val });
        }
    }

    // --- Output path ---
    if (!root.contains("out_put_path") || !root["out_put_path"].is_object())
        throw std::runtime_error("config must have an 'out_put_path' object");

    const auto& out = root["out_put_path"];
    cfg.out_relative = resolve_vars(out["src_relative_path"].get<std::string>(), vars);
    cfg.out_base     = out.contains("src_base")
        ? resolve_vars(out["src_base"].get<std::string>(), vars) : "";

    return cfg;
}

std::string resolve_vars(const std::string& s,
                         const std::map<std::string, std::string>& vars) {
    std::string result = s;
    for (const auto& [key, val] : vars) {
        std::string pattern = "${" + key + "}";
        size_t pos = 0;
        while ((pos = result.find(pattern, pos)) != std::string::npos) {
            result.replace(pos, pattern.length(), val);
            pos += val.length();
        }
    }
    return result;
}

static std::string resolve_path(const std::string& base,
                                const std::string& relative) {
    if (base.empty()) return relative;
    fs::path p(base);
    p /= relative;
    return p.lexically_normal().string();
}

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open())
        throw std::runtime_error("cannot read source file: " + path);
    size_t size = ifs.tellg();
    ifs.seekg(0);
    std::vector<uint8_t> buf(size);
    ifs.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

static uint64_t pad_to(uint64_t offset, uint64_t alignment) {
    uint64_t rem = offset % alignment;
    return rem == 0 ? offset : offset + (alignment - rem);
}

std::string build_initramfs(const Config& cfg) {
    // --- Phase 1: read source files ---
    struct Filedata {
        std::string dest_path;
        std::vector<uint8_t> content;
    };
    std::vector<Filedata> files;
    for (const auto& f : cfg.files) {
        std::string src = resolve_path(f.src_base, f.src_relative);
        std::cout << "  [read] " << src << "  ->  " << f.dest_path << std::endl;
        files.push_back({f.dest_path, read_file(src)});
    }

    // --- boot_cfg → /boot.cfg blob（可选；非源文件，构建期合成）---
    if (!cfg.boot_cfg.empty()) {
        const size_t hdr_sz = sizeof(boot_cfg_blob_header);
        const size_t rec_sz = sizeof(boot_cfg_blob_record);
        std::vector<uint8_t> blob(hdr_sz + cfg.boot_cfg.size() * rec_sz, 0);
        auto* bh  = reinterpret_cast<boot_cfg_blob_header*>(blob.data());
        bh->magic = BOOT_CFG_BLOB_MAGIC;
        bh->count = (uint32_t)cfg.boot_cfg.size();
        auto* rec = reinterpret_cast<boot_cfg_blob_record*>(blob.data() + hdr_sz);
        for (size_t i = 0; i < cfg.boot_cfg.size(); ++i) {
            rec[i].key      = cfg.boot_cfg[i].key;
            rec[i].reserved = 0;
            rec[i].value    = cfg.boot_cfg[i].value;
        }
        std::cout << "  [gen ] /boot.cfg (" << cfg.boot_cfg.size() << " keys, "
                  << blob.size() << " bytes)" << std::endl;
        files.push_back({ "/boot.cfg", std::move(blob) });
    }

    // --- Phase 2: calculate offsets ---
    uint64_t header_sz   = sizeof(initramfs_header);
    uint64_t nfiles      = files.size();
    uint64_t meta_off    = header_sz;
    uint64_t entries_sz  = nfiles * sizeof(file_entry);

    std::vector<uint64_t> path_offs(nfiles);
    uint64_t cur = entries_sz;
    for (size_t i = 0; i < nfiles; i++) {
        path_offs[i] = cur;
        cur += files[i].dest_path.size() + 1;
    }

    uint64_t meta_sz        = cur;
    uint64_t meta_sz_padded = pad_to(meta_sz, 8);
    uint64_t data_off       = meta_off + meta_sz_padded;

    std::vector<uint64_t> data_offs(nfiles);
    uint64_t data_cur = 0;
    for (size_t i = 0; i < nfiles; i++) {
        data_offs[i] = data_cur;
        data_cur += files[i].content.size();
    }
    uint64_t data_sz = data_cur;

    // --- Phase 3: write output ---
    std::string out_path = resolve_path(cfg.out_base, cfg.out_relative);
    auto parent = fs::path(out_path).parent_path();
    if (!parent.empty())
        fs::create_directories(parent);

    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs.is_open())
        throw std::runtime_error("cannot write output: " + out_path);

    initramfs_header hdr{};
    hdr.magic               = INITRAMFS_MAGIC;
    hdr.version             = 0;
    hdr.flags               = 0;
    hdr.file_entry_count    = nfiles;
    hdr.metadata_seg_offset = meta_off;
    hdr.metadata_seg_size   = meta_sz;
    hdr.data_seg_offset     = data_off;
    hdr.data_seg_size       = data_sz;
    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    for (size_t i = 0; i < nfiles; i++) {
        file_entry fe{};
        fe.file_size                    = files[i].content.size();
        fe.file_path_in_metadata_offset = path_offs[i];
        fe.file_in_dataseg_offset       = data_offs[i];
        ofs.write(reinterpret_cast<const char*>(&fe), sizeof(fe));
    }

    for (size_t i = 0; i < nfiles; i++) {
        ofs.write(files[i].dest_path.data(), files[i].dest_path.size());
        ofs.put('\0');
    }

    for (uint64_t i = meta_sz; i < meta_sz_padded; i++)
        ofs.put(0);

    for (size_t i = 0; i < nfiles; i++)
        ofs.write(reinterpret_cast<const char*>(files[i].content.data()),
                  files[i].content.size());

    ofs.close();
    std::cout << "  [write] " << out_path
              << " (" << (header_sz + meta_sz_padded + data_sz) << " bytes)"
              << std::endl;
    return out_path;
}
