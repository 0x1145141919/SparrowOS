// ════════════════════════════════════════════════════════════════
// boot_cfg.cpp — 启动期 override 字典解析（kernel.elf 侧实现）
//
// 见 include/boot/boot_cfg.h 的语义说明。本文件独立承载【解析/落地】逻辑，
// 调用点（exec_env_prepare）只保留一行 boot_cfg_load()。
//
// 路径：g_asset_table["initramfs movable"] → 主窗口重链成 VA →
//       initramfs_lookup("/boot.cfg") → blob 校验 → 逐记录 apply。
// 全程无堆、无异常；任何失败只是「保持默认」，绝不 boot_halt。
// ════════════════════════════════════════════════════════════════
#include "boot/boot_cfg.h"

#include "abi/asset_names.h"                    // asset_names::initramfs
#include "boot/asset_table.h"                   // g_asset_table / asset_table_entry
#include "abi/boot.h"                           // movable_file_entry_t
#include "initramfs/fs_format.h"                // initramfs_header
#include "init/initramfs_lookup.h"              // initramfs_lookup
#include "memory/main_phyaddr_access_window.h"  // PHYACC_VA

boot_cfg_t g_boot_cfg = BOOT_CFG_DEFAULT;   // 常量初始化，无动态构造

namespace {

// 单键落地：未知键 → 返回 0（忽略）；越界值 → 丢弃但不报错（fail-safe）。
uint32_t apply_record(uint16_t key, uint64_t value)
{
    switch (key) {
        case BOOT_CFG_KEY_LOG_UART:
            g_boot_cfg.log_uart = (value != 0);
            return 1;
        case BOOT_CFG_KEY_LOG_GOP:
            g_boot_cfg.log_gop  = (value != 0);
            return 1;
        case BOOT_CFG_KEY_YMIR_MODE:
            if (value <= (uint64_t)ymir_mode_t::TEST_WRAITH)
                g_boot_cfg.ymir = (ymir_mode_t)value;
            return 1;
        default:
            return 0;   // 未知键：忽略（前向兼容）
    }
}

}  // namespace

uint32_t boot_cfg_load()
{
    g_boot_cfg = BOOT_CFG_DEFAULT;   // 幂等：先归默认

    if (!g_asset_table) return 0;

    const asset_table_entry* e = g_asset_table->read(asset_names::initramfs);
    if (!e) return 0;

    // initramfs = movable（纯物理描述符）→ 经主窗口重链成 VA。
    // 依赖 PhyAddrAccessor::Init 已完成（见调用点 exec_env_prepare 的注释）。
    const movable_file_entry_t mf = *(const movable_file_entry_t*)e->data;
    if (!mf.base_ppn || !mf.size) return 0;
    const initramfs_header* rh =
        (const initramfs_header*)PHYACC_VA(mf.base_ppn << 12);

    uint64_t fsz = 0;
    uint64_t fva = initramfs_lookup(rh, "/boot.cfg", &fsz);
    if (!fva || fsz < sizeof(boot_cfg_blob_header)) return 0;   // 无文件 → 默认

    const boot_cfg_blob_header* h = (const boot_cfg_blob_header*)fva;   // 已是 VA
    if (h->magic != BOOT_CFG_BLOB_MAGIC) return 0;                      // 魔数坏 → 默认

    const uint64_t need = sizeof(boot_cfg_blob_header)
                        + (uint64_t)h->count * sizeof(boot_cfg_blob_record);
    if (fsz < need) return 0;                                           // 长度坏 → 默认

    const boot_cfg_blob_record* r = (const boot_cfg_blob_record*)(h + 1);
    uint32_t applied = 0;
    for (uint32_t i = 0; i < h->count; ++i)
        applied += apply_record(r[i].key, r[i].value);
    return applied;
}
