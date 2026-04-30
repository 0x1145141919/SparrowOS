#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

struct symbol_entry {
    uint64_t address;
    char name[119];
    uint8_t type;
};
// 解析 nm 一行，返回 symbol_entry
bool parse_nm_line(const std::string& line, symbol_entry& entry) {
    // line 格式: fffff8000000008de T _ZN8LocalCPUD2Ev
    std::istringstream iss(line);
    std::string addr_str, type_str, name_str;
    if (!(iss >> addr_str >> type_str >> name_str)) return false;

    // 地址
    entry.address = std::stoull(addr_str, nullptr, 16);

    // 类型
    entry.type = static_cast<uint8_t>(type_str[0]);

    // 名字
    std::memset(entry.name, 0, sizeof(entry.name));
    std::strncpy(entry.name, name_str.c_str(), sizeof(entry.name) - 1);

    return true;
}

#ifndef PJ_ROOT_PATH
#define PJ_ROOT_PATH "/home/PS/PS_git/OS_pj_uefi/kernel"
#endif

constexpr char PJ_ROOT[] = PJ_ROOT_PATH;
constexpr char KIMAGE_MAIN_ELF[] = "kernel.elf";
constexpr char INIT_ELF[] = "init.elf";
constexpr char SORTED_NM_TEXT_LIST[] = "ksymbols_sorted.txt";
constexpr char KSYMBOLS_BIN[] = "ksymbols.bin";

int main(int argc, char* argv[]) {
    // 构造完整路径
    std::string elf_path = std::string(PJ_ROOT) + "/" + std::string(KIMAGE_MAIN_ELF);
    std::string txt_path = std::string(PJ_ROOT) + "/" + std::string(SORTED_NM_TEXT_LIST);
    std::string bin_path = std::string(PJ_ROOT) + "/" + std::string(KSYMBOLS_BIN);
    std::string init_elf_path = std::string(PJ_ROOT) + "/" + std::string(INIT_ELF);
    // 先nm|sort对FIRST_STAGE_ELF生成文本中间体到SORTED_NM_TEXT_LIST文件
    std::string nm_cmd = "nm " + elf_path +' '+ init_elf_path + " | sort > " + txt_path;
    
    if (system(nm_cmd.c_str()) != 0) {
        std::cerr << "Error: Failed to run nm command: " << nm_cmd << std::endl;
        return 1;
    }

    // 再打开SORTED_NM_TEXT_LIST文件，解析每一行生成二进制符号表文件ksymbols.bin
    std::ifstream input_file(txt_path);
    if (!input_file.is_open()) {
        std::cerr << "Error: Could not open " << txt_path << std::endl;
        return 1;
    }

    std::vector<symbol_entry> symbols;
    std::string line;
    symbol_entry entry;

    while (std::getline(input_file, line)) {
        if (parse_nm_line(line, entry)) {
            symbols.push_back(entry);
        }
    }
    input_file.close();

    // 将符号表写入二进制文件
    std::ofstream output_file(bin_path, std::ios::binary);
    if (!output_file.is_open()) {
        std::cerr << "Error: Could not create binary file " << bin_path << std::endl;
        return 1;
    }

    for (const auto& sym : symbols) {
        output_file.write(reinterpret_cast<const char*>(&sym.address), sizeof(sym.address));
        output_file.write(sym.name, sizeof(sym.name));
        output_file.write(reinterpret_cast<const char*>(&sym.type), sizeof(sym.type));
    }
    output_file.close();

    // 清理临时文件
    //unlink(txt_path.c_str());
    std::cout << "Successfully generated kernel symbols table." << std::endl;
    return 0;
}