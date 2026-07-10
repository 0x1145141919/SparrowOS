#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"
#include <util/arch/x86-64/cpuid_intel.h>

// ============================================================
// Doorbell 寄存器读写
// NVMe Spec 3.1.2:
//   SQyTDBL @ BAR0 + 1000h + ((2*y)   * (4 << CAP.DSTRD))
//   CQyHDBL @ BAR0 + 1000h + ((2*y+1) * (4 << CAP.DSTRD))
//   stride = 4 << DSTRD (bytes),  = 1 << DSTRD (uint32_t units)
// ============================================================
void NVMe_Controller::sq_dorbell_write(uint32_t qid, uint32_t value)
{
    doorbells[(uint32_t)qid * 2 * doorbell_stride_u32] = value;
    asm volatile("mfence" ::: "memory");
}

void NVMe_Controller::cq_dorbell_write(uint32_t qid, uint32_t value)
{
    doorbells[((uint32_t)qid * 2 + 1) * doorbell_stride_u32] = value;
    asm volatile("mfence" ::: "memory");
}

uint32_t NVMe_Controller::sq_dorbell_read(uint32_t qid)
{
    return doorbells[(uint32_t)qid * 2 * doorbell_stride_u32];
}

uint32_t NVMe_Controller::cq_dorbell_read(uint32_t qid)
{
    return doorbells[((uint32_t)qid * 2 + 1) * doorbell_stride_u32];
}
void NVMe_Controller::msix_enable(uint32_t x2apic_id, uint16_t msix_vec, uint8_t cpu_vec)
{
    MSIX_entry_t& entry=this->msix_table[msix_vec];

    // 1. Mask 该 entry，编程期间不触发中断
    entry.vector_control |= 1u;
    asm volatile("mfence" ::: "memory");

    // 2. 编程 MSI-X Address/Data — 严格遵循 d6749eb7 的硬件时序:
    //    upper → addr → upper=0 → data
    uint32_t addr = 0xfee00000 | ((x2apic_id & 0xff) << 12);
    uint32_t upper_addr = x2apic_id & ~0xff;
    entry.message_upper_addr = upper_addr;
    entry.message_addr       = addr;
    entry.message_upper_addr = 0;
    entry.message_data       = cpu_vec;
    asm volatile("mfence" ::: "memory");

    // 3. Unmask
    entry.vector_control &= ~1u;
    asm volatile("mfence" ::: "memory");
}
KURD_t NVMe_Controller::msix_vec_alloc(uint32_t processor_id, uint16_t msix_vec)
{
    KURD_t success=empty_kurd;
    success.event_code=DEVICES_locs::NVMe_events::msix_vec_alloc;
    // msix_vec 0=Admin CQ, ≥1=IO CQ → 与 cq_count 比较确保不越界
    if(msix_vec >= this->cq_count||processor_id >= logical_processor_count){
        KURD_t fail=empty_kurd;
        fail.event_code=DEVICES_locs::NVMe_events::msix_vec_alloc;
        fail.reason=DEVICES_locs::NVMe_events::msix_vec_alloc_results::fail_reasons::param_out_of_range;
        return fail;
    }
    KURD_t kurd;

    // ── 构造 interrupt_token ──
    // token_private 编码:
    //   lo 64 bits = NVMe_Controller* (this)
    //   hi 64 bits = CQ ID (= msix_vec, 0=Admin, ≥1=IO CQ)
    // interrupt_handle 从 token_private 提取两者后统一分发到 cq_interrupt_handler(qid)
    __uint128_t token_private = (__uint128_t)(uint64_t)this;
    token_private |= (__uint128_t)(uint64_t)msix_vec << 64;
    interrupt_token_t token = { token_private, 0, NVMe_Controller::interrupt_handle };

    uint8_t vec = out_interrupt_vec_alloc(&token, processor_id, &kurd);
    if(error_kurd(kurd))return kurd;

    // 跟踪分配状态（offline → msix_vec_free 时使用）
    if(msix_vec == 0){
        ADmin_queue_belonged_processor=processor_id;
        ADmin_queue_vec=vec;
    }else{
        IO_CQ_vecs[processor_id]=vec;
    }

    // ── 用 tran_get_x2apic_id 通过 GS slot 获取目标处理器的 x2APIC ID ──
    uint32_t x2apicid = tran_get_x2apic_id(processor_id);
    if (x2apicid == 0xFFFFFFFF) {
        out_interrupt_vec_free(vec, processor_id);
        KURD_t fail=empty_kurd;
        fail.event_code=DEVICES_locs::NVMe_events::msix_vec_alloc;
        fail.reason=DEVICES_locs::NVMe_events::msix_vec_alloc_results::fail_reasons::param_out_of_range;
        return fail;
    }
    msix_enable(x2apicid, msix_vec, vec);
    return success;
}
KURD_t NVMe_Controller::msix_vec_free(uint16_t msix_vec)
{
    KURD_t success=empty_kurd;
    KURD_t fail=empty_kurd;
    success.event_code=DEVICES_locs::NVMe_events::msix_vec_dealloc;
    fail.event_code=DEVICES_locs::NVMe_events::msix_vec_dealloc;
    MSIX_entry_t& entry=this->msix_table[msix_vec];

    // ── 从 MSI-X entry 还原 IDT vector 和目标 x2APIC ID ──
    uint8_t vec = (uint8_t)entry.message_data;
    uint32_t apic_id = (entry.message_upper_addr << 8)
                      | ((entry.message_addr >> 12) & 0xff);

    // ── 用 tran_get_processor_id 通过 GS slot 查表: x2apic_id → processor_id ──
    uint32_t processor_id = tran_get_processor_id(apic_id);
    if(processor_id == 0xFFFFFFFF){
        fail.reason=DEVICES_locs::NVMe_events::msix_vec_dealloc_results::fail_reasons::processor_not_found;
        return fail;
    }

    KURD_t kurd=out_interrupt_vec_free(vec,processor_id);
    if(error_kurd (kurd))return kurd;

    if(msix_vec == 0){
        ADmin_queue_belonged_processor=~0;
        ADmin_queue_vec=0xff;
    }
    // Mask entry
    entry.vector_control |= 1u;
    return success;
}

// ============================================================
// PCIe 枚举回调：NVMe 设备发现
// ============================================================
namespace {
    // 临时存储扫描到的 NVMe 设备（最大 64 个）
    static constexpr uint32_t NVME_DISCOVERY_MAX = 64;
    static struct {
        uint16_t seg;
        uint8_t  bus;
        uint8_t  dev;
        uint8_t  func;
        vaddr_t  ecam_va;
    } nvme_discovered[NVME_DISCOVERY_MAX];
    static uint32_t nvme_discovered_count = 0;
}

void pcie_nvme_on_device(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func,
                         vaddr_t ecam_va)
{
    if (nvme_discovered_count >= NVME_DISCOVERY_MAX) return;
    uint32_t i = nvme_discovered_count;
    nvme_discovered[i].seg     = seg;
    nvme_discovered[i].bus     = bus;
    nvme_discovered[i].dev     = dev;
    nvme_discovered[i].func    = func;
    nvme_discovered[i].ecam_va = ecam_va;
    nvme_discovered_count = i + 1;
}

void pcie_nvme_scan_complete()
{
    uint32_t count = nvme_discovered_count;
    if (count == 0) return;

    NVMe_Controller::node_array = new NVMe_Controller::node[count]();
    NVMe_Controller::controllers_count = count;

    for (uint32_t i = 0; i < count; i++) {
        NVMe_Controller::node_array[i].pcie_seg  = nvme_discovered[i].seg;
        NVMe_Controller::node_array[i].pcie_bus  = nvme_discovered[i].bus;
        NVMe_Controller::node_array[i].pcie_dev  = nvme_discovered[i].dev;
        NVMe_Controller::node_array[i].pcie_func = nvme_discovered[i].func;
        NVMe_Controller::node_array[i].ecam_va  = nvme_discovered[i].ecam_va;
        NVMe_Controller::node_array[i].controller = nullptr;
    }
}