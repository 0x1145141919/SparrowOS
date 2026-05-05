#pragma once
#include <stdint.h>
#include <abi/os_error_definitions.h>

struct prp_root_t {
    uint64_t prp1;
    uint64_t prp2;
    uint64_t list_head_pa;
    uint32_t list_page_count;
    uint32_t page_count;
};

struct mem_segs_t {
    uint64_t count;
    struct entry_t {
        uint64_t base;
        uint64_t nuof_4kbpgs;
    };
    entry_t* entries;
};

// root_out 必须由调用方提供存储，函数填充后用于 destroy_PRP_root 清理
KURD_t build_PRP_root(uint64_t pbase, uint32_t page_count,
                       uint32_t mps_shift, prp_root_t* root_out, KURD_t& kurd);

KURD_t build_PRP_root_advance(mem_segs_t& segs,
                               uint32_t mps_shift,
                               prp_root_t* root_out, KURD_t& kurd);

KURD_t destroy_PRP_root(const prp_root_t& root,
                         uint32_t mps_shift, KURD_t& kurd);
