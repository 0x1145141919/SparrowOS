#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/PCIe/base.h"
#include "arch/x86_64/PCIe/prased.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include <util/arch/x86-64/cpuid_intel.h>

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
    bool*          done_flag;    // 线程完成后置 true（非 volatile，通过 kthread_wait 保证可见性）
    uint64_t*      result_ptr;   // 线程完成后存储 KURD_t 的低32位
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
                    volatile auto* cfg = (volatile uint8_t*)(seg.vminterval.vbase + off);

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
static void* nvme_init_thread_entry(void* arg)
{
    auto* a = static_cast<nvme_init_thread_arg*>(arg);

    if (a->ctrl) {
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
        // 将 KURD_t 的低32位写入 result_ptr 供主线程检查
        if (a->result_ptr)
            *a->result_ptr = (uint64_t)(
                (uint32_t)r.result | ((uint32_t)r.reason << 16));
    }

    if (a->done_flag)
        *a->done_flag = true;

    return nullptr;
}

// ============================================================
// hex_dump: 打印缓冲区前 n 字节（十六进制 + ASCII）
// ============================================================
static void hex_dump(const char* label, const void* buf, uint32_t bytes)
{
    bsp_kout << "[NVMe IOtest] " << label << kendl;
    auto* p = (const uint8_t*)buf;
    uint32_t limit = bytes < 64 ? bytes : 64;
    for (uint32_t i = 0; i < limit; i += 16) {
        bsp_kout << "  ";
        bsp_kout.shift_hex();
        bsp_kout << (uint32_t)i;
        bsp_kout.shift_dec();
        bsp_kout << ": ";
        for (uint32_t j = 0; j < 16 && i + j < limit; j++) {
            bsp_kout<<HEX << (uint32_t)p[i + j];
            bsp_kout << ' ';
        }
        bsp_kout << " |";
        for (uint32_t j = 0; j < 16 && i + j < limit; j++) {
            uint8_t c = p[i + j];
            bsp_kout << (char)(c >= 0x20 && c < 0x7F ? c : '.');
        }
        bsp_kout << '|' << kendl;
    }
}

// ============================================================
// nvme_iotest_thread_entry: 0:6:0 靶盘 I/O 命令测试线程
//
// 测试序列:
//   1. 读 LBA 0 → hex dump（应看到 GPT 签名）
//   2. 写 0xA5 pattern → 读回 → 逐字节对比
//   3. Compare 同一缓冲区 → 预期 SUCCESS
//   4. 写不同 pattern → Compare → 预期 Compare Failure
//   5. 清理缓冲区
// ============================================================
static void* nvme_iotest_thread_entry(void* arg)
{
    auto* ctrl = static_cast<NVMe_Controller*>(arg);
    if (!ctrl || ctrl->get_ns_count() == 0 || !ctrl->get_namespaces()) {
        bsp_kout << "[NVMe IOtest] No namespace available" << kendl;
        return nullptr;
    }

    BlockDevice* ns = &ctrl->get_namespaces()[0];  // 第一个 namespace
    uint32_t ss = ns->sector_size;
    if (ss == 0) ss = 512;

    bsp_kout << "[NVMe IOtest] Starting I/O tests, sector_size="
             << (uint32_t)ss << kendl;

    // ---- 分配 2 页物理连续缓冲区 ----
    KURD_t kurd;
    void* va = __wrapped_pgs_valloc(&kurd, 2, page_state_t::kernel_pinned, 12);
    if (!va || error_kurd(kurd)) {
        bsp_kout << "[NVMe IOtest] Buffer alloc failed" << kendl;
        return nullptr;
    }
    ksetmem_8(va, 0, 8192);

    phyaddr_t pa = 0;
    kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)va, pa);
    if (error_kurd(kurd) || pa == 0) {
        bsp_kout << "[NVMe IOtest] PA translation failed" << kendl;
        __wrapped_pgs_vfree(va, 2);
        return nullptr;
    }

    pbuf_t buf      = { .pbase = pa, .size = 8192 };
    pbuf_t buf2     = { .pbase = pa + ss, .size = 8192 - ss };
    LBA_interval_t interval = { .start = 0, .LBA_count = 1 };

    // ============================================================
    // TEST 1: Read LBA 0
    // ============================================================
    bsp_kout << "[NVMe IOtest] --- Test 1: Read LBA 0 ---" << kendl;
    ksetmem_8(va, 0, ss);
    KURD_t r1 = NVMe_Controller::read(ns, buf, interval, 0);
    if (!error_kurd(r1)) {
        hex_dump("LBA 0 after read:", va, ss);
    } else {
        bsp_kout << "[NVMe IOtest] Read FAILED, reason=0x";
        bsp_kout.shift_hex();
        bsp_kout << r1.reason;
        bsp_kout.shift_dec();
        bsp_kout << kendl;
    }

    // ============================================================
    // TEST 2: Write pattern → read back → verify
    // ============================================================
    bsp_kout << "[NVMe IOtest] --- Test 2: Write 0xA5 pattern ---" << kendl;
    ksetmem_8(va, 0xA5, ss);
    KURD_t r2w = NVMe_Controller::write(ns, buf, interval, 0);
    if (error_kurd(r2w)) {
        bsp_kout << "[NVMe IOtest] Write FAILED, reason=0x";
        bsp_kout.shift_hex();
        bsp_kout << r2w.reason;
        bsp_kout.shift_dec();
        bsp_kout << kendl;
    }

    ksetmem_8(va, 0, ss);
    KURD_t r2r = NVMe_Controller::read(ns, buf, interval, 0);
    if (!error_kurd(r2r)) {
        // 逐字节校验
        auto* data = (uint8_t*)va;
        bool match = true;
        for (uint32_t i = 0; i < ss; i++) {
            if (data[i] != 0xA5) { match = false; break; }
        }
        bsp_kout << "[NVMe IOtest] Readback verify: "
                 << (match ? "PASS" : "FAIL (data mismatch)") << kendl;
        if (!match) hex_dump("readback data:", va, 32);
    } else {
        bsp_kout << "[NVMe IOtest] Readback FAILED, reason=0x";
        bsp_kout.shift_hex();
        bsp_kout << r2r.reason;
        bsp_kout.shift_dec();
        bsp_kout << kendl;
    }

    // ============================================================
    // TEST 3: Compare with same buffer → expect SUCCESS
    // ============================================================
    bsp_kout << "[NVMe IOtest] --- Test 3: Compare (self, expect match) ---" << kendl;
    KURD_t r3 = NVMe_Controller::compare(ns, buf, interval, 0);
    bsp_kout << "[NVMe IOtest] Compare result: status=0x";
    bsp_kout.shift_hex();
    bsp_kout << (uint32_t)r3.reason;
    bsp_kout.shift_dec();
    bsp_kout << (!error_kurd(r3) ? " (PASS)" : " (UNEXPECTED FAIL)") << kendl;

    // ============================================================
    // TEST 4: Write different pattern → Compare → expect FAILURE
    // ============================================================
    bsp_kout << "[NVMe IOtest] --- Test 4: Compare (different, expect FAIL) ---" << kendl;
    ksetmem_8((uint8_t*)va + ss, 0x5A, ss);  // buf2 = 0x5A pattern
    KURD_t r4 = NVMe_Controller::compare(ns, buf2, interval, 0);
    bsp_kout << "[NVMe IOtest] Compare result: status=0x";
    bsp_kout <<HEX<< (uint32_t)r4.reason;
    bsp_kout.shift_dec();
    if (error_kurd(r4)) {
        bsp_kout << " (expected failure)" << kendl;
    } else {
        bsp_kout << " (PASS — unexpected, data may be zeroed or deallocated)" << kendl;
    }

    // ============================================================
    // Cleanup
    // ============================================================
    // 恢复 LBA 0（写零清理）
    ksetmem_8(va, 0, ss);
    NVMe_Controller::write(ns, buf, interval, 0);

    __wrapped_pgs_vfree(va, 2);

    bsp_kout << "[NVMe IOtest] All tests completed" << kendl;
    return nullptr;
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
                        volatile auto* cfg = (volatile uint8_t*)(seg_ecam.vminterval.vbase + off);

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
                            vaddr_t ecam_va = seg_ecam.vminterval.vbase + off;
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
    // 每个线程需要独立的内存存放参数 + 完成标记
    nvme_init_thread_arg* thread_args   = new nvme_init_thread_arg[nvme_count]();
    uint64_t*             thread_results = new uint64_t[nvme_count]();
    bool*                 done_flags     = new bool[nvme_count]();
    uint64_t*             tids           = new uint64_t[nvme_count]();

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < nvme_count; i++) {
        thread_args[i].node_index  = i;
        thread_args[i].ctrl        = node_array[i].controller;
        thread_args[i].done_flag   = &done_flags[i];
        thread_args[i].result_ptr  = &thread_results[i];
        done_flags[i]              = false;

        KURD_t kurd;
        tids[i] = create_kthread(nvme_init_thread_entry,
                                 &thread_args[i], &kurd);
        if (tids[i] == INVALID_TID || error_kurd(kurd)) {
            bsp_kout << "[NVMe] Failed to spawn init thread for node "
                     << (uint32_t)i << kendl;
            done_flags[i] = true;  // 标记为已完成（未真正开始）
        } else {
            spawned++;
        }
    }

    bsp_kout << "[NVMe] Spawned " << (uint32_t)spawned << " init thread(s)" << kendl;

    // ---- 6. 等待所有线程完成 ----
    for (uint32_t i = 0; i < nvme_count; i++) {
        if (tids[i] != INVALID_TID) {
            kthread_wait(tids[i]);
        }
    }

    // ---- 7. 汇总 ----
    uint32_t ok_count = 0;
    uint32_t fail_count = 0;
    for (uint32_t i = 0; i < nvme_count; i++) {
        // thread_results[i] 低16位 = result, 次低16位 = reason
        // result=0 → SUCCESS
        uint32_t low = (uint32_t)(thread_results[i] & 0xFFFFFFFF);
        if ((low & 0xFFFF) == 0) {
            ok_count++;
        } else {
            fail_count++;
        }
    }

    bsp_kout << "[NVMe] Init summary: " << (uint32_t)ok_count
             << " OK, " << (uint32_t)fail_count << " failed" << kendl;

    // ---- 8b. 若 0:6:0 存在且初始化成功，创建测试线程（不 wait） ----
    if (ok_count > 0) {
        constexpr uint8_t  TARGET_BUS  = 0;
        constexpr uint8_t  TARGET_DEV  = 6;
        constexpr uint8_t  TARGET_FUNC = 0;

        for (uint32_t i = 0; i < nvme_count; i++) {
            if (node_array[i].pcie_bus  == TARGET_BUS &&
                node_array[i].pcie_dev  == TARGET_DEV &&
                node_array[i].pcie_func == TARGET_FUNC)
            {
                uint32_t result = (uint32_t)(thread_results[i] & 0xFFFFFFFF);
                if ((result & 0xFFFF) == 0 && node_array[i].controller) {
                    KURD_t spawn_kurd;
                    uint64_t test_tid = create_kthread(
                        nvme_iotest_thread_entry,
                        node_array[i].controller, &spawn_kurd);
                    if (test_tid != INVALID_TID && !error_kurd(spawn_kurd)) {
                        bsp_kout << "[NVMe] I/O test thread spawned for 0:6:0" << kendl;
                    }
                }
                break;
            }
        }
    }

    // ---- 8. 清理临时资源 ----
    delete[] thread_args;
    delete[] thread_results;
    delete[] done_flags;
    delete[] tids;

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
