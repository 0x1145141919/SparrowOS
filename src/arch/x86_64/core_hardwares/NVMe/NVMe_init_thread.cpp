#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/PCIe/base.h"
#include "arch/x86_64/PCIe/prased.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include <util/arch/x86-64/cpuid_intel.h>
#include "Scheduler/per_processor_scheduler.h"
#include "Scheduler/kthread_abi.h"
// ============================================================
// NVMe PCIe 类码
// ============================================================
constexpr uint8_t  PCI_BASE_CLASS_MASS_STORAGE = 0x01;
constexpr uint8_t  PCI_SUB_CLASS_NVM           = 0x08;

// ============================================================
// 线程参数
// ============================================================
struct nvme_init_thread_arg {
    uint32_t       node_index;
    NVMe_Controller* ctrl;
};

// ============================================================
// scan_pcie_nvme_count: 遍历 PCIe 枚举空间，统计 NVMe 控制器数量
// ============================================================
static uint32_t scan_pcie_nvme_count()
{
    if (!global_container) return 0;

    uint32_t count = 0;
    for (auto it = global_container->begin(); it != global_container->end(); ++it) {
        const ecam_node_t& seg = *it;

        for (uint16_t bus = seg.start_bus_num;
             bus < seg.start_bus_num + seg.bus_count; bus++)
        {
            for (uint8_t dev = 0; dev < 32; dev++) {
                for (uint8_t func = 0; func < 8; func++) {
                    uint64_t off = (uint64_t)(bus - seg.start_bus_num) * 32 * 8 * 0x1000
                                 + (uint64_t)dev * 8 * 0x1000
                                 + (uint64_t)func * 0x1000;
                    volatile auto* cfg = (volatile uint8_t*)(seg.vminterval.vbase() + off);

                    uint16_t vendor = *(volatile uint16_t*)(cfg + 0x00);
                    if (vendor == 0xFFFF) {
                        if (func == 0) break;  // 单功能设备，跳过其余 func
                        continue;
                    }

                    uint8_t base_cls = *(volatile uint8_t*)(cfg + 0x0B);
                    uint8_t sub_cls  = *(volatile uint8_t*)(cfg + 0x0A);
                    if (base_cls == PCI_BASE_CLASS_MASS_STORAGE &&
                        sub_cls == PCI_SUB_CLASS_NVM)
                    {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

// ============================================================
// nvme_init_thread_entry: 内核线程入口，调用单控制器的 device_init
// ============================================================
static KURD_t nvme_init_thread_entry(void* arg)
{
    auto* a = static_cast<nvme_init_thread_arg*>(arg);
    if (!a->ctrl) {
        return KURD_t(result_code::FAIL, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::Init,
            level_code::ERROR, err_domain::CORE_MODULE);
    }

    KURD_t r = NVMe_Controller::device_init(a->ctrl);
    if (error_kurd(r)) {
        bsp_kout << "[NVMe] node " << (uint32_t)a->node_index
                 << ": init failed, reason=0x";
        bsp_kout.shift_hex();
        bsp_kout << r.reason;
        bsp_kout.shift_dec();
        bsp_kout << kendl;
    } else {
        bsp_kout << "[NVMe] node " << (uint32_t)a->node_index
                 << ": init OK" << kendl;
    }
    return r;
}

// ============================================================
// NVMe_Controller::Init (static)
//
// 系统初始化入口：扫描 PCIe 总线上的所有 NVMe 控制器，创建内核线程并行初始化。
//
// 流程:
//   1. 扫描 PCIe，统计 NVMe 控制器数量 N
//   2. 分配 node_array[N]
//   3. 再次扫描，填充每个 node 的 BDF/ecam_va 信息
//   4. 设置 controllers_count = N
//   5. 为每个控制器 spawn 初始化线程
//   6. 等待所有线程完成
//   7. 输出汇总结果
// ============================================================
KURD_t NVMe_Controller::Init(uint64_t flags)
{
    (void)flags;

    // ---- 1. 第一遍扫描：统计数量 ----
    uint32_t nvme_count = scan_pcie_nvme_count();
    bsp_kout << "[NVMe] Found " << (uint32_t)nvme_count << " controller(s)" << kendl;

    if (nvme_count == 0) {
        node_array    = nullptr;
        controllers_count = 0;
        return KURD_t(
            result_code::SUCCESS, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::Init,
            level_code::INFO, err_domain::CORE_MODULE);
    }

    // ---- 2. 分配 node_array ----
    node_array = new node[nvme_count]();
    if (!node_array) {
        controllers_count = 0;
        return KURD_t(
            result_code::FAIL, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::Init,
            level_code::ERROR, err_domain::CORE_MODULE);
    }

    // ---- 3. 第二遍扫描：填充 node 信息 ----
    uint32_t index = 0;
    if (global_container) {
        for (auto it = global_container->begin(); it != global_container->end(); ++it) {
            const ecam_node_t& seg_ecam = *it;

            for (uint8_t bus = seg_ecam.start_bus_num;
                 bus < seg_ecam.start_bus_num + seg_ecam.bus_count && index < nvme_count; bus++)
            {
                for (uint8_t dev = 0; dev < 32 && index < nvme_count; dev++) {
                    for (uint8_t func = 0; func < 8 && index < nvme_count; func++) {
                        uint64_t off = (uint64_t)(bus - seg_ecam.start_bus_num) * 32 * 8 * 0x1000
                                     + (uint64_t)dev * 8 * 0x1000
                                     + (uint64_t)func * 0x1000;
                        volatile auto* cfg = (volatile uint8_t*)(seg_ecam.vminterval.vbase() + off);

                        uint16_t vendor = *(volatile uint16_t*)(cfg + 0x00);
                        if (vendor == 0xFFFF) {
                            if (func == 0) break;
                            continue;
                        }

                        uint8_t base_cls = *(volatile uint8_t*)(cfg + 0x0B);
                        uint8_t sub_cls  = *(volatile uint8_t*)(cfg + 0x0A);
                        if (base_cls == PCI_BASE_CLASS_MASS_STORAGE &&
                            sub_cls == PCI_SUB_CLASS_NVM)
                        {
                            vaddr_t ecam_va = seg_ecam.vminterval.vbase() + off;
                            node_array[index].pcie_seg  = seg_ecam.seg_group_number;
                            node_array[index].pcie_bus  = bus;
                            node_array[index].pcie_dev  = dev;
                            node_array[index].pcie_func = func;
                            node_array[index].ecam_va   = ecam_va;
                            node_array[index].controller = nullptr;
                            index++;
                        }
                    }
                }
            }
        }
    }

    controllers_count = nvme_count;

    // ---- 4. 创建 NVMe_Controller 实例 ----
    // 先全部创建完再 spawn 线程，避免 node_array 的不确定性
    for (uint32_t i = 0; i < nvme_count; i++) {
        node_array[i].controller = new NVMe_Controller(node_array[i].ecam_va);
    }

    // ---- 5. 并行初始化 ----
    nvme_init_thread_arg* thread_args   = new nvme_init_thread_arg[nvme_count]();
    uint64_t*             tids           = new uint64_t[nvme_count]();

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < nvme_count; i++) {
        thread_args[i].node_index  = i;
        thread_args[i].ctrl        = node_array[i].controller;

        KURD_t kurd;
        kthread_creating_package pkg;
        pkg.func_raw   = (uint64_t)nvme_init_thread_entry;
        pkg.args[0]    = (uint64_t)&thread_args[i];
        pkg.args[1]    = 0;
        pkg.args[2]    = 0;
        pkg.args[3]    = 0;
        pkg.args[4]    = 0;
        pkg.launch_pid = fast_get_processor_id();
        tids[i] = creat_kthread(&pkg, &kurd);
        if (tids[i] == INVALID_TID || error_kurd(kurd)) {
            bsp_kout << "[NVMe] Failed to spawn init thread for node "
                     << (uint32_t)i << kendl;
            // 未真正启动，计入失败
        } else {
            spawned++;
        }
    }

    bsp_kout << "[NVMe] Spawned " << (uint32_t)spawned << " init thread(s)" << kendl;

    // ---- 6. 等待所有线程完成并回收 ----
    bool* init_ok = new bool[nvme_count]();
    uint32_t ok_count = 0;
    uint32_t fail_count = 0;
    for (uint32_t i = 0; i < nvme_count; i++) {
        if (tids[i] == INVALID_TID) {
            fail_count++;
            continue;
        }

        zombie_observe_results_t z_result;
        uint64_t raw;
        do {
            raw = zombie_observe(tids[i], &z_result);
        } while (z_result == ZOMBIE_ALIVE);

        if (z_result == ZOMBIE_TID_NOT_FOUND) {
            fail_count++;
            continue;
        }

        KURD_t thread_kurd = raw_analyze(raw);
        if (!error_kurd(thread_kurd)) {
            init_ok[i] = true;
            ok_count++;
        } else {
            fail_count++;
        }

        ckurd rel_result = release_kthread(tids[i]);
        if (error_kurd(raw_analyze(rel_result))) {
            bsp_kout << "[NVMe] release_kthread(" << tids[i] << ") failed" << kendl;
        }
    }

    bsp_kout << "[NVMe] Init summary: " << (uint32_t)ok_count
             << " OK, " << (uint32_t)fail_count << " failed" << kendl;

    // ---- 8. 清理临时资源 ----
    delete[] thread_args;
    delete[] tids;
    delete[] init_ok;

    if (fail_count == nvme_count && ok_count == 0) {
        // 全部失败
        return KURD_t(
            result_code::FAIL, (uint16_t)fail_count,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::Init,
            level_code::ERROR, err_domain::CORE_MODULE);
    }

    return KURD_t(
        result_code::SUCCESS, 0,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::Init,
        level_code::INFO, err_domain::CORE_MODULE);
}
