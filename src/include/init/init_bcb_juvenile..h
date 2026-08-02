#pragma once
#include <stdint.h>
#include <abi/src_loc.h>
#include <memory/memory_base.h>
#include <abi/bcb_handoff.h>
struct bcb_state {
    phyaddr_t base;
    uint8_t   max_order;
    //uint64_t  leaf_bitmap_va;   // 叶子区地址 = region_va + 2^(N-2) 字节
    uint64_t  region_va;        //架构选择就用这个，因为leaf_bitmap_va是复合量反而引入了不必要的复杂度，不够kiss
    //uint64_t  leaf_bytes;       // 2^N bits，也被注释掉，画蛇添足
    uint64_t  free_leaves;      // = free_count[0]
    uint64_t scan_hint_off_;   // first-fit 游标
};

class init_bcb_juvenile {
public:
    bcb_state*  bcbs_;  uint64_t bcb_count_;
    phyaddr_t   region_pbase_;  uint64_t region_size_;
    //uint64_t    total_pages_;  可以根据bcb_state*  bcbs_+bcb_count_测算
    //phyaddr_t dram_top_; 这里不应该有这个概念，吃下basic_allocator的纯净内存视图后进行BCBs初始化的时候尾巴上可能有剩余。不绝对代表真正的dram_top
    uint64_t    scan_hint_bcb_;  
    bcb_desc_t* descs_;         // 堆上升序描述数组，get_descs/get_desc_count 暴露
    //page*       mem_map_;  uint64_t mem_map_pages_;  phyaddr_t mem_map_pbase_;  // pages_arr
    //init.elf阶段彻底不需要page*mem_map只需要BCBs记录空闲占用信息即可
    // 生命周期：从 basic_allocator 纯净视图算 plan → 铺叶子 → 标记保留区 → 活
    loc_code_t plan_and_setup();                       // 0 成功；失败返回 SRC_LOC()

    // 分配 count 页连续空闲，基址对齐 2^align_log2 字节（align_log2 ∈ [12,30]）
    phyaddr_t alloc(uint64_t count, uint8_t align_log2, loc_code_t* err);   // 失败 0

    // 归还
    loc_code_t free(phyaddr_t base, uint64_t count);

    // 保留区标记（调用方标记 header/loaded files 等非纯内存视图可排除的占用）
    loc_code_t mark_reserved(phyaddr_t base, uint64_t size);

    // 查询
    uint64_t free_page_count() const;
    uint64_t total_page_count() const;
    //phyaddr_t dram_top() const;

    // 交接
    const bcb_desc_t* get_descs() const;//堆上分配的一个升序数组，数目需要下面的 get_desc_count获得 
    uint64_t get_desc_count() const;
    //mem_interval      get_bitmap_region() const;      // {pa, size} 穿越，但是实际上新的bcb_desc_t让这个接口下岗
    //void relinquish_pages_arr(phyaddr_t* pbase, uint64_t* pcount);

private:
    // 叶子位图访问（复用 layout 常量，位偏移 = 2^(N+1) + offset）
    bool leaf_test(uint64_t bcb_idx, uint64_t off) const;
    void leaf_set (uint64_t bcb_idx, uint64_t off, bool free);
    // 带对齐 run 扫描：跨 BCB，物理对齐（start 物理地址 ≡ 0 mod 2^align_log2）
    phyaddr_t scan_aligned_run(uint64_t count, uint8_t align_log2, loc_code_t* err);
    int64_t   bcb_for_addr(phyaddr_t addr) const;     // 二分
    void      mark_occupied(phyaddr_t base, uint64_t size);  // 保留区入叶子
};
