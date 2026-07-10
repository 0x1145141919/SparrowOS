#include "arch/x86_64/core_hardwares/NVMe/NVMe_surface.h"
#include "arch/x86_64/core_hardwares/NVMe/Identify_structues.h"
#include "arch/x86_64/core_hardwares/NVMe/io_queue_cmd.h"
#include "arch/x86_64/core_hardwares/NVMe/get_set_features.h"
#include "arch/x86_64/core_hardwares/NVMe/PRPs.h"
#include <memory/FreePagesAllocator.h>
#include <memory/phyaddr_accessor.h>
#include <util/kout.h>
#include <arch/x86_64/PCIe/base.h>
#include <util/arch/x86-64/cpuid_intel.h>

extern uint32_t logical_processor_count;

NVMe_Controller::node* NVMe_Controller::node_array;
uint32_t NVMe_Controller::controllers_count;

// ============================================================
// 预初始化阶段的 KURD helper
// ============================================================
namespace {

constexpr uint64_t CAP_TO(uint64_t cap)    { return (cap >> 24) & 0xFF; }
constexpr uint64_t CAP_DSTRD(uint64_t cap) { return (cap >> 32) & 0xF; }

constexpr uint32_t CC_EN   = 1u << 0;
constexpr uint32_t CC_IOCQES_SHIFT = 20;
constexpr uint32_t CC_IOSQES_SHIFT = 16;
constexpr uint32_t INTMC = (1u << 1) | (1u << 2);  // bits 1:2

constexpr uint32_t CSTS_RDY = 1u << 0;
constexpr uint32_t CSTS_CFS = 1u << 1;

constexpr uint16_t DEFAULT_ADMIN_QUEUE_ENTRIES = 64;
constexpr uint32_t SQ_ENTRY_SIZE = 64;
constexpr uint32_t CQ_ENTRY_SIZE = 16;


static KURD_t alloc_contiguous_pages(KURD_t* kurd_out,
                                     void** va_out,
                                     phyaddr_t* pa_out,
                                     uint32_t page_count,
                                     size_t zero_size) {
    *va_out = __wrapped_pgs_valloc(kurd_out, page_count,
                                    page_state_t::kernel_pinned, 12);
    if (error_kurd(*kurd_out) || !*va_out) {
        return *kurd_out;
    }
    ksetmem_8(*va_out, 0, zero_size);
    *pa_out = 0;
    *kurd_out = KspacePageTable::v_to_phyaddrtraslation(
        (vaddr_t)*va_out, *pa_out);
    if (error_kurd(*kurd_out) || !*pa_out) {
        return *kurd_out;
    }
    return empty_kurd;
}

} // anonymous namespace

// ============================================================
// wait_for_ready
// ============================================================
bool NVMe_Controller::wait_for_ready(head_regs_t* regs, bool target,
                                     uint32_t timeout_500ms)
{
    uint32_t remain_ms = timeout_500ms * 500;
    while (remain_ms > 0) {
        uint32_t csts = regs->controller_status;
        if (csts & CSTS_CFS) return false;
        if (((csts & CSTS_RDY) != 0) == target) return true;
        kthread_sleep(1 * 1000);
        remain_ms--;
    }
    return false;
}

// ============================================================
// ============================================================
// hmb_alloc：在 second_stage_init 中分配并部署 HMB
// __wrapped_pgs_valloc 保证物理连续，HMB 只需一个 vm_interval 描述
// 描述符列表在 Set Features 时临时构建在栈上（同步命令）
// ============================================================
KURD_t NVMe_Controller::hmb_alloc(KURD_t& kurd)
{
    uint32_t hmpre_4k = *(uint32_t*)(admin_buffer.vbase() + 0x110);
    if (hmpre_4k == 0) {
        bsp_kout << "[NVMe] HMB not recommended" << kendl;
        return empty_kurd;
    }

    uint32_t page_count = hmpre_4k;

    void* buf = __wrapped_pgs_valloc(
        &kurd, page_count, page_state_t::kernel_pinned, 12);
    if (error_kurd(kurd) || !buf) {
        hmb_buffer = {};  // zero all fields
        return kurd;
    }
    ksetmem_8(buf, 0, page_count * 4096);

    phyaddr_t buf_pa = 0;
    kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)buf, buf_pa);
    if (error_kurd(kurd)) {
        __wrapped_pgs_vfree(buf, page_count);
        hmb_buffer = {};
        return kurd;
    }

    uint32_t cc = head_regs->controller_configuration;
    uint32_t mps_shift = 12 + ((cc >> 7) & 0xF);
    uint32_t mps       = 1u << mps_shift;
    uint32_t mps_units = page_count * 4096 / mps;

    // 栈上构建描述符——synchronized_cmd_submit 同步完成
    NVMe::features_detail::hmb_descriptor_t desc;
    desc.baddr = buf_pa;
    desc.bsize = mps_units;
    desc.rsvd  = 0;

    // 描述符列表物理地址
    phyaddr_t desc_pa = buf_pa;  // 描述符放在 HMB 缓冲区开头

    NVMe::features_detail::hmb_cdw11_t en{};
    en.ehm = 1;

    NVMe::command::submit_command_common cmd{};
    cmd.fiedls.opcode = NVMe::command::admin_opcode::SET_FEATURES;
    cmd.fiedls.nsid   = 0;
    cmd.dwords[10]    = NVMe::features::FID_HOST_MEMORY_BUFFER;
    cmd.dwords[11]    = en.raw;
    cmd.dwords[12]    = mps_units;
    cmd.dwords[13]    = (uint32_t)desc_pa;
    cmd.dwords[14]    = (uint32_t)(desc_pa >> 32);
    cmd.dwords[15]    = 1;

    // 先把描述符写入 HMB 缓冲区头部（控制器可能在命令返回后读取）
    *(NVMe::features_detail::hmb_descriptor_t*)buf = desc;
    // 确保写入顺序：描述符先写，Set Features 后发
    __sync_synchronize();

    uint64_t enc = synchronized_cmd_submit(0, cmd);
    uint16_t cid = enc >> 16;
    NVMe::command::complete_command_common cqe = sqs[0].complete_commands_bank[cid];
    release_cmd(0, cid);
    if (NVMe::status::is_error(cqe.fields.status)) {
        __wrapped_pgs_vfree(buf, page_count);
        hmb_buffer = {};
        return kurd;
    }

    hmb_buffer.vpn  = reinterpret_cast<vaddr_t>(buf) >> 12;
    hmb_buffer.ppn  = buf_pa >> 12;
    hmb_buffer.npages = (uint64_t)page_count;
    hmb_buffer.access = KSPACE_RW_UC_ACCESS;

    bsp_kout << "[NVMe] HMB enabled: " << (uint32_t)page_count
             << " pages (" << (uint32_t)mps_units << " MPS units)" << kendl;
    return empty_kurd;
}

// ============================================================
// hmb_free：在 offline 中释放 HMB
// ============================================================
KURD_t NVMe_Controller::hmb_free(KURD_t& kurd)
{
    if (hmb_buffer.vbase() == 0)
        return empty_kurd;

    NVMe::features_detail::hmb_cdw11_t dis{};
    dis.ehm = 0;
    set_features_cmd(NVMe::features::FID_HOST_MEMORY_BUFFER,
                     dis.raw, 0, kurd);

    __wrapped_pgs_vfree((void*)hmb_buffer.vbase(), hmb_buffer.byte_cnt() / 4096);
    hmb_buffer = {};

    bsp_kout << "[NVMe] HMB freed" << kendl;
    return empty_kurd;
}
NVMe_Controller::NVMe_Controller(vaddr_t ecam) : ecam(ecam)
{
    sqs   = nullptr;
    cqs   = nullptr;
    NSs   = nullptr;
    head_regs                = nullptr;
    doorbells               = nullptr;
    doorbell_stride_u32     = 0;
    msix_table              = nullptr;
    msix_pending_bits_array = nullptr;
    max_msix_vectors        = 0;
    NS_count                = 0;
    state                   = NVMe::CTRL_UNINIT;
}

// ============================================================
// 第二阶段初始化
// ============================================================
KURD_t NVMe_Controller::second_stage_init()
{
    // ---- 1. 读 CAP ----
    NVMe::cap_reg_t cap{ .value = head_regs->cap };
    uint32_t to = cap.field.to;
    IO_SQ_ENTRY_COUNT = min(DEFAULT_IO_SQ_ENTRY_COUNT, (uint16_t)cap.field.mqes);
    IO_CQ_ENTRY_COUNT = min(DEFAULT_IO_CQ_ENTRY_COUNT, (uint16_t)cap.field.mqes);

    // 硬性要求：cap.mqes 必须 ≥ I/O 队列数
    if (cap.field.mqes < sq_count - 1)
        return empty_kurd;
    if (to == 0) to = 10;

    // ---- 2. CC.EN = 0, 配置 ----
    NVMe::controler_ctrl_t ctrl{ .value = head_regs->controller_configuration };
    ctrl.field.EN      = 0;
    ctrl.field.IOCQES  = 4;  // CQ entry size: 2^4 = 16
    ctrl.field.IOSQES  = 6;  // SQ entry size: 2^6 = 64
    ctrl.field.CSS     = 0;
    head_regs->controller_configuration = ctrl.value;
    if (!wait_for_ready(head_regs, false, to))
        return empty_kurd;

    KURD_t kurd = empty_kurd;

    // ---- 3. 分配 Admin 队列环 ----
    uint32_t asq_bytes = align_up(DEFAULT_ADMIN_QUEUE_ENTRIES * SQ_ENTRY_SIZE, 4096);
    uint32_t acq_bytes = align_up(DEFAULT_ADMIN_QUEUE_ENTRIES * CQ_ENTRY_SIZE, 4096);

    void* asq_va = nullptr;
    phyaddr_t asq_pa = 0;
    kurd = alloc_contiguous_pages(&kurd, &asq_va, &asq_pa, asq_bytes / 4096, asq_bytes);
    if (error_kurd(kurd)) return kurd;

    void* acq_va = nullptr;
    phyaddr_t acq_pa = 0;
    kurd = alloc_contiguous_pages(&kurd, &acq_va, &acq_pa, acq_bytes / 4096, acq_bytes);
    if (error_kurd(kurd)) return kurd;

    // ---- 4. AQA + ASQ/ACQ 基址 ----
    uint32_t aqa = (DEFAULT_ADMIN_QUEUE_ENTRIES - 1) |
                   ((DEFAULT_ADMIN_QUEUE_ENTRIES - 1) << 16);
    head_regs->admin_queue_attributes      = aqa;
    head_regs->admin_submission_queue_base = asq_pa;
    head_regs->admin_completion_queue_base = acq_pa;

    // ---- 5. CC.EN = 1 ----
    ctrl.field.EN = 1;
    head_regs->controller_configuration = ctrl.value;

    // Clear INTMC bits before enabling
    if (!wait_for_ready(head_regs, true, to))
        return empty_kurd;

    // Clear interrupt mask set
    head_regs->interrupt_mask_set = 0;
    __sync_synchronize();

    // ---- 6. 填充 sqs[0] / cqs[0] ----
    sqs[0].sq_ring = {
        .vpn = reinterpret_cast<vaddr_t>(asq_va) >> 12, .ppn = asq_pa >> 12,
        .npages = asq_bytes >> 12, .access = KSPACE_RW_UC_ACCESS };
    cqs[0].cq_ring = {
        .vpn = reinterpret_cast<vaddr_t>(acq_va) >> 12, .ppn = acq_pa >> 12,
        .npages = acq_bytes >> 12, .access = KSPACE_RW_UC_ACCESS };
    cqs[0].num_of_entries = DEFAULT_ADMIN_QUEUE_ENTRIES;

    bsp_kout << "[NVMe] Controller CAP=";
    bsp_kout.shift_hex();
    bsp_kout << cap.value;
    bsp_kout.shift_dec();
    bsp_kout << " nn=" << kendl;

    // ---- 7. Identify Controller ----
    // 使用 pre_init 中分配的 admin_buffer（1 页，4096 字节）
    if (admin_buffer.vpn == 0) {
        bsp_kout << "[NVMe] admin_buffer not allocated" << kendl;
        return kurd;
    }
    bsp_kout << "[NVMe] identify_ctrl..." << kendl;
    NVMe::identify_ctrl_t* id_ctrl = reinterpret_cast<NVMe::identify_ctrl_t*>(admin_buffer.vbase());
    // ---- 8. MSI-X 分配 ----
    kurd = msix_vec_alloc(fast_get_processor_id(), 0);
    if (error_kurd(kurd)) {
        bsp_kout << "[NVMe] msix_vec_alloc for admin vector failed" << kendl;
        return kurd;
    }
    kurd = identify_ctrl(admin_buffer.pbase(), kurd);
    if (error_kurd(kurd)) return empty_kurd;

    bsp_kout << "[NVMe] VID=0x";
    bsp_kout.shift_hex();
    bsp_kout << id_ctrl->vid;
    bsp_kout << " NN=";
    bsp_kout.shift_dec();
    bsp_kout << id_ctrl->nn;
    bsp_kout << kendl;

    // ---- 9. I/O 队列 + HMB 初始化 ----
    io_queue_init(sq_count-1,cq_count-1,kurd);
    if (error_kurd(kurd)) return kurd;

    kurd = hmb_alloc(kurd);
    if (error_kurd(kurd)) {
        bsp_kout << "[NVMe] hmb_alloc failed, continuing" << kendl;
    }

    // ---- 10. Identify Namespaces ----
    uint32_t nn = id_ctrl->nn;
    if (nn > 64) nn = 64;
    NSs    = new BlockDevice[nn]();
    NS_count = nn;

    for (uint32_t ns = 1; ns <= nn; ns++) {
        ksetmem_8(reinterpret_cast<void*>(admin_buffer.vbase()), 0, 4096*4);
        // admin_buffer.pbase 在 pre_init 中已固定，无需重复查询

        bsp_kout << "[NVMe] identify_ns ns=" << (uint32_t)ns << kendl;
        kurd = identify_ns(ns, admin_buffer.pbase(), kurd);
        if (error_kurd(kurd)) {
            bsp_kout << "[NVMe] ns=" << (uint32_t)ns << " failed, skip" << kendl;
            continue;
        }

        NVMe::identify_ns_nvm_t* ns_id = reinterpret_cast<NVMe::identify_ns_nvm_t*>(admin_buffer.vbase());
        uint64_t nsze  = ns_id->nsze;
        uint8_t  flbas = ns_id->flbas;
        uint32_t lfmt  = *(uint32_t*)(reinterpret_cast<uint8_t*>(admin_buffer.vbase()) + 128 + (flbas & 0x0F) * 4);
        uint32_t ss    = 1u << ((lfmt >> 16) & 0xFF);

        NSs[ns - 1].sector_size   = ss;
        NSs[ns - 1].sector_count  = nsze;
        NSs[ns - 1].block_device_type = 0;
        NSs[ns - 1].ops = nullptr;

        auto* p = new NVMe_device_private();
        p->controller_id = 0;
        p->nsid = ns;
        NSs[ns - 1].private_data = p;

        bsp_kout << "[NVMe] NS " << (uint32_t)ns << ": size=";
        bsp_kout.shift_hex();
        bsp_kout << nsze;
        bsp_kout.shift_dec();
        bsp_kout << " sectors, sector_size=" << ss << kendl;
    }

    // Read CMB register info if available
    uint32_t cmbloc = head_regs->controller_memory_buffer_location;
    uint32_t cmbsz  = head_regs->controller_memory_buffer_size;
    if (cmbsz > 0) {
        bsp_kout << "[NVMe] CMB available: LOC=0x";
        bsp_kout.shift_hex();
        bsp_kout << cmbloc << " SZ=0x" << cmbsz;
        bsp_kout.shift_dec();
        bsp_kout << kendl;
    }

    state = NVMe::CTRL_READY;
    bsp_kout << "[NVMe] init complete (" << (uint32_t)nn << " namespaces)" << kendl;
    return empty_kurd;
}

// ============================================================
// pre_init
// ============================================================
KURD_t NVMe_Controller::pre_init()
{
    if (this->state != NVMe::CTRL_UNINIT)
        return empty_kurd;

    // sqs 数组：__wrapped_pgs_valloc（物理连续，含内嵌 complete_commands_bank / block_tokens / flying_slots）
    {
        const uint64_t bytes = sizeof(sq_complex) * (logical_processor_count + 1);
        const uint64_t pages = align_up(bytes, 4096) >> 12;
        KURD_t ak;
        void* va = __wrapped_pgs_valloc(&ak, pages, page_state_t::kernel_pinned, 12);
        if (error_kurd(ak) || !va) return ak;
        ksetmem_8(va, 0, bytes);
        this->sqs = static_cast<sq_complex*>(va);
    }
    this->sq_count = logical_processor_count + 1;
    this->sqs[0].flying_slots.enable(this->sqs[0].flying_slots_raw_map, DEFAULT_ADMIN_QUEUE_ENTRIES);
    this->sqs[0].sqid          = 0;
    this->sqs[0].num_of_entries = DEFAULT_ADMIN_QUEUE_ENTRIES;
    this->sqs[0].belonged_cqid  = 0;
    this->sqs[0].tail_idx       = 0;

    pci_header_endpoint_t* header = reinterpret_cast<pci_header_endpoint_t*>(this->ecam);
    pci_command_t command = { .value = header->command };
    command.fields.memory_space = 0;
    command.fields.io_space = 0;
    command.fields.bus_master = 0;
    header->command = command.value;
    __sync_synchronize();

    uint8_t bar_idx = 0;
    while (bar_idx < 6) {
        bar_t bar = { .value = header->bars[bar_idx] };
        if (bar.io_field.identifier) { bar_idx++; continue; }

        if (bar.mem_field.mem_type == 0b10) {
            if (bar_idx + 1 >= 6) { bar_idx++; continue; }
            bar_t bar_up = { .value = header->bars[bar_idx + 1] };
            phyaddr_t bar_base = (bar.value & ~0xf) | (uint64_t(bar_up.value) << 32);
            header->bars[bar_idx + 1] = ~0;
            __sync_synchronize();
            header->bars[bar_idx] = ~0;
            __sync_synchronize();
            uint32_t probe = header->bars[bar_idx] & ~0xF;
            uint32_t probe_up = header->bars[bar_idx + 1];
            __sync_synchronize();
            uint64_t bar_size = 1 + ~((uint64_t(probe_up) << 32) | probe);
            header->bars[bar_idx + 1] = bar_up.value;
            __sync_synchronize();
            header->bars[bar_idx] = bar.value;
            __sync_synchronize();
            if (bar_size > 0 && bar_base != 0) {
                vm_interval bar_interval = {
                    .vpn = 0, .ppn = bar_base >> 12, .npages = bar_size >> 12,
                    .access = KSPACE_RW_UC_ACCESS };
                KURD_t kurd;
                vaddr_t mapped_vbase = Kspace_pinterval_alloc_and_map(bar_interval, &kurd);
                if (!error_kurd(kurd)) {
                    bar_intervals[bar_idx].vpn = mapped_vbase >> 12;
                    bar_intervals[bar_idx].ppn = bar_base >> 12;
                    bar_intervals[bar_idx].npages = bar_size >> 12;
                    bar_intervals[bar_idx].access = KSPACE_RW_UC_ACCESS;
                } else return kurd;
            }
            bar_idx += 2;
            continue;
        } else if (bar.mem_field.mem_type == 0) {
            if (bar.value == 0) { bar_idx++; continue; }
            uint32_t base = bar.value & ~0xF;
            header->bars[bar_idx] = ~0;
            __sync_synchronize();
            uint32_t probe = header->bars[bar_idx] & ~0xF;
            __sync_synchronize();
            uint64_t bar_size = 1 + static_cast<uint64_t>(~probe);
            header->bars[bar_idx] = bar.value;
            __sync_synchronize();
            if (bar_size > 0 && base != 0) {
                vm_interval bar_interval = {
                    .vpn = 0, .ppn = base >> 12, .npages = bar_size >> 12,
                    .access = KSPACE_RW_UC_ACCESS };
                KURD_t kurd;
                vaddr_t mapped_vbase = Kspace_pinterval_alloc_and_map(bar_interval, &kurd);
                if (!error_kurd(kurd)) {
                    bar_intervals[bar_idx].vpn = mapped_vbase >> 12;
                    bar_intervals[bar_idx].ppn = base >> 12;
                    bar_intervals[bar_idx].npages = bar_size >> 12;
                    bar_intervals[bar_idx].access = KSPACE_RW_UC_ACCESS;
                } else return kurd;
            }
            bar_idx++;
            continue;
        } else {
            bar_idx++;
            continue;
        }
    }

    command.fields.memory_space = 1;
    command.fields.bus_master = 1;
    header->command = command.value;
    __sync_synchronize();

    if (header->status & 0x10) {
        uint8_t cap_offset = header->capabilities_ptr;
        while (cap_offset != 0) {
            volatile uint8_t* cap_base = reinterpret_cast<volatile uint8_t*>(this->ecam) + cap_offset;
            uint8_t cap_id = cap_base[0];
            uint8_t next_cap = cap_base[1];

            if (cap_id == PCI_CAP_ID_MSIX) {
                volatile pci_msix_cap_t* msix_cap = reinterpret_cast<volatile pci_msix_cap_t*>(cap_base);
                pci_msix_ctrl_t msix_ctrl = { .value = msix_cap->ctrl };
                msix_ctrl.fields.function_mask = 0;
                msix_ctrl.fields.enable = true;
                this->max_msix_vectors = msix_ctrl.fields.table_size + 1;
                this->cq_count = 1+min(this->max_msix_vectors-1,logical_processor_count);
                {
                    const uint64_t bytes = sizeof(cq_complex) * this->cq_count;
                    const uint64_t pages = align_up(bytes, 4096) >> 12;
                    KURD_t ak;
                    void* va = __wrapped_pgs_valloc(&ak, pages, page_state_t::kernel_pinned, 12);
                    if (error_kurd(ak) || !va) return ak;
                    ksetmem_8(va, 0, bytes);
                    this->cqs = static_cast<cq_complex*>(va);
                }
                cqs[0].is_first_time = true;

                uint32_t table_bir = msix_cap->table_offset & 0x7;
                uint32_t table_offset = msix_cap->table_offset & ~0x7;
                uint32_t pba_bir  = msix_cap->pba_offset & 0x7;
                uint32_t pba_offset = msix_cap->pba_offset & ~0x7;

                if (table_bir < 6 && bar_intervals[table_bir].vpn != 0)
                    this->msix_table = reinterpret_cast<MSIX_entry_t*>(bar_intervals[table_bir].vbase() + table_offset);

                if (this->max_msix_vectors > 0) {
                    this->msix_pending_bits_array =
                        reinterpret_cast<uint64_t*>(bar_intervals[pba_bir].vbase() + pba_offset);
                    for (uint32_t i = 0; i < this->max_msix_vectors; i++)
                        this->msix_table[i].vector_control |= 1u;
                    asm volatile("mfence" ::: "memory");
                }
                msix_cap->ctrl = msix_ctrl.value;
                break;
            }
            cap_offset = next_cap;
        }
    }

    // 分配 admin 缓冲区（4 页，用于 Identify 等 Admin 命令 + 命名空间 Identify 暂存）
    {
        KURD_t kurd2;
        void* buf = __wrapped_pgs_valloc(&kurd2, 4,
                                          page_state_t::kernel_pinned, 12);
        if (!error_kurd(kurd2) && buf) {
            ksetmem_8(buf, 0, 4096 * 4);
            phyaddr_t buf_pa = 0;
            kurd2 = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)buf, buf_pa);
            if (!error_kurd(kurd2)) {
                admin_buffer.vpn  = reinterpret_cast<vaddr_t>(buf) >> 12;
                admin_buffer.ppn  = buf_pa >> 12;
                admin_buffer.npages   = 4;
                admin_buffer.access = KSPACE_RW_ACCESS;
            }
        }
    }

    head_regs = reinterpret_cast<head_regs_t*>(bar_intervals[0].vbase());
    doorbells = reinterpret_cast<uint32_t*>(bar_intervals[0].vbase() + 0x1000);
    NVMe::cap_reg_t cap{ .value = head_regs->cap };
    doorbell_stride_u32 = 1u << cap.field.DSTRD;
    return KURD_t();
}

// ============================================================
// device_init
// ============================================================
KURD_t NVMe_Controller::device_init(NVMe_Controller* dev)
{
    KURD_t kurd = dev->pre_init();
    if (error_kurd(kurd)) return kurd;
    return dev->second_stage_init();
}

// ============================================================
// offline
// ============================================================
KURD_t NVMe_Controller::offline(uint64_t flags)
{
    if (state != NVMe::CTRL_READY)
        return empty_kurd;

    KURD_t kurd = empty_kurd;
    // Free IO queues（先删所有 SQ，再删所有 CQ，需要 Admin 队列活着）
    io_queue_free(kurd);

    // Free HMB（通过 Admin 命令取消，再释放页面）
    hmb_free(kurd);

    // ---- 通知控制器关机，等待完成 ----
    NVMe::controler_ctrl_t ctrl{ .value = head_regs->controller_configuration };
    ctrl.field.SHN = 1;
    head_regs->controller_configuration = ctrl.value;

    uint32_t remain_ms = 10000;
    while (remain_ms > 0) {
        uint32_t csts = head_regs->controller_status;
        uint32_t shst = (csts >> 2) & 0x3;
        if (shst == 2) break;
        kthread_sleep(1 * 1000);
        remain_ms--;
    }

    // ---- 禁用控制器 ----
    ctrl.value = head_regs->controller_configuration;
    ctrl.field.EN = 0;
    ctrl.field.SHN = 0;
    head_regs->controller_configuration = ctrl.value;

    NVMe::cap_reg_t cap{ .value = head_regs->cap };
    uint32_t to = cap.field.to;
    if (to == 0) to = 10;
    wait_for_ready(head_regs, false, to);

    // ---- 控制器已停止，以下安全释放主机端资源 ----
    // Free Admin queue ring
    if (sqs[0].sq_ring.vpn != 0) {
        __wrapped_pgs_vfree(reinterpret_cast<void*>(sqs[0].sq_ring.vbase()),
                             sqs[0].sq_ring.npages);
        sqs[0].sq_ring.vpn = 0;
    }
    if (cqs[0].cq_ring.vpn != 0) {
        __wrapped_pgs_vfree(reinterpret_cast<void*>(cqs[0].cq_ring.vbase()),
                             cqs[0].cq_ring.npages);
        cqs[0].cq_ring.vpn = 0;
    }

    // Free admin buffer
    if (admin_buffer.vpn != 0) {
        __wrapped_pgs_vfree(reinterpret_cast<void*>(admin_buffer.vbase()), 4);
        admin_buffer.vpn = 0;
        admin_buffer.ppn = 0;
        admin_buffer.npages = 0;
    }

    // Free MSI-X vectors + mask table entries
    for (uint16_t v = 0; v < max_msix_vectors; v++) {
        msix_vec_free(v);
    }
    if (msix_table) {
        for (uint32_t i = 0; i < max_msix_vectors; i++)
            msix_table[i].vector_control |= 1;
    }

    {
        uint64_t pages = align_up(sizeof(sq_complex) * sq_count, 4096) >> 12;
        __wrapped_pgs_vfree(sqs, pages);
    }
    {
        uint64_t pages = align_up(sizeof(cq_complex) * cq_count, 4096) >> 12;
        __wrapped_pgs_vfree(cqs, pages);
    }
    delete[] NSs;
    sqs = nullptr;
    cqs = nullptr;
    NSs = nullptr;
    sq_count = 0;
    cq_count = 0;
    NS_count = 0;

    state = NVMe::CTRL_UNINIT;
    return empty_kurd;
}
