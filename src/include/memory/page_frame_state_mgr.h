#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "abi/src_loc.h"
#include "abi/boot.h"   // movable_file_entry_t（pages_arr movable 纯物理描述符）

// ════════════════════════════════════════════════════════════════
// page_frame_state_mgr — 页框状态管理器（原 all_pages_arr 重构）
//
// 持有数据（adopt 一次性构建，唯一权威结构）：
//   - pages_arr（page[] 状态数组，1B/页）：init 穿越的 mem_map，收养即冻结不覆写
//   - intervals[]（有序区间数组）：base + numof4kbpgs + baseidx_in_memmap，
//     既作 phyaddr → mem_map 索引二分定位表，也作可分配区域边界
//   - early_alloc_closed：初始分配关闭控制位
//
// 空闲段（free runs）不持有：是 intervals + mem_map 的投影。FPA 收养直接拿
// intervals_snapshot() 的全量 interval 副本（摘除 scan_base_idx 字段，无论状态），
// 由 FPA 新初始化自行分桶、从脏 order-0 向上折叠，无需预解析 run 表。
//
// 状态接口分两类：
//   - 物理地址型（state_set / state_query / kind_check）：经 intervals 定位，
//     kind_check / idx_base_kind_check 返回 bool（全找到且全匹配才 true）。
//   - 纯索引型（idx_base_*）：直接操作 pages_arr 内部条目下标，刻意不做
//     跨 interval 检查，只做越界校验。
//
// 错误约定（开发期）：SRC_LOC() 应急盖戳（见 abi/src_loc.h），不编排 KURD。
//   adopt/state_set/state_query/idx_base_state_* → loc_code_t：0=成功，非0=SRC_LOC()。
//   early_alloc → phyaddr_t：成功=物理基址，失败=0。
// ════════════════════════════════════════════════════════════════

class page_frame_state_mgr {
public:
    // 空闲段瞬态结构（free_segs_get 返回类型；FPA 消费后释放）
    /*struct free_segs_t {
        uint64_t count;
        struct entry_t { phyaddr_t base; uint64_t size; };
        entry_t* entries;
    };*/
    //不需要
private:
    static page*    pages_arr;             // mem_map 状态数组（权威账本）
    static uint64_t pages_arr_entry_count; // = pages_arr 字节数（sizeof(page)==1）
    static bool     early_alloc_closed;    // 初始分配关闭控制位

    // 有序区间数组（adopt 从 free_segs_descriptors + phymem_segments 解析构建）
    struct interval_t {
        uint64_t base;               // 经全局 phymem_segments 解析（纯洁视图逐字拷贝）
        uint64_t numof4kbpgs;
        uint64_t baseidx_in_memmap;
        uint64_t scan_base_idx;
    };
    static interval_t* intervals;
    static uint64_t    intervals_count;

    static interval_t* locate_interval(phyaddr_t base);   // 二分定位

public:
    // 收养（exec_env_prepare 早期，早于 FPA）：
    //   pages_arr_file — pages_arr movable 资产（纯物理描述符 base_ppn/size，无 KMMU
    //                     映射）。本模块经主窗口（PHYACC_VA）把物理基址重链成可访问
    //                     VA 作为 mem_map 线性基址——调用方必须先 PhyAddrAccessor::Init。
    //   free_segs/count — free_segs_descriptors_table（init 穿越，索引式）
    // 收养 = 构建 intervals + 记账，不覆写 mem_map 状态。
    static loc_code_t adopt(const movable_file_entry_t* pages_arr_file,
                            const free_seg_descriptor_t* free_segs,
                            uint64_t free_segs_count);

    // ── 状态接口 ──
    static loc_code_t state_set(phyaddr_t phybase, uint64_t page_count, page_state_t state);
    static loc_code_t state_query(phyaddr_t phyaddr, page_state_t* out_state);
    // 区间页框种类校验：仅当全部页框在 pages_arr 内被找到且状态都 == expected 才返回 true，其它 false
    static bool kind_check(phyaddr_t phybase, uint64_t page_count, page_state_t expected);
    // 下面三个 idx_base 接口只基于 pages_arr 内部索引（mem_map 条目下标），
    // 刻意不做跨 interval 检查——只做越界（pages_arr_entry_count）校验。
    static loc_code_t idx_base_state_set(uint64_t base_idx, uint64_t page_count, page_state_t state);
    static loc_code_t idx_base_state_query(uint64_t base_idx, page_state_t* out_state);
    // 越界直接 false
    static bool idx_base_kind_check(uint64_t base_idx, uint64_t page_count, page_state_t expected);

    // ── 地址 / pages_arr 索引互转（只做合法性校验，不改状态）──
    // phyaddr → mem_map 条目下标；不在任何 interval 内 / 越界返回 SRC_LOC()
    static loc_code_t phyaddr_to_page_idx(phyaddr_t phyaddr, uint64_t* out_idx);
    // 下标 → 物理基址；下标越界/无归属区间返回 SRC_LOC()
    static loc_code_t page_idx_to_phyaddr(uint64_t idx, phyaddr_t* out_phyaddr);

    // ── 内部 interval 数组暴露（FPA 收养用）──
    // 副本结构：摘除 scan_base_idx
    struct interval_desc_t {
        uint64_t base;               // 经全局 phymem_segments 解析的物理基址
        uint64_t numof4kbpgs;
        uint64_t baseidx_in_memmap;  // 对应区间起始页在 pages_arr 的条目下标
    };
    static uint64_t intervals_count_get();   // 内部区间总数
    // 堆上分配全部 interval 的副本（摘除 scan_base_idx 字段，不按状态/光标过滤——
    //   FPA 新初始化自行分桶、从脏 order-0 向上折叠）；副本归调用方所有（delete[]）。
    // 失败返回 nullptr 且 out_count 置 0。
    static interval_desc_t* intervals_snapshot(uint64_t* out_count);

    // ── 初始时期接口 ──
    static phyaddr_t early_alloc(uint64_t page_count, uint8_t align_log2,
                                 page_state_t state = page_state_t::kernel_persisit);
    static void      early_alloc_close();   // 翻转控制位；翻转后 early_alloc 返回 0
    static bool      early_alloc_open();    // 控制位查询
        /*
    // ── 查询 ──
    // 即时扫描生成空闲段（堆上瞬态，调用方 delete[]；FPA 收养即用即删）
    static free_segs_t* free_segs_get();*/
    /*统一解释为什么废弃了free_segs_t相关，因为FPA现在进化为可以从脏的初始化状态开始向上折叠，不需要洁癖
    由是FPA初始化应该拿到的是空闲dram段，或者说page_frame_state_mgr里面的intervals数组（扣掉scan_base_idx）
    */
};

// ── 页级分配包装（迁移期保留于此；后续可归 FPA / out_surfaces）──
extern "C" {
    void* __wrapped_pgs_valloc(KURD_t* kurd_out, uint64_t _4kbpgscount,
                               page_state_t TYPE, uint8_t alignment_log2);
    KURD_t __wrapped_pgs_vfree(void* vbase, uint64_t _4kbpgscount);
    vaddr_t stack_alloc(KURD_t* kurd_out, uint64_t _4kbpgscount);
}