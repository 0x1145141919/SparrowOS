#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/PCIe/base.h"
#include "arch/x86_64/PCIe/prased.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "Scheduler/kthread_abi.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
using namespace kio;

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

// ============================================================
// 辅助：BDF 字符串解析（格式 "B:D:F"）
// ============================================================
static bool parse_bdf(const char* s, size_t len,
                      uint16_t* seg, uint8_t* bus, uint8_t* dev, uint8_t* func)
{
    // 格式：seg:bus:dev:func 或 bus:dev:func（seg 默认为 0）
    uint16_t seg_val = 0;
    uint8_t vals[4];
    int count = 0;
    const char* p = s;
    const char* end = s + len;

    while (p < end && count < 4) {
        const char* start = p;
        while (p < end && *p != ':') p++;

        uint64_t v = 0;
        bool ok = false;
        if (p > start) {
            const char* np = start;
            if (p - start > 2 && np[0] == '0' && (np[1] == 'x' || np[1] == 'X')) {
                np += 2;
                while (np < p) {
                    char c = *np++;
                    uint8_t d;
                    if (c >= '0' && c <= '9')       d = c - '0';
                    else if (c >= 'a' && c <= 'f')  d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')  d = c - 'A' + 10;
                    else return false;
                    v = (v << 4) | d;
                }
            } else {
                while (np < p) {
                    if (*np < '0' || *np > '9') return false;
                    v = v * 10 + (*np - '0');
                    np++;
                }
            }
            ok = true;
        }
        if (!ok) return false;

        if (count == 0 && (p - start) > 2) {
            // 四段格式：seg:bus:dev:func
            seg_val = (uint16_t)v;
        } else {
            vals[count] = (uint8_t)v;
        }
        count++;

        if (p < end) p++;  // 跳过 ':'
    }

    if (count == 3) {
        *seg = 0;
        *bus = vals[0];
        *dev = vals[1];
        *func = vals[2];
        return true;
    } else if (count == 4) {
        *seg = seg_val;
        *bus = vals[0];
        *dev = vals[1];
        *func = vals[2];
        return true;
    }
    return false;
}

// ============================================================
// 辅助：通过 BDF 找 ECAM 节点和 PCI 配置空间
// ============================================================
static volatile void* find_ecam_by_bdf(uint16_t seg, uint8_t bus,
                                        uint8_t dev, uint8_t func)
{
    if (!global_container) return nullptr;
    for (auto it = global_container->begin(); it != global_container->end(); ++it) {
        const ecam_node_t& node = *it;
        if (node.seg_group_number != seg) continue;
        uint8_t end_bus = node.start_bus_num + node.bus_count - 1;
        if (bus < node.start_bus_num || bus > end_bus) continue;

        uint64_t off = (bus - node.start_bus_num) * 32 * 8 * 0x1000
                     + dev  * 8 * 0x1000
                     + func * 0x1000;
        return (volatile void*)(node.vminterval.vbase() + off);
    }
    return nullptr;
}

// ============================================================
// NVMe_on 命令
// ============================================================
static KURD_t cmd_nvme_on(const line_t* line)
{
    if (line->token_count < 2)
        goto usage;

    {
        uint16_t seg; uint8_t bus, dev, func;
        if (!parse_bdf(line->tokens[1].str, line->tokens[1].len, &seg, &bus, &dev, &func))
            goto usage;

        // 检查 class code（Mass Storage: 01h, NVM Subclass: 08h）
        volatile void* cfg = find_ecam_by_bdf(seg, bus, dev, func);
        if (!cfg) {
            bsp_kout << "[NVMe] No PCIe config space for given BDF" << kendl;
            return make_ok();
        }

        uint16_t vendor = *(volatile uint16_t*)((uintptr_t)cfg + 0x00);
        uint16_t device = *(volatile uint16_t*)((uintptr_t)cfg + 0x02);
        uint8_t  base_cls = *(volatile uint8_t*)((uintptr_t)cfg + 0x0B);
        uint8_t  sub_cls  = *(volatile uint8_t*)((uintptr_t)cfg + 0x0A);

        if (base_cls != 0x01 || sub_cls != 0x08) {
            bsp_kout << "[NVMe] Not an NVMe device (class=";
            bsp_kout.shift_hex();
            bsp_kout << (uint32_t)base_cls << ":" << (uint32_t)sub_cls;
            bsp_kout.shift_dec();
            bsp_kout << ")" << kendl;
            return make_ok();
        }

        // 检查是否已初始化
        for (uint32_t i = 0; i < NVMe_Controller::controllers_count; i++) {
            auto& n = NVMe_Controller::node_array[i];
            if (n.pcie_bus == bus && n.pcie_dev == dev && n.pcie_func == func) {
                // 创建新控制器
                vaddr_t ecam_va = (vaddr_t)cfg;
                NVMe_Controller* ctrl = new NVMe_Controller(ecam_va);
                NVMe_Controller::node_array[i].controller = ctrl;
                NVMe_Controller::controllers_count++;

        bsp_kout << "[NVMe] Initializing device " << kendl;
        bsp_kout.shift_hex();
        bsp_kout << vendor << ":" << device;
        bsp_kout.shift_dec();
        bsp_kout << " at " << (uint32_t)bus << ":" << (uint32_t)dev << ":" << (uint32_t)func << kendl;

        // 启动初始化线程
        kthread_creating_package pkg;
        pkg.func_raw = (uint64_t) +[](void* arg) -> void* {
            auto* dev = (NVMe_Controller*)arg;
            KURD_t r = NVMe_Controller::device_init(dev);
            if (error_kurd(r)) {
                bsp_kout << "[NVMe] init failed: reason=0x";
                bsp_kout.shift_hex();
                bsp_kout << r.reason;
                bsp_kout.shift_dec();
                bsp_kout << kendl;
            }
            return nullptr;
        };
        pkg.args[0]    = (uint64_t)ctrl;
        pkg.args[1]    = 0;
        pkg.args[2]    = 0;
        pkg.args[3]    = 0;
        pkg.args[4]    = 0;
        pkg.launch_pid = fast_get_processor_id();

        KURD_t kurd;
        uint64_t tid = creat_kthread(&pkg, &kurd);
        if (tid == INVALID_TID || error_kurd(kurd)) {
            bsp_kout << "[NVMe] Failed to spawn init thread" << kendl;
            NVMe_Controller::controllers_count--;
        } else {
            kthread_sleep(5000000);
            zombie_observe_results_t zr;
            zombie_observe(tid, &zr);
            if (zr == ZOMBIE_DEAD) {
                release_kthread(tid);
            }
        }
    }
    
            }
        }
return make_ok();
        

usage:
    bsp_kout << "Usage: NVMe_on <B:D:F>" << kendl;
    return make_ok();
}

// ============================================================
// NVMe_off 命令
// ============================================================
static KURD_t cmd_nvme_off(const line_t* line)
{
    if (line->token_count < 2)
        goto usage;

    {
        uint16_t seg; uint8_t bus, dev, func;
        if (!parse_bdf(line->tokens[1].str, line->tokens[1].len, &seg, &bus, &dev, &func))
            goto usage;

        // 查找控制器
        NVMe_Controller* ctrl = nullptr;
        uint32_t idx;
        for (idx = 0; idx < NVMe_Controller::controllers_count; idx++) {
            auto& n = NVMe_Controller::node_array[idx];
            if (n.pcie_bus == bus && n.pcie_dev == dev && n.pcie_func == func) {
                ctrl = n.controller;
                break;
            }
        }

        if (!ctrl) {
            bsp_kout << "[NVMe] No active controller at given BDF" << kendl;
            return make_ok();
        }

        bsp_kout << "[NVMe] Shutting down device " << kendl;
        bsp_kout.shift_hex();
        bsp_kout << (uint32_t)bus << ":" << (uint32_t)dev << ":" << (uint32_t)func;
        bsp_kout.shift_dec();
        bsp_kout << kendl;

        kthread_creating_package pkg;
        pkg.func_raw = (uint64_t) +[](void* arg) -> void* {
            auto* dev = (NVMe_Controller*)arg;
            KURD_t r = dev->offline(0);
            if (error_kurd(r)) {
                bsp_kout << "[NVMe] offline failed: reason=0x";
                bsp_kout.shift_hex();
                bsp_kout << r.reason;
                bsp_kout.shift_dec();
                bsp_kout << kendl;
            } else {
                bsp_kout << "[NVMe] offline complete" << kendl;
            }
            return nullptr;
        };
        pkg.args[0]    = (uint64_t)ctrl;
        pkg.args[1]    = 0;
        pkg.args[2]    = 0;
        pkg.args[3]    = 0;
        pkg.args[4]    = 0;
        pkg.launch_pid = fast_get_processor_id();

        KURD_t kurd;
        uint64_t tid = creat_kthread(&pkg, &kurd);
        if (tid == INVALID_TID || error_kurd(kurd)) {
            bsp_kout << "[NVMe] Failed to spawn offline thread" << kendl;
        } else {
            kthread_sleep(5000000);
            zombie_observe_results_t zr;
            zombie_observe(tid, &zr);
            if (zr == ZOMBIE_DEAD) {
                release_kthread(tid);
            }
        }
    }
    return make_ok();

usage:
    bsp_kout << "Usage: NVMe_off <B:D:F>" << kendl;
    return make_ok();
}

// ============================================================
// NVMe_iotest 命令 — 锁妖塔
//
// 用法: NVMe_iotest <B:D:F>
//
// 通过 BDF 定位一个已初始化的 NVMe 控制器，然后校验其身份是否
// 与实验盘（15b7:5017, SN=25100Y402391）完全一致。
// 校验通过才执行 I/O 测试；任何不匹配一律拒绝。
// ============================================================
static KURD_t cmd_nvme_iotest(const line_t* line)
{
    if (line->token_count < 2)
        goto usage;

    uint16_t seg; uint8_t bus, dev, func;
    if (!parse_bdf(line->tokens[1].str, line->tokens[1].len, &seg, &bus, &dev, &func))
        goto usage;

    {
    // ── 通过 BDF 找已初始化的控制器 ────────────────────────────
    NVMe_Controller* ctrl = nullptr;
    for (uint32_t i = 0; i < NVMe_Controller::controllers_count; i++) {
        auto& n = NVMe_Controller::node_array[i];
        if (n.pcie_bus == bus && n.pcie_dev == dev &&
            n.pcie_func == func && n.pcie_seg == seg && n.controller)
        {
            ctrl = n.controller;
            break;
        }
    }
    if (!ctrl) {
        bsp_kout << "[NVMe_iotest] No initialized controller at given BDF" << kendl;
        return make_ok();
    }

    // ── 金身：实验盘绝对身份 ────────────────────────────────
    NVMe_Controller::Identity golden;
    golden.vid    = 0x15b7;
    golden.did    = 0x5017;
    golden.ssvid  = 0x15b7;
    golden.cntlid = 0;
    const char sn_pad[] = "25100Y402391        ";   // 20 bytes
    const char mn_pad[] = "WD Blue SN5000 2TB                      ";   // 40 bytes
    __builtin_memcpy(golden.serial, sn_pad, 20);
    __builtin_memcpy(golden.model,  mn_pad, 40);

    // ── 身份校验 ────────────────────────────────────────────
    if (!ctrl->identity_verify(&golden)) {
        bsp_kout << "[NVMe_iotest] LOCKED — 盘身份不匹配，安全拒绝" << kendl;
        return make_ok();
    }

    bsp_kout << "[NVMe_iotest] ⛩️  实验盘已确认 (15b7:5017, SN=25100Y402391)" << kendl;

    // ── 执行 I/O 测试 ──────────────────────────────────────
    if (ctrl->get_ns_count() == 0 || !ctrl->get_namespaces()) {
        bsp_kout << "[NVMe_iotest] No namespace available" << kendl;
        return make_ok();
    }

    BlockDevice* ns = &ctrl->get_namespaces()[0];
    uint32_t ss = ns->sector_size;
    if (ss == 0) ss = 512;

    bsp_kout << "[NVMe_iotest] sector_size=" << (uint32_t)ss << kendl;

    KURD_t kurd;
    void* va = __wrapped_pgs_valloc(&kurd, 2, page_state_t::kernel_pinned, 12);
    if (!va || error_kurd(kurd)) {
        bsp_kout << "[NVMe_iotest] Buffer alloc failed" << kendl;
        return make_ok();
    }
    ksetmem_8(va, 0, 8192);

    phyaddr_t pa = 0;
    kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)va, pa);
    if (error_kurd(kurd) || pa == 0) {
        bsp_kout << "[NVMe_iotest] PA translation failed" << kendl;
        __wrapped_pgs_vfree(va, 2);
        return make_ok();
    }

    pbuf_t buf      = { .pbase = pa, .size = 8192 };
    pbuf_t buf2     = { .pbase = pa + ss, .size = 8192 - ss };
    LBA_interval_t interval = { .start = 0, .LBA_count = 1 };

    // TEST 1: Read LBA 0
    bsp_kout << "[NVMe_iotest] --- Test 1: Read LBA 0 ---" << kendl;
    ksetmem_8(va, 0, ss);
    KURD_t r1 = NVMe_Controller::read(ns, buf, interval, 0);
    if (!error_kurd(r1)) {
        bsp_kout << "[NVMe_iotest] Read OK, first 32 bytes:" << kendl;
        auto* d = (uint8_t*)va;
        for (uint32_t i = 0; i < 32; i++) {
            bsp_kout << HEX << (uint32_t)d[i] << ' ';
        }
        bsp_kout << kendl;
    } else {
        bsp_kout << "[NVMe_iotest] Read FAILED, reason=0x" << HEX << r1.reason << kendl;
    }

    // TEST 2: Write 0xA5 → read back → verify
    bsp_kout << "[NVMe_iotest] --- Test 2: Write 0xA5 pattern ---" << kendl;
    ksetmem_8(va, 0xA5, ss);
    KURD_t r2w = NVMe_Controller::write(ns, buf, interval, 0);
    if (error_kurd(r2w)) {
        bsp_kout << "[NVMe_iotest] Write FAILED, reason=0x" << HEX << r2w.reason << kendl;
    }

    ksetmem_8(va, 0, ss);
    KURD_t r2r = NVMe_Controller::read(ns, buf, interval, 0);
    if (!error_kurd(r2r)) {
        bool match = true;
        for (uint32_t i = 0; i < ss; i++) {
            if (((uint8_t*)va)[i] != 0xA5) { match = false; break; }
        }
        bsp_kout << "[NVMe_iotest] Readback verify: "
                 << (match ? "PASS" : "FAIL") << kendl;
    } else {
        bsp_kout << "[NVMe_iotest] Readback FAILED, reason=0x" << HEX << r2r.reason << kendl;
    }

    // TEST 3: Compare with self → expect SUCCESS
    bsp_kout << "[NVMe_iotest] --- Test 3: Compare (self, expect match) ---" << kendl;
    KURD_t r3 = NVMe_Controller::compare(ns, buf, interval, 0);
    bsp_kout << "[NVMe_iotest] Compare: "
             << (!error_kurd(r3) ? "PASS" : "FAIL") << kendl;

    // TEST 4: Write different → Compare → expect FAILURE
    bsp_kout << "[NVMe_iotest] --- Test 4: Compare (different, expect FAIL) ---" << kendl;
    ksetmem_8((uint8_t*)va + ss, 0x5A, ss);
    KURD_t r4 = NVMe_Controller::compare(ns, buf2, interval, 0);
    bsp_kout << "[NVMe_iotest] Compare (mismatch): "
             << (error_kurd(r4) ? "expected FAIL" : "UNEXPECTED PASS") << kendl;

    // Cleanup
    ksetmem_8(va, 0, ss);
    NVMe_Controller::write(ns, buf, interval, 0);
    __wrapped_pgs_vfree(va, 2);

    bsp_kout << "[NVMe_iotest] ⛩️  测试完成" << kendl;
    }
    return make_ok();

usage:
    bsp_kout << "Usage: NVMe_iotest <B:D:F>" << kendl;
    return make_ok();
}

// ============================================================
// 靶盘数据序列校验
// ============================================================
static bool verify_sequence(void* buf, uint64_t start_lba, uint32_t sector_size, uint32_t count)
{
    uint32_t* p = (uint32_t*)buf;
    uint32_t u32_per = sector_size / sizeof(uint32_t);
    uint32_t base = (uint32_t)(start_lba * u32_per);
    uint32_t total = count * u32_per;
    for (uint32_t i = 0; i < total; i++) {
        if (p[i] != base + i)
            return false;
    }
    return true;
}

// ============================================================
// NVMe_read_stress 命令 — PRP 压力测试
//
// 用法: NVMe_read_stress <B:D:F>
//
// 先验明金身（同 iotest），然后以多种扇区数和缓冲区大小组合
// 执行 read 命令，校验回读数据是否符合靶盘序列公式。
// 只读不写，安全可重复。覆盖 PRP1 only / PRP1+PRP2 / PRP List
// 单页 / PRP List 多页（daisy-chain）四种路径。
// ============================================================
struct read_stress_case {
    const char* name;
    uint64_t   start_lba;
    uint32_t   lba_count;
    uint32_t   buf_pages;
};

static const read_stress_case stress_cases[] = {
    { "prp1_only",          0,  1,   1   },
    { "prp1_prp2",          0,  2,   2   },
    { "prp_list_start",     0,  3,   3   },
    { "mid_range",          0,  16,  16  },
    { "boundary_66",        0,  66,  66  },
    { "boundary_67",        0,  67,  67  },
    { "daisy_68",           0,  68,  68  },
    { "daisy_128",          0,  128, 128 },
    { "oversize_1s_2pg",    0,  1,   2   },
    { "oversize_2s_4pg",    0,  2,   4   },
};

static KURD_t cmd_nvme_read_stress(const line_t* line)
{
    if (line->token_count < 2)
        goto usage;

    uint16_t seg; uint8_t bus, dev, func;
    if (!parse_bdf(line->tokens[1].str, line->tokens[1].len, &seg, &bus, &dev, &func))
        goto usage;

    {
    // ── 通过 BDF 找控制器 ────────────────────────────────
    NVMe_Controller* ctrl = nullptr;
    for (uint32_t i = 0; i < NVMe_Controller::controllers_count; i++) {
        auto& n = NVMe_Controller::node_array[i];
        if (n.pcie_bus == bus && n.pcie_dev == dev &&
            n.pcie_func == func && n.pcie_seg == seg && n.controller)
        {
            ctrl = n.controller;
            break;
        }
    }
    if (!ctrl) {
        bsp_kout << "[NVMe_read_stress] No initialized controller at given BDF" << kendl;
        return make_ok();
    }

    // ── 金身：实验盘绝对身份 ──────────────────────────────
    NVMe_Controller::Identity golden;
    golden.vid    = 0x15b7;
    golden.did    = 0x5017;
    golden.ssvid  = 0x15b7;
    golden.cntlid = 0;
    const char sn_pad[] = "25100Y402391        ";
    const char mn_pad[] = "WD Blue SN5000 2TB                      ";
    __builtin_memcpy(golden.serial, sn_pad, 20);
    __builtin_memcpy(golden.model,  mn_pad, 40);

    if (!ctrl->identity_verify(&golden)) {
        bsp_kout << "[NVMe_read_stress] LOCKED — 盘身份不匹配，安全拒绝" << kendl;
        return make_ok();
    }

    bsp_kout << "[NVMe_read_stress] ⛩️  实验盘已确认 (15b7:5017, SN=25100Y402391)" << kendl;

    // ── 取 Namespace ─────────────────────────────────────
    if (ctrl->get_ns_count() == 0 || !ctrl->get_namespaces()) {
        bsp_kout << "[NVMe_read_stress] No namespace available" << kendl;
        return make_ok();
    }

    BlockDevice* ns = &ctrl->get_namespaces()[0];
    uint32_t ss = ns->sector_size;
    if (ss == 0) ss = 4096;
    uint64_t max_sectors = ns->sector_count;

    bsp_kout << "[NVMe_read_stress] sector_size=" << ss
             << " max_sectors=0x" << HEX << max_sectors << kendl;

    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t skipped = 0;
    constexpr uint32_t case_count = sizeof(stress_cases) / sizeof(stress_cases[0]);

    for (uint32_t ci = 0; ci < case_count; ci++) {
        const auto& c = stress_cases[ci];

        // 检查 LBA 范围
        if (c.start_lba + c.lba_count > max_sectors) {
            bsp_kout << "[NVMe_read_stress] --- " << c.name
                     << " — SKIP (LBA out of range)" << kendl;
            skipped++;
            continue;
        }

        // 分配缓冲区
        KURD_t kurd;
        void* va = __wrapped_pgs_valloc(&kurd, c.buf_pages,
                                         page_state_t::kernel_pinned, 12);
        if (!va || error_kurd(kurd)) {
            bsp_kout << "[NVMe_read_stress] --- " << c.name
                     << " — SKIP (alloc fail)" << kendl;
            skipped++;
            continue;
        }

        // 获取物理地址
        phyaddr_t pa = 0;
        kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)va, pa);
        if (error_kurd(kurd) || pa == 0) {
            bsp_kout << "[NVMe_read_stress] --- " << c.name
                     << " — SKIP (PA translation fail)" << kendl;
            __wrapped_pgs_vfree(va, c.buf_pages);
            skipped++;
            continue;
        }

        uint64_t data_size = (uint64_t)c.lba_count * ss;
        uint64_t buf_size  = (uint64_t)c.buf_pages * 4096;
        if (buf_size < data_size) {
            bsp_kout << "[NVMe_read_stress] --- " << c.name
                     << " — SKIP (buf size mismatch)" << kendl;
            __wrapped_pgs_vfree(va, c.buf_pages);
            skipped++;
            continue;
        }

        pbuf_t pbuf        { .pbase = pa, .size = buf_size };
        LBA_interval_t interval { .start = c.start_lba, .LBA_count = c.lba_count };

        // 读取
        ksetmem_8(va, 0, buf_size);
        KURD_t r = NVMe_Controller::read(ns, pbuf, interval, 0);

        if (error_kurd(r)) {
            bsp_kout << "[NVMe_read_stress] --- " << c.name << " ("
                     << c.lba_count << " sect, " << c.buf_pages << "pg buf)"
                     << " — READ FAIL, reason=0x" << HEX << r.reason << kendl;
            failed++;
        } else {
            bool ok = verify_sequence(va, c.start_lba, ss, c.lba_count);
            bsp_kout << "[NVMe_read_stress] --- " << c.name << " ("
                     << c.lba_count << " sect, " << c.buf_pages << "pg buf)"
                     << (ok ? " — PASS" : " — FAIL (data mismatch)") << kendl;
            if (ok) passed++;
            else    failed++;
        }

        __wrapped_pgs_vfree(va, c.buf_pages);
    }

    bsp_kout << "[NVMe_read_stress] === " << passed << "/" << (passed + failed)
             << " PASSED (failed=" << failed << " skipped=" << skipped << ")" << kendl;
    }
    return make_ok();

usage:
    bsp_kout << "Usage: NVMe_read_stress <B:D:F>" << kendl;
    return make_ok();
}

// ============================================================
// 命令表
// ============================================================
static command_entry_t nvme_commands[] = {
    {
        .name        = "NVMe_on",
        .description = "Initialize NVMe controller at BDF (e.g. 1:0:0)",
        .handler     = cmd_nvme_on,
        .risk        = command_risk_level_t::WARNING,
        .need_confirm= false,
    },
    {
        .name        = "NVMe_off",
        .description = "Shutdown NVMe controller at BDF",
        .handler     = cmd_nvme_off,
        .risk        = command_risk_level_t::WARNING,
        .need_confirm= false,
    },
    {
        .name        = "NVMe_iotest",
        .description = "I/O test at B:D:F, locked to WD Blue SN5000 (15b7:5017)",
        .handler     = cmd_nvme_iotest,
        .risk        = command_risk_level_t::DANGEROUS,
        .need_confirm= true,
    },
    {
        .name        = "NVMe_read_stress",
        .description = "PRP stress test (read-only) at B:D:F, locked to WD Blue SN5000",
        .handler     = cmd_nvme_read_stress,
        .risk        = command_risk_level_t::DANGEROUS,
        .need_confirm= true,
    },
};

static constexpr size_t nvme_cmd_count =
    sizeof(nvme_commands) / sizeof(nvme_commands[0]);

// ============================================================
// 注册入口
// ============================================================
void register_nvme_kshell_commands()
{
    for (size_t i = 0; i < nvme_cmd_count; i++) {
        KURD_t r = kshell_framework_t::command_register(&nvme_commands[i]);
        if (error_kurd(r)) {
            bsp_kout << "[NVMe] Failed to register command: "
                     << nvme_commands[i].name << kendl;
        }
    }
}
