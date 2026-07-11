#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/PCIe/base.h"
#include "arch/x86_64/PCIe/prased.h"
#include "util/arch/x86-64/cpuid_intel.h"
#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "Scheduler/kthread_abi.h"
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
            } else {
                bsp_kout << "[NVMe] init complete" << kendl;
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
