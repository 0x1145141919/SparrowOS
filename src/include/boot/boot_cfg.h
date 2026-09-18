#pragma once
#include <stdint.h>

// ════════════════════════════════════════════════════════════════
// boot_cfg — 启动期 override 字典：wire 契约 + kernel 侧落地（唯一来源）
//
// 本头是 boot.cfg 的【单一事实来源】，被两个世界共用：
//   · initramfs builder（host）：把 initramfs_config.json 的 "boot_cfg" 对象
//     按此处注册表序列化成 /boot.cfg blob，打进 initramfs.img；
//   · kernel.elf（src/utils/boot_cfg.cpp）：解析 blob → g_boot_cfg。
//   ——键 id / 键名 / blob 布局 / magic 全部只在这一处定义，两侧不得各自复现。
// 只用 <stdint.h>，无 STL / 无平台依赖，故可被 host 与 freestanding 同时包含。
//
// 新增一个配置键 = 仅两处：
//   ① 本文件：enum 加号 + BOOT_CFG_KEY_TABLE 加 {name,id}（builder 自动生效）；
//   ② kernel：boot_cfg.cpp 的 apply_record 加 case（语义落地）。
// ════════════════════════════════════════════════════════════════

// ── 键 id（wire 层稳定标识：勿复用、勿改号；新增一律往后追加）──
enum : uint16_t {
    BOOT_CFG_KEY_LOG_UART  = 1,   // 0/1：init_printk 文本后端 UART(COM1) 开关
    BOOT_CFG_KEY_LOG_GOP   = 2,   // 0/1：init_printk 文本后端 GOP 控制台开关
    BOOT_CFG_KEY_YMIR_MODE = 3,   // 0=normal 1=test_mmu 2=test_wraith
    // 4：fpa.strategy —— 保留位（已设计、暂未接线）
};

// ── 键名注册表（JSON 里的字符串 ↔ id；builder 按此解析并校验，未知键即报错）──
struct boot_cfg_key_name_t {
    const char* name;
    uint16_t    id;
};
inline constexpr boot_cfg_key_name_t BOOT_CFG_KEY_TABLE[] = {
    { "log.uart",  BOOT_CFG_KEY_LOG_UART  },
    { "log.gop",   BOOT_CFG_KEY_LOG_GOP   },
    { "ymir.mode", BOOT_CFG_KEY_YMIR_MODE },
};
inline constexpr uint32_t BOOT_CFG_KEY_TABLE_COUNT =
    sizeof(BOOT_CFG_KEY_TABLE) / sizeof(BOOT_CFG_KEY_TABLE[0]);

// ── /boot.cfg blob 布局：[header][record × count] ──
// header 8B、record 16B → 全程 8 对齐，无需 pack/__attribute__。
constexpr uint32_t BOOT_CFG_BLOB_MAGIC = 0x67664342u;   // 'B''C''f''g'（LE）

struct boot_cfg_blob_header {
    uint32_t magic;   // BOOT_CFG_BLOB_MAGIC
    uint32_t count;   // 记录条数
};
struct boot_cfg_blob_record {
    uint16_t key;       // BOOT_CFG_KEY_*
    uint16_t reserved;  // 0（对齐占位）
    uint64_t value;     // 语义随 key
};

static_assert(sizeof(boot_cfg_blob_header) == 8, "boot_cfg_blob_header must be 8 bytes");
static_assert(sizeof(boot_cfg_blob_record) == 16, "boot_cfg_blob_record must be 16 bytes");

// ────────────────────────────────────────────────────────────────
// 以下为 kernel.elf 侧落地（host builder 包含本头时仅作声明，不参与链接）
// ────────────────────────────────────────────────────────────────

// kthread_ymir 世界选择
enum class ymir_mode_t : uint8_t {
    NORMAL      = 0,   // 业务初始化（默认）
    TEST_MMU    = 1,   // 预留：MMU 压测分支
    TEST_WRAITH = 2,   // 预留：WRAITH 验收分支
};

// 日志文本后端开关（相互独立；环 ring_log 恒开，不在此列）
// 语义 = 「编译期默认 ⊕ /boot.cfg 覆盖」：
//   文件缺省 / 损坏 / 未知键 / 越界值 → 保持编译期默认；绝不 boot_halt。
// 默认：UART 开 / GOP 关 —— GOP 滚屏极慢，且与 init.elf 侧 INIT_LOG_GOP=0 同向。
// 形态：POD + 常量初始化（零全局构造）；无堆、无异常、无锁。
struct boot_cfg_t {
    bool        log_uart;   // init_printk → UART(COM1)
    bool        log_gop;    // init_printk → GOP 文本控制台
    ymir_mode_t ymir;       // kthread_ymir 世界选择
    // uint8_t fpa_strategy; // 保留（未接线）
};

// 编译期默认 —— 单一来源；/boot.cfg 只做 override
inline constexpr boot_cfg_t BOOT_CFG_DEFAULT = {
    .log_uart = true,
    .log_gop  = false,
    .ymir     = ymir_mode_t::NORMAL,
};

// 活体：常量初始化（无动态构造）。boot_cfg_load() 先归默认再逐键覆盖。
extern boot_cfg_t g_boot_cfg;

// 载入一次（幂等）：g_asset_table["initramfs movable"] → 主窗口重链 →
// initramfs_lookup("/boot.cfg") → blob 校验 → 逐记录 apply。
// 调用点：exec_env_prepare 内、init_panic_early_support()（PhyAddrAccessor 就绪）
//         之后、init_output_subsystem() 之前。
// 返回实际生效键数；0 = 无文件 / 损坏（此时 g_boot_cfg == 默认）。
uint32_t boot_cfg_load();
