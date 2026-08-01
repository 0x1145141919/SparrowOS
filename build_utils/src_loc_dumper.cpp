#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cctype>

// 复用内核侧 src_loc.h 的 hash44，保证宿主与内核同一算法、杜绝漂移
#include "../src/include/abi/src_loc.h"

#ifndef PJ_ROOT_PATH
#define PJ_ROOT_PATH "/home/PS/PS_git/OS_pj_uefi/kernel"
#endif

constexpr char SRCLOC_JSON[] = "srcloc.json";

namespace fs = std::filesystem;

struct src_entry {
    uint64_t hash;
    std::string path;   // 相对 base_dir 的路径
};

static bool is_source_file(const fs::path& p)
{
    static const char* exts[] = {".c", ".cpp", ".cc", ".cxx",
                                 ".h", ".hh", ".hpp", ".hxx",
                                 ".asm", ".s", ".S"};
    std::string ext = p.extension().string();
    for (const char* e : exts) {
        if (ext == e) return true;
    }
    return false;
}

static std::string to_json_string(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

int main(int argc, char* argv[])
{
    std::string root = PJ_ROOT_PATH;
    if (argc > 1) root = argv[1];

    std::vector<src_entry> entries;
    // 只遍历 src/（内核源码根），跳过构建残留目录
    fs::path src_root = fs::path(root) / "src";

    if (!fs::exists(src_root)) {
        std::cerr << "Error: source root not found: " << src_root << std::endl;
        return 1;
    }

    for (fs::recursive_directory_iterator it(src_root), end; it != end; ++it) {
        auto& de = *it;
        if (de.is_directory()) {
            const std::string name = de.path().filename().string();
            // 跳过 CMake 构建残留与点目录
            if (name == "CMakeFiles" || name == "cmake_install.cmake" || name[0] == '.') {
                it.disable_recursion_pending();
            }
            continue;
        }
        const fs::path fp = de.path();
        if (!is_source_file(fp)) continue;
        std::string rel = fs::relative(fp, fs::path(root)).string();
        std::string abs = (fs::path(root) / rel).lexically_normal().string();
        uint64_t h = src_loc::hash44(abs.c_str());
        entries.push_back({h, rel});
    }

    if (entries.empty()) {
        std::cerr << "Error: no source files collected under " << src_root << std::endl;
        return 1;
    }

    // 按 hash 升序排列（解码侧二分查找的前提）
    std::sort(entries.begin(), entries.end(),
              [](const src_entry& a, const src_entry& b) { return a.hash < b.hash; });

    std::string out_path = (fs::path(root) / SRCLOC_JSON).string();
    std::ofstream of(out_path);
    if (!of.is_open()) {
        std::cerr << "Error: cannot write " << out_path << std::endl;
        return 1;
    }

    of << "{\n";
    of << "  \"base_dir\": \"" << to_json_string((fs::path(root)).string()) << "\",\n";
    of << "  \"table\": [\n";
    for (size_t i = 0; i < entries.size(); i++) {
        of << "    { \"hash\": " << entries[i].hash
           << ", \"path\": \"" << to_json_string(entries[i].path) << "\" }";
        if (i + 1 < entries.size()) of << ",";
        of << "\n";
    }
    of << "  ]\n";
    of << "}\n";
    of.close();

    std::cout << "Successfully generated " << out_path
              << " with " << entries.size() << " entries." << std::endl;
    return 0;
}
