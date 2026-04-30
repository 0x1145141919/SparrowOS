#include "builder.hpp"
#include <iostream>
#include <cstdio>
#include <stdexcept>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <config.json>\n"
        "\n"
        "Build an initramfs image from a JSON configuration file.\n"
        "See Docs/initramfs/ for format details.\n",
        prog);
}

int main(int argc, char** argv) {
    if (argc != 2) { usage(argv[0]); return 1; }

    try {
        std::cout << "[initramfs-builder] config: " << argv[1] << std::endl;
        auto cfg = parse_config(argv[1]);
        std::cout << "[initramfs-builder] building " << cfg.files.size()
                  << " file(s)..." << std::endl;
        auto out = build_initramfs(cfg);
        std::cout << "[initramfs-builder] done -> " << out << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[initramfs-builder] error: " << e.what() << std::endl;
        return 1;
    }
}
