#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/PCIe/base.h"
#include "arch/x86_64/PCIe/prased.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include <util/arch/x86-64/cpuid_intel.h>
#include "Scheduler/per_processor_scheduler.h"
#include "Scheduler/kthread_abi.h"
#include "ktime.h"
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
    volatile uint64_t* report;   // 共享 u64 汇报画板槽位（nullptr = 不汇报，兼容 Init 旧路径）
};

// ============================================================
// 汇报画板语义：0 = 进行中，1 = 成功，2 = 失败
// ============================================================
constexpr uint64_t NVME_BOARD_IN_FLIGHT = 0;
constexpr uint64_t NVME_BOARD_OK        = 1;
constexpr uint64_t NVME_BOARD_FAIL      = 2;

// 并行初始化/关机的轮询参数：总上限 5s，每轮遍历后睡眠 50us
// （故意不用 bq_system —— 理论上占用率更优，但更复杂且不一定延迟友好）
constexpr uint64_t NVME_PARALLEL_TIMEOUT_US = 5 * 1000 * 1000;  // 5s
constexpr uint64_t NVME_PARALLEL_POLL_US    = 50;               // 50us

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
// 结果写入共享 u64 汇报画板（report != nullptr 时）
// ============================================================
static KURD_t nvme_init_thread_entry(void* arg)
{
    auto* a = static_cast<nvme_init_thread_arg*>(arg);
    if (!a->ctrl) {
        if (a->report) {
            __sync_synchronize();
            *a->report = NVME_BOARD_FAIL;
        }
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

    if (a->report) {
        __sync_synchronize();
        *a->report = error_kurd(r) ? NVME_BOARD_FAIL : NVME_BOARD_OK;
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

// ============================================================
// poll_report_board: 轮询并行汇报画板
//
// 不超过 timeout_us 内反复遍历 board[0..count)，每次遍历后睡眠
// poll_us（kthread_sleep 微秒级）。全部槽位非 0（全部汇报完成）
// 即提前退出；全部成功返回 true。
// ============================================================
static bool poll_report_board(volatile uint64_t* board, uint32_t count,
                              uint64_t timeout_us, uint64_t poll_us,
                              uint32_t* ok_count_out)
{
    uint64_t deadline = ktime::get_microsecond_stamp() + timeout_us;
    uint32_t ok = 0;
    uint32_t reported = 0;

    while (true) {
        ok = 0;
        reported = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t v = board[i];
            if (v != NVME_BOARD_IN_FLIGHT) {
                reported++;
                if (v == NVME_BOARD_OK) ok++;
            }
        }
        // 全部汇报完成 → 提前退出（无论成败，无可再等）
        if (reported == count) break;
        // 超时硬上限
        if (ktime::get_microsecond_stamp() >= deadline) break;
        kthread_sleep(poll_us);
    }

    if (ok_count_out) *ok_count_out = ok;
    return (reported == count) && (ok == count);
}

// ============================================================
// reap_threads: 收尸辅助
//
// 等待全部 tid 退出并 release_kthread。上限 1s；仍有存活者返回
// false —— 调用方此时不得释放汇报画板/参数块（防悬垂写）。
// ============================================================
static bool reap_threads(uint64_t* tids, uint32_t count)
{
    uint64_t deadline = ktime::get_microsecond_stamp() + 1000000;
    uint32_t alive = 0;
    do {
        alive = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (tids[i] == INVALID_TID) continue;
            zombie_observe_results_t zr;
            zombie_observe(tids[i], &zr);
            if (zr == ZOMBIE_DEAD) {
                release_kthread(tids[i]);
                tids[i] = INVALID_TID;
            } else if (zr == ZOMBIE_ALIVE) {
                alive++;
            } else {  // ZOMBIE_TID_NOT_FOUND：已不存在，视为回收完毕
                tids[i] = INVALID_TID;
            }
        }
        if (alive == 0) break;
        if (ktime::get_microsecond_stamp() >= deadline) break;
        kthread_sleep(NVME_PARALLEL_POLL_US);
    } while (true);
    return alive == 0;
}

// ============================================================
// nvme_offline_thread_entry: 单控制器关机线程（写汇报画板）
// ============================================================
static KURD_t nvme_offline_thread_entry(void* arg)
{
    auto* a = static_cast<nvme_init_thread_arg*>(arg);
    KURD_t r;
    if (!a->ctrl) {
        r = KURD_t(result_code::FAIL, 0,
            module_code::DEVICE, DEVICES_locs::NVMe,
            DEVICES_locs::NVMe_events::Offline,
            level_code::ERROR, err_domain::CORE_MODULE);
    } else {
        r = a->ctrl->offline(0);
        if (error_kurd(r)) {
            bsp_kout << "[NVMe] node " << (uint32_t)a->node_index
                     << ": offline failed, reason=0x";
            bsp_kout.shift_hex();
            bsp_kout << r.reason;
            bsp_kout.shift_dec();
            bsp_kout << kendl;
        } else {
            bsp_kout << "[NVMe] node " << (uint32_t)a->node_index
                     << ": offline OK" << kendl;
        }
    }

    if (a->report) {
        __sync_synchronize();
        *a->report = error_kurd(r) ? NVME_BOARD_FAIL : NVME_BOARD_OK;
    }
    return r;
}

// ============================================================
// nvme_parallel_init_all: 并行初始化所有 NVMe 控制器
//
// 参考 kshell NVMe_on 的初始化写法（PCIe 类码校验 + device_init），
// 但为每个控制器并行 spawn 一个内核线程，每个线程把结果写进共享的
// u64 汇报画板；主线程 ≤5s 轮询画板（每轮 50us），全部成功立即提前退出。
//
// 首次调用负责扫描 PCIe 并填充 node_array；重复调用直接复用已有节点
// （已初始化的控制器在 device_init/pre_init 内会无副作用跳过）。
// ============================================================
void nvme_parallel_init_all()
{
    // ---- 1. 首次调用：扫描 PCIe 并填充 node_array ----
    if (!NVMe_Controller::node_array || NVMe_Controller::controllers_count == 0) {
        uint32_t nvme_count = scan_pcie_nvme_count();
        bsp_kout << "[NVMe] Found " << (uint32_t)nvme_count << " controller(s)" << kendl;

        if (nvme_count == 0) {
            NVMe_Controller::node_array    = nullptr;
            NVMe_Controller::controllers_count = 0;
            return;
        }

        NVMe_Controller::node_array = new NVMe_Controller::node[nvme_count]();
        if (!NVMe_Controller::node_array) {
            NVMe_Controller::controllers_count = 0;
            return;
        }

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
                                NVMe_Controller::node_array[index].pcie_seg   = seg_ecam.seg_group_number;
                                NVMe_Controller::node_array[index].pcie_bus   = bus;
                                NVMe_Controller::node_array[index].pcie_dev   = dev;
                                NVMe_Controller::node_array[index].pcie_func  = func;
                                NVMe_Controller::node_array[index].ecam_va    = ecam_va;
                                NVMe_Controller::node_array[index].controller = nullptr;
                                index++;
                            }
                        }
                    }
                }
            }
        }

        NVMe_Controller::controllers_count = nvme_count;

        // 先全部创建实例再并行 spawn，避免 node_array 状态不确定
        for (uint32_t i = 0; i < nvme_count; i++) {
            NVMe_Controller::node_array[i].controller =
                new NVMe_Controller(NVMe_Controller::node_array[i].ecam_va);
        }
    }

    const uint32_t n = NVMe_Controller::controllers_count;
    if (n == 0) return;

    // ---- 2. 汇报画板 + 每线程参数（画板 0 = 进行中）----
    volatile uint64_t* board = new volatile uint64_t[n]();
    nvme_init_thread_arg* args = new nvme_init_thread_arg[n]();
    uint64_t* tids = new uint64_t[n]();

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < n; i++) {
        args[i].node_index = i;
        args[i].ctrl       = NVMe_Controller::node_array[i].controller;
        args[i].report     = &board[i];

        if (!args[i].ctrl) {
            board[i] = NVME_BOARD_FAIL;  // 无实例，直接失败槽
            continue;
        }

        KURD_t kurd;
        kthread_creating_package pkg;
        pkg.func_raw   = (uint64_t)nvme_init_thread_entry;
        pkg.args[0]    = (uint64_t)&args[i];
        pkg.args[1]    = 0;
        pkg.args[2]    = 0;
        pkg.args[3]    = 0;
        pkg.args[4]    = 0;
        pkg.launch_pid = fast_get_processor_id();
        tids[i] = creat_kthread(&pkg, &kurd);
        if (tids[i] == INVALID_TID || error_kurd(kurd)) {
            bsp_kout << "[NVMe] Failed to spawn init thread for node "
                     << (uint32_t)i << kendl;
            board[i] = NVME_BOARD_FAIL;
        } else {
            spawned++;
        }
    }

    bsp_kout << "[NVMe] Spawned " << (uint32_t)spawned
             << " parallel init thread(s)" << kendl;

    // ---- 3. ≤5s 轮询汇报画板（每轮 50us，全部成功提前退出）----
    uint32_t ok_count = 0;
    const bool all_ok = poll_report_board(board, n,
        NVME_PARALLEL_TIMEOUT_US, NVME_PARALLEL_POLL_US, &ok_count);

    bsp_kout << "[NVMe] Parallel init: " << (uint32_t)ok_count << "/" << (uint32_t)n
             << " OK" << (all_ok ? " (all ready)" : " (timeout/partial fail)") << kendl;

    // ---- 4. 收尸：全部线程退出后才释放画板/参数（防悬垂写）----
    if (!reap_threads(tids, n)) {
        bsp_kout << "[NVMe] Parallel init: straggler thread(s) alive, board/args leaked" << kendl;
        return;
    }
    delete[] board;
    delete[] args;
    delete[] tids;
}

// ============================================================
// nvme_parallel_offline_all: 并行析构/关机所有 NVMe 控制器
//
// 每个有效控制器并行 spawn 一个线程执行 offline()（释放队列/HMB/
// 缓冲并通知控制器 SHN 关机），结果写共享 u64 汇报画板，主线程
// ≤5s 轮询（每轮 50us），全部完成提前退出。供关机路径调用。
// ============================================================
void nvme_parallel_offline_all()
{
    if (!NVMe_Controller::node_array || NVMe_Controller::controllers_count == 0) {
        bsp_kout << "[NVMe] Parallel offline: nothing to do" << kendl;
        return;
    }

    // 紧凑收集有效控制器（跳过空槽）
    const uint32_t total = NVMe_Controller::controllers_count;
    uint32_t live = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (NVMe_Controller::node_array[i].controller) live++;
    }
    if (live == 0) {
        bsp_kout << "[NVMe] Parallel offline: no active controller" << kendl;
        return;
    }

    // ---- 汇报画板 + 每线程参数（画板 0 = 进行中）----
    volatile uint64_t* board = new volatile uint64_t[live]();
    nvme_init_thread_arg* args = new nvme_init_thread_arg[live]();
    uint64_t* tids = new uint64_t[live]();

    uint32_t spawned = 0;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < total; i++) {
        NVMe_Controller* ctrl = NVMe_Controller::node_array[i].controller;
        if (!ctrl) continue;

        args[slot].node_index = i;
        args[slot].ctrl       = ctrl;
        args[slot].report     = &board[slot];

        KURD_t kurd;
        kthread_creating_package pkg;
        pkg.func_raw   = (uint64_t)nvme_offline_thread_entry;
        pkg.args[0]    = (uint64_t)&args[slot];
        pkg.args[1]    = 0;
        pkg.args[2]    = 0;
        pkg.args[3]    = 0;
        pkg.args[4]    = 0;
        pkg.launch_pid = fast_get_processor_id();
        tids[slot] = creat_kthread(&pkg, &kurd);
        if (tids[slot] == INVALID_TID || error_kurd(kurd)) {
            bsp_kout << "[NVMe] Failed to spawn offline thread for node "
                     << (uint32_t)i << kendl;
            board[slot] = NVME_BOARD_FAIL;
        } else {
            spawned++;
        }
        slot++;
    }

    bsp_kout << "[NVMe] Spawned " << (uint32_t)spawned
             << " parallel offline thread(s)" << kendl;

    // ---- ≤5s 轮询汇报画板（每轮 50us，全部完成提前退出）----
    uint32_t ok_count = 0;
    const bool all_ok = poll_report_board(board, live,
        NVME_PARALLEL_TIMEOUT_US, NVME_PARALLEL_POLL_US, &ok_count);

    bsp_kout << "[NVMe] Parallel offline: " << (uint32_t)ok_count << "/" << (uint32_t)live
             << " OK" << (all_ok ? " (all down)" : " (timeout/partial fail)") << kendl;

    // ---- 收尸：全部线程退出后才释放画板/参数（防悬垂写）----
    if (!reap_threads(tids, live)) {
        bsp_kout << "[NVMe] Parallel offline: straggler thread(s) alive, board/args leaked" << kendl;
        return;
    }
    delete[] board;
    delete[] args;
    delete[] tids;
}
