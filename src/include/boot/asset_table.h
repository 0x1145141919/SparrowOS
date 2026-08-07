#pragma once
#include <stdint.h>
#include "abi/asset_route.h"
#include "abi/src_loc.h"

// ════════════════════════════════════════════════════════════════
// asset_table — kernel.elf 侧资产表（init.elf → kernel.elf handoff 清单）
//
// 定位：输出子系统（kout）的**前驱**——它在任何打印能力就绪之前就要被消费。
// 因此本模块刻意保持"死简单"：堆上扁平数组 + 有效性位图 + 逐条状态机。
// 无红黑树、无哈希、无动态扩容——出错面最小，任何逻辑都能一眼看穿。
//
// 生命周期（一用即弃，consume-and-dispose）：
//   basic_init:
//     kpoolmemmgr_t::Init()                    ① 第一堆
//     g_asset_table = asset_table_create()      ② 控制器（堆）
//     asset_table_pour(pkt_base, props, count)  ③ 从 v2 包深拷贝 name+data 进堆，全 PENDING
//     [拷出一等字段 phymem_segments / bcb_table]④ 本模块外，basic_init 自己做
//     ksetmem_8(transfer, 0, ...)               ⑤ 阅后即焚（pour 已完成，包内全部拷走）
//   各阶段消费:
//     asset_table_read / asset_table_deal       ⑥ 认领：拷出 data → deal 释放条目元数据
//   全部 dealed（pending_left == 0）:
//     asset_table_dispose()                     ⑦ 释放 entries[] + 位图 + 控制器本体
//
// 关键语义：
//   - pour 对 name/data 做堆深拷贝（包内偏移 → 内核堆），焚包后仍有效
//   - deal 释放的是条目**元数据**（name 串 + desc 描述符拷贝），**不是物理资产**；
//     物理页/映射的归属由消费方自行管理（如 kimg 物理页自省完经 BCB 释放）
//   - 位图 = 槽有效性（pour 全置 1，deal 清位）；state = PENDING→DEALED 单向成长
//   - dispose 仅在 pending_left == 0 时允许；否则 = 编程错误（漏认领）→ 调用方 halt
// ════════════════════════════════════════════════════════════════

enum asset_state : uint8_t {
    ASSET_STATE_PENDING = 0,   // 已 pour，待认领
    ASSET_STATE_DEALED  = 1,   // 已处理：name/data 已堆上释放，位图位已清
};

struct asset_table_entry {
    char*    name;         // 堆拷贝完整多arg 串；键 = arg0（首个空格前）
    void*    data;         // 堆拷贝 desc blob（desc_size 字节）
    uint32_t kind;         // asset_kind（pour 时按 arg1 路由解析定死）
    uint16_t desc_size;    // data blob 字节数（拷出/释放用）
    uint8_t  state;        // ASSET_STATE_*
};

struct asset_table_t {
    asset_table_entry* entries;     // 堆上，count 个
    uint64_t*          valid_bits;  // 堆上，ceil(count/64) 个 u64；bit i = 槽 i 有效
    uint64_t           count;       // 条目总数
    uint64_t           pending_left; // 剩余 PENDING 数；归零 = 全部 dealed
};

// 全局单例（kernel.elf 侧）。basic_init 里 asset_table_create() 显式构造——
// 全局裸指针零动态初始化，不违反"禁止全局 C++ 构造"纪律。
extern asset_table_t* g_asset_table;

// ── 生命周期 ────────────────────────────────────────────────────

// 创建控制器（堆上，全部字段零初始）。第一堆未就绪时返回 nullptr。
asset_table_t* asset_table_create();

// pour：从 v2 信息包 properties_table 深拷贝建表。
//   pkt_base    — 信息包物理基址（重链 name/data 偏移用）
//   props       — 包内 asset_entry_t[]（name/data 为包基址相对偏移）
//   props_count — 条目数
// 逐条：解析 arg1 路由 → kind/desc_size → 堆拷 name 串 + desc blob → PENDING + 位图置 1。
// 失败（未知路由 / 空 data / desc 缺大小 / 堆 OOM / arg0 重复）清理半成品并返回 SRC_LOC()。
// ⚠ 必须在焚包（ksetmem_8）之前调用。
loc_code_t asset_table_pour(asset_table_t* t, const uint8_t* pkt_base,
                            const asset_entry_t* props, uint64_t props_count);

// 全部 dealed（pending_left == 0）后才允许：释放 entries[] + valid_bits[] + 控制器本体。
// pending_left != 0 返回 SRC_LOC()（漏认领，调用方 halt）。成功后调用方须把 g_asset_table 置 nullptr。
loc_code_t asset_table_dispose(asset_table_t* t);

// ── 消费 ────────────────────────────────────────────────────────

// 偷看：按 arg0 找条目，不消费（不改状态）。未命中返回 nullptr。
const asset_table_entry* asset_table_read(const asset_table_t* t, const char* arg0);

// 认领并处理掉：
//   校验 state==PENDING 且位图位有效 → 若 dst 容量足够则拷 desc_size 字节到 dst
//   → 释放条目 name/data → 置 DEALED → 清位图位 → pending_left--。
//   dst 可为 nullptr = "校验即弃"（只看不取，如 kernel_* mem 的 persist 校验）。
//   dst_cap 不足 / 未命中 / 已 DEALED → 返回 SRC_LOC()（不销毁任何东西）。
loc_code_t asset_table_deal(asset_table_t* t, const char* arg0,
                            void* dst, uint64_t dst_cap);

// ── 查询 ────────────────────────────────────────────────────────

uint64_t asset_table_pending_count(const asset_table_t* t);

// ── typed 认领便利（inline，先按 kind 校验再走 asset_table_deal）──
// 单线程 boot 阶段 read+deal 无竞态；kind 不符即拒绝，防裸 void* 误 cast。

inline loc_code_t asset_deal_iv(asset_table_t* t, const char* arg0, vm_interval* out)
{
    if (!out) return SRC_LOC();
    const asset_table_entry* e = asset_table_read(t, arg0);
    if (!e || e->kind != ASSET_KIND_MEM_INTERVAL) return SRC_LOC();
    return asset_table_deal(t, arg0, out, sizeof(vm_interval));
}

inline loc_code_t asset_deal_movable(asset_table_t* t, const char* arg0, movable_file_entry_t* out)
{
    if (!out) return SRC_LOC();
    const asset_table_entry* e = asset_table_read(t, arg0);
    if (!e || e->kind != ASSET_KIND_MOVABLE_FILE) return SRC_LOC();
    return asset_table_deal(t, arg0, out, sizeof(movable_file_entry_t));
}

inline loc_code_t asset_deal_scalar(asset_table_t* t, const char* arg0, uint64_t* out)
{
    if (!out) return SRC_LOC();
    const asset_table_entry* e = asset_table_read(t, arg0);
    if (!e || e->kind != ASSET_KIND_SCALAR) return SRC_LOC();
    return asset_table_deal(t, arg0, out, sizeof(uint64_t));
}

inline loc_code_t asset_deal_blob(asset_table_t* t, const char* arg0, void* dst, uint64_t dst_cap)
{
    const asset_table_entry* e = asset_table_read(t, arg0);
    if (!e || e->kind != ASSET_KIND_PHYMEM_BLOB) return SRC_LOC();
    return asset_table_deal(t, arg0, dst, dst_cap);
}
