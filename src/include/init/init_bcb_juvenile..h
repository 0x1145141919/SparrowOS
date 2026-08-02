#pragma once
#include <stdint.h>
#include <abi/src_loc.h>
#include <memory/memory_base.h>
#include <abi/bcb_handoff.h>

struct bcb_state {
    phyaddr_t base;
    uint8_t   max_order;
    uint64_t  region_va;        // 本 BCB 位图区虚拟地址（UEFI 恒等映射，== 物理地址）
    uint64_t  free_leaves;      // = free_count[0]
    uint64_t  scan_hint_off_;   // 本 BCB 内 first-fit 游标
};

class init_bcb_juvenile {
public:
    bcb_state*  bcbs_;
    uint64_t    bcb_count_;
    phyaddr_t   region_pbase_;  // 位图区物理基址（连续分配，逐 BCB 切片）
    uint64_t    region_size_;
    uint64_t    scan_hint_bcb_; // 跨 BCB first-fit 游标
    bcb_desc_t* descs_;         // 堆上升序描述数组，get_descs/get_desc_count 暴露

    // 生命周期：从 basic_allocator 纯净视图算 plan → 铺叶子（全空闲）→ 活
    // 排他性（init 镜像/header/loaded files/low-1MB/位图区）由调用方在视图层标记，
    // 分配器只管理 free/occupied，不引入 reserved 语义
    loc_code_t plan_and_setup();                       // 0 成功；失败返回 SRC_LOC()

    // 分配 count 页连续空闲，基址对齐 2^align_log2 字节（align_log2 ∈ [12,30]）
    phyaddr_t alloc(uint64_t count, uint8_t align_log2, loc_code_t* err);   // 失败 0

    // 归还
    loc_code_t free(phyaddr_t base, uint64_t count);

    // 查询
    uint64_t free_page_count() const;
    uint64_t total_page_count() const;

    // 交接
    const bcb_desc_t* get_descs() const;   // 堆上升序描述数组
    uint64_t get_desc_count() const;

private:
    // 叶子位图访问（复用 layout 常量，位偏移 = 2^(N+1) + offset）
    bool leaf_test(uint64_t bcb_idx, uint64_t off) const;
    void leaf_set (uint64_t bcb_idx, uint64_t off, bool free);
    // 带对齐 run 扫描：跨 BCB，物理对齐（start 物理地址 ≡ 0 mod 2^align_log2）
    phyaddr_t scan_aligned_run(uint64_t count, uint8_t align_log2, loc_code_t* err);
    int64_t   bcb_for_addr(phyaddr_t addr) const;     // 二分
};
