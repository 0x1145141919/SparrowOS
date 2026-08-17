#pragma once
#include <abi/boot.h>
#include <abi/src_loc.h>

// ════════════════════════════════════════════════════════════════
// exec_env_prepare — 环境准备（三阶段 boot 的第一阶段）
//
// 特殊约束（为什么独立成文件）：
//   - 本函数运行于"无 kout / 无输出子系统"的摸黑阶段——一切失败只能裸停机
//     （boot_halt），绝不依赖 bsp_kout / GfxPrim 之后的任何打印能力
//   - 它是 kernel.elf 侧最早执行的业务函数：探针 → 第一堆 → 信息包链接 →
//     一等字段复制 → 资产表 pour → 三个必需资产初始化
//   - 约束只在此文件内生效，不污染 basic_init / truly_start
//
// 职责顺序：
//   ① g_env = probe_env()               探针（KVM/TCG/裸机）
//   ② kpoolmemmgr_t::Init()             第一堆
//   ③ link_init_to_kernel_header         信息包 偏移式 → 指针式 analyzed 视图
//   ④ 一等字段复制到堆                   phymem_segments / free_segs
//                                        （free_segs 索引式，adopt 内解析，无需中转）
//   ⑤ asset_table_t::create() + pour    资产表全量深拷贝
//   ⑥ 早期 panic 支撑                    "phyaddr_window mem" → Kspace_phyaddr_access_window
//                                         （二级重链钥匙，后续 PhyAddrAccessor 也依赖）
//                                        + "ksymbols movable" 应急经窗口重链 → ksymmanager
//                                        + "hpet_mmio mem" → HPET readonly_timer
//                                        （保证第一条可能崩溃即可调符号表 + 时间戳）
//   ⑦ 输出子系统链路 (init_output_subsystem)
//                                        "log_buffer mem" "gop_framebuffer mem"
//                                        "gop_info gop" → DmesgRingBuffer / GfxPrim
//                                        → textconsole_GoP::Init+Clear → serial_init_stage1
//                                        → bsp_kout.Init+shift_dec（至此 kout 可正常打印）
//   ⑧ page_frame_state_mgr 收养           adopt("pages_arr mem" 资产 +
//                                         free_segs_descriptors_table)，接管 mem_map
//                                         权威账本，供 early_alloc 早期分配；FPA 后续
//                                         基于 intervals_snapshot 自行分桶重建
//
// 调用方：_kernel_Init asm（kernel_entry.asm）在 basic_init 之前调用。
// ════════════════════════════════════════════════════════════════

// pkg — init.elf 传递的 v2 信息包基址（kernel 侧可访问线性地址）
extern "C" void exec_env_prepare(init_to_kernel_header_v2* pkg);
