#pragma once
#include <stdint.h>
#include "abi/asset_route.h"
#include "abi/src_loc.h"
#include "init_inheritance_analyzed.h"
// ════════════════════════════════════════════════════════════════
// asset_table — kernel.elf 侧资产表（init.elf → kernel.elf handoff 清单）
//
// 定位：输出子系统（kout）的**前驱**——它在任何打印能力就绪之前就要被消费。
// 因此本模块刻意保持"死简单"：堆上扁平数组 + 有效性位图 + 逐条状态机。
// 无红黑树、无哈希、无动态扩容——出错面最小，任何逻辑都能一眼看穿。
//
// 生命周期（一用即弃，consume-and-dispose）：
//   exec_env_prepare:
//     kpoolmemmgr_t::Init()                    ① 第一堆
//     an = link_init_to_kernel_header(pkg)   ② 信息包链接（原地：偏移 → 指针视图，info_pkg_link）
//   basic_init:
//     g_asset_table = asset_table_t::create()   ③ 控制器（堆，静态工厂）
//     g_asset_table->pour(&an)                  ④ 从 analyzed 视图深拷贝 name+data 进堆，全 PENDING
//     [拷出一等字段 phymem_segments / bcb_table]⑤ 本模块外，basic_init 自己做
//     ksetmem_8(pkg, 0, ...)                    ⑥ 阅后即焚（pour 已完成，包内全部拷走）
//   各阶段消费:
//     read() → 自拷 → deal(full_name)          ⑦ 认领：read 直读 → 调用方自拷 → deal 标记销毁
//   全部 dealed（pending_count() == 0）:
//     dispose()                                ⑧ 释放 entries[] + 位图 + 控制器本体
//
// 关键语义：
//   - pour 对 name/data 做堆深拷贝（analyzed 指针 → 内核堆），焚包后仍有效
//   - deal 释放的是条目**元数据**（name 串 + desc 描述符拷贝），**不是物理资产**；
//     物理页/映射的归属由消费方自行管理（如 kimg 物理页自省完经 BCB 释放）
//   - 调用方在 deal 前必须自拷 read() 返回的 name/data——deal 后 entry 指针悬垂
//   - 位图 = 槽有效性（pour 全置 1，deal 清位）；state = PENDING→DEALED 单向成长
//   - dispose 仅在 pending_left == 0 时允许；否则 = 编程错误（漏认领）→ 调用方 halt
// ════════════════════════════════════════════════════════════════

enum asset_state : uint8_t {
    ASSET_STATE_PENDING = 0,   // 已 pour，待认领
    ASSET_STATE_DEALED  = 1,   // 已处理：name/data 已堆上释放，位图位已清
};

struct asset_table_entry {
    char*    name;         // 堆拷贝完整多arg 串；键 = full_name（任意两两不同）
    void*    data;         // 堆拷贝 desc blob（desc_size 字节）
    uint32_t kind;         // asset_kind（pour 时按 arg1 路由解析定死）
    uint16_t desc_size;    // data blob 字节数（拷出/释放用）
    uint8_t  state;        // ASSET_STATE_*
};

class asset_table_t {
public:
    // 创建控制器（堆上，全部字段零初始）。第一堆未就绪时返回 nullptr。
    // 构造函数私有——一律经静态工厂创建，杜绝栈上/全局裸构造。
    static asset_table_t* create();

    // pour：从已链接的 analyzed 视图（info_pkg_link 产出）深拷贝建表。
    //   pkg — analyzed 指针式视图；pkg->properties_table 每条 name/data 已是
    //         包内真实线性地址（链接已完成），此处只做堆深拷贝
    // 逐条：解析 arg1 路由 → kind/desc_size → 堆拷 name 串 + desc blob → PENDING + 位图置 1。
    // 失败（未知路由 / 空 data / desc 缺大小 / 堆 OOM / full_name 重复）清理半成品并返回 SRC_LOC()。
    // ⚠ 必须在焚包（ksetmem_8）之前调用。
    loc_code_t pour(const init_to_kernel_header_analyzed* pkg);

    // 全部 dealed（pending_left == 0）后才允许：释放 entries[] + valid_bits[] + 控制器本体。
    // pending_left != 0 返回 SRC_LOC()（漏认领，调用方 halt）。成功后调用方须把 g_asset_table 置 nullptr。
    loc_code_t dispose();

    // 偷看：按完整名（full_name）找条目，不消费（不改状态）。未命中返回 nullptr。
    const asset_table_entry* read(const char* full_name) const;

    // 认领并处理掉：按完整名（full_name）标记已处理，释放该条目 name/data。
    // 调用方必须在 deal 前自拷 read() 返回的 name/data——deal 后 entry 指针悬垂。
    // 未命中 / 已 DEALED → 返回 SRC_LOC()（不销毁任何东西）。
    loc_code_t deal(const char* full_name);

    uint64_t pending_count() const { return pending_left; }

private:
    asset_table_t() = default;   // 仅 create() 内部经堆分配 + placement new 调用

    asset_table_entry* entries;      // 堆上，count 个
    uint64_t*          valid_bits;   // 堆上，ceil(count/64) 个 u64；bit i = 槽 i 有效
    uint64_t           count;        // 条目总数
    uint64_t           pending_left; // 剩余 PENDING 数；归零 = 全部 dealed
};

// 全局单例（kernel.elf 侧）。basic_init 里 create() 显式构造——
// 全局裸指针零动态初始化，不违反"禁止全局 C++ 构造"纪律。
extern asset_table_t* g_asset_table;
