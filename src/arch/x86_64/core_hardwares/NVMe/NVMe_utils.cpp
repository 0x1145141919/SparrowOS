#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/Interrupt_system/x86_vecs_deliver_mgr.h"
#include "arch/x86_64/Interrupt_system/loacl_processor.h"

KURD_t NVMe_Controller::default_kurd()
{
    return KURD_t(result_code::SUCCESS,0,module_code::DEVICE,DEVICES_locs::NVMe,0,level_code::INFO,err_domain::HARDWARE);
}
KURD_t NVMe_Controller::default_success()
{
    return default_kurd();
}
KURD_t NVMe_Controller::default_failure()
{
    return set_result_fail_and_error_level(default_kurd());
}
KURD_t NVMe_Controller::default_fatal()
{
    return set_fatal_result_level(default_kurd());
}
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
KURD_t NVMe_Controller::msix_vec_alloc(uint32_t processor_id, uint16_t msix_vec)
{
    using namespace DEVICES_locs::NVMe_events::msix_vec_alloc_results::fail_reasons;
    KURD_t success=default_success();
    success.event_code=DEVICES_locs::NVMe_events::msix_vec_alloc;
    //为了简化特地用cqid就只使用MSIX向量号
    if(msix_vec >= this->cq_count||processor_id >= logical_processor_count){
        KURD_t fail=default_failure();
        fail.event_code=DEVICES_locs::NVMe_events::msix_vec_alloc;
        fail.reason=param_out_of_range;
        return fail;
    }
    KURD_t kurd;
    uint8_t vec;
    if(msix_vec == 0){
        // 使用reinterpret_cast转换函数指针类型以匹配hard_interrupt_func_t签名
        hard_interrupt_func_t handler = reinterpret_cast<hard_interrupt_func_t>(ADMIN_CQ_handler);
        vec = out_interrupt_vec_alloc(handler, processor_id, &kurd);
        if(error_kurd(kurd))return kurd;
        ADmin_queue_belonged_processor=processor_id;
        ADmin_queue_vec=vec;
    }else{
        // 使用reinterpret_cast转换函数指针类型以匹配hard_interrupt_func_t签名
        hard_interrupt_func_t handler = reinterpret_cast<hard_interrupt_func_t>(IO_CQ_handler);
        vec = out_interrupt_vec_alloc(handler, processor_id, &kurd);
        if(error_kurd(kurd))return kurd;
        IO_CQ_vecs[processor_id]=vec;
    }
    // [GS_COMPLEX_TODO] 从 GS slot 读 APIC ID: slot 存储 query_x2apicid() 结果
    // 临时用 processor_id 代替，仅确保编译通过
    uint32_t x2apicid = static_cast<uint32_t>(processor_id);
    MSIX_entry_t& entry=this->msix_table[msix_vec];

    // 1. 先 Mask 该 entry，确保编程期间不会触发中断
    entry.vector_control |= 1u;
    asm volatile("mfence" ::: "memory");

    uint32_t addr = 0xfee00000 | ((x2apicid & 0xff) << 12);
    uint32_t upper_addr= x2apicid&~0xff;
    entry.message_upper_addr = upper_addr;
    entry.message_addr      = addr;
    entry.message_upper_addr = 0;
    entry.message_data      = vec;
    asm volatile("mfence" ::: "memory");

    // 3. Unmask
    entry.vector_control &= ~1u;
    asm volatile("mfence" ::: "memory");
    return success;
}
KURD_t NVMe_Controller::msix_vec_free(uint16_t msix_vec)
{
    KURD_t success=default_success();
    KURD_t fail=default_failure();
    success.event_code=DEVICES_locs::NVMe_events::msix_vec_dealloc;
    fail.event_code=DEVICES_locs::NVMe_events::msix_vec_dealloc;
    MSIX_entry_t& entry=this->msix_table[msix_vec];
    uint32_t addr=entry.message_addr;
    uint32_t addr_upper=entry.message_upper_addr;
    uint32_t data=entry.message_data;
    uint8_t vec=data;
    uint32_t apic_id=(addr_upper&~0xff)|((addr>>12)&0xff);
    uint32_t processor_id=0;
    // [GS_COMPLEX_TODO] 从 GS slot 查表: apic_id → processor_id
    // 临时假定 apic_id == processor_id
    processor_id = apic_id < logical_processor_count ? apic_id : 0;
    if(processor_id>=logical_processor_count){
        fail.reason=DEVICES_locs::NVMe_events::msix_vec_dealloc_results::fail_reasons::processor_not_found;
        return fail;
    }
    KURD_t kurd=out_interrupt_vec_free(vec,processor_id);
    if(error_kurd (kurd))return kurd;
    if(msix_vec){
        IO_CQ_vecs[processor_id]=0xff;
    }else{
        ADmin_queue_belonged_processor=~0;
        ADmin_queue_vec=0xff;
    }
    uint32_t ctrl=entry.vector_control;
    ctrl|=1;
    entry.vector_control=ctrl;
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