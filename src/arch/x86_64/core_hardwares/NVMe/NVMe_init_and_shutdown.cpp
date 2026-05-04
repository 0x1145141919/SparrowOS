#include "arch/x86_64/core_hardwares/NVMe.h"
#include "arch/x86_64/PCIe/base.h"
#include "util/arch/x86-64/cpuid_intel.h"
extern uint32_t logical_processor_count;
NVMe_Controller::node* NVMe_Controller::node_array;
uint32_t NVMe_Controller::controllers_count;
// ============================================================
// 预初始化阶段的 KURD fail reason（先放在源文件，待头文件采纳）
// ============================================================
namespace {
static KURD_t make_success() {
    return KURD_t(
        result_code::SUCCESS, 0,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::Init,
        level_code::INFO, err_domain::CORE_MODULE);
}
static KURD_t make_fail(uint16_t reason) {
    return KURD_t(
        result_code::FAIL, reason,
        module_code::DEVICE, DEVICES_locs::NVMe,
        DEVICES_locs::NVMe_events::Init,
        level_code::ERROR, err_domain::CORE_MODULE);
}

// ============================================================
// 辅助常量
// ============================================================
constexpr uint64_t CAP_TO(uint64_t cap)    { return (cap >> 24) & 0xFF; }
constexpr uint64_t CAP_DSTRD(uint64_t cap) { return (cap >> 32) & 0xF; }

constexpr uint32_t CC_EN   = 1u << 0;

constexpr uint32_t CSTS_RDY = 1u << 0;
constexpr uint32_t CSTS_CFS = 1u << 1;

constexpr uint8_t  OP_IDENTIFY      = 0x06;
constexpr uint32_t IDENTIFY_CTRL    = 0x01;
constexpr uint32_t IDENTIFY_NS      = 0x00;
constexpr uint32_t IDENTIFY_SIZE    = 4096;

constexpr uint16_t DEFAULT_ADMIN_QUEUE_ENTRIES = 64;
constexpr uint32_t SQ_ENTRY_SIZE = 64;
constexpr uint32_t CQ_ENTRY_SIZE = 16;

// ============================================================
// 辅助函数：分配物理连续页并获取虚拟/物理地址
// ============================================================
static KURD_t alloc_contiguous_pages(KURD_t* kurd_out, 
                                     void** va_out, 
                                     phyaddr_t* pa_out,
                                     uint32_t page_count,
                                     size_t zero_size) {
    *va_out = __wrapped_pgs_valloc(kurd_out, page_count, page_state_t::kernel_pinned, 12);
    if (error_kurd(*kurd_out) || !*va_out) {
        return *kurd_out;
    }
    
    // 清零内存
    ksetmem_8(*va_out, 0, zero_size);
    
    // 获取物理地址
    *pa_out = 0;
    *kurd_out = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)*va_out, *pa_out);
    if (error_kurd(*kurd_out) || !*pa_out) {
        return *kurd_out;
    }
    
    return make_success();
}

// ============================================================
// 等待 CSTS.RDY 达到目标状态（带超时）
// ============================================================

} // anonymous namespace
bool NVMe_Controller::wait_for_ready(head_regs_t* regs, bool target,
                           uint32_t timeout_500ms)
{
    uint32_t remain_ms = timeout_500ms * 500;  // TO 单位 = 500ms
    while (remain_ms > 0) {
        uint32_t csts = regs->controller_status;
        if (csts & CSTS_CFS) return false;
        if (((csts & CSTS_RDY) != 0) == target) return true;
        kthread_sleep(1 * 1000);  // 1ms，让出 CPU
        remain_ms--;
    }
    return false;
}
// ============================================================
// 构造函数（PCI 探测与 MMIO 映射）
// ============================================================
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
// 第二阶段初始化（线程化）
// ============================================================
KURD_t NVMe_Controller::second_stage_init()
{
    // ---- 1. 读 CAP ----
    NVMe::cap_reg_t cap={
        .value = head_regs->cap
    };
    uint32_t to = cap.field.to;
    IO_SQ_ENTRY_COUNT=min(DEFAULT_IO_SQ_ENTRY_COUNT,cap.field.mqes);
    IO_CQ_ENTRY_COUNT=min(DEFAULT_IO_CQ_ENTRY_COUNT,cap.field.mqes);
    if (to == 0) to = 10;
    using namespace DEVICES_locs::NVMe_events::init_results::fail_reasons;
    // ---- 2. 禁用控制器 ----
    NVMe::controler_ctrl_t ctrl= {.value=head_regs->controller_configuration};
    ctrl.field.EN=0;
    ctrl.field.IOCQES=4;
    ctrl.field.IOSQES=6;
    ctrl.field.CSS=0;
    head_regs->controller_configuration=ctrl.value;
    if (!wait_for_ready(head_regs, false, to))
        return make_fail(ctrl_disable_timeout);
    
    KURD_t kurd = empty_kurd;
    
    // ---- 3. 分配 Admin 队列环内存（物理连续） ----
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

    // ---- 4. 写 AQA + ASQ/ACQ 基址 ----
    uint32_t aqa = (DEFAULT_ADMIN_QUEUE_ENTRIES - 1) | ((DEFAULT_ADMIN_QUEUE_ENTRIES - 1) << 16);
    head_regs->admin_queue_attributes       = aqa;
    head_regs->admin_submission_queue_base  = asq_pa;
    head_regs->admin_completion_queue_base  = acq_pa;

    // ---- 5. CC.EN = 1 ----
    ctrl.field.EN=1;
    //其它配置
    head_regs->controller_configuration = ctrl.value;
    if (!wait_for_ready(head_regs, true, to))
        return make_fail(ctrl_enable_timeout);

    // ---- 6. 填充 sqs[0] / cqs[0] 的环信息 ----
    sqs[0].sq_ring = {
        .vbase = (vaddr_t)asq_va, .pbase = asq_pa,
        .size = asq_bytes, .access = KSPACE_RW_UC_ACCESS };
    cqs[0].cq_ring = {
        .vbase = (vaddr_t)acq_va, .pbase = acq_pa,
        .size = acq_bytes, .access = KSPACE_RW_UC_ACCESS };
    cqs[0].num_of_entries=DEFAULT_ADMIN_QUEUE_ENTRIES;
    // ---- 7. Identify Controller ----
    void* id_buf = nullptr;
    phyaddr_t id_pa = 0;
    kurd = alloc_contiguous_pages(&kurd, &id_buf, &id_pa, 1, IDENTIFY_SIZE);
    if (error_kurd(kurd)) return kurd;

    NVMe::command::submit_command_common ic{};
    ic.fiedls.opcode = OP_IDENTIFY;
    ic.fiedls.nsid   = 0;
    ic.fiedls.DPTR1  = id_pa;
    ic.dwords[10]    = IDENTIFY_CTRL;
    kurd=msix_vec_alloc(fast_get_processor_id(),0);
    if(error_kurd(kurd))return kurd;
    uint64_t st = ADMIN_cmd_submit_and_process(ic, kurd);
    if (NVMe::status::is_error((uint16_t)st))
        return make_fail(identify_ctrl_fail);

    uint32_t nn = *(uint32_t*)((uint8_t*)id_buf + 512);
    if (nn > 64) nn = 64;

    // ---- 8. Identify Namespaces ----
    NSs    = new BlockDevice[nn]();
    NS_count = nn;
    for (uint32_t ns = 1; ns <= nn; ns++) {
        ksetmem_8(id_buf, 0, IDENTIFY_SIZE);
        kurd = KspacePageTable::v_to_phyaddrtraslation((vaddr_t)id_buf, id_pa);
        if (error_kurd(kurd)) continue;

        NVMe::command::submit_command_common nc{};
        nc.fiedls.opcode = OP_IDENTIFY;
        nc.fiedls.nsid   = ns;
        nc.fiedls.DPTR1  = id_pa;
        nc.dwords[10]    = IDENTIFY_NS;

        if (NVMe::status::is_error((uint16_t)ADMIN_cmd_submit_and_process(nc, kurd)))
            continue;

        uint64_t nsze  = *(uint64_t*)((uint8_t*)id_buf);
        uint8_t  flbas = *(uint8_t*)((uint8_t*)id_buf + 76);
        uint32_t lfmt  = *(uint32_t*)((uint8_t*)id_buf + 128 + (flbas & 0x0F) * 4);
        uint32_t ss    = 1u << ((lfmt >> 16) & 0xFF);

        NSs[ns - 1].sector_size   = ss;
        NSs[ns - 1].sector_count  = nsze;
        NSs[ns - 1].block_device_type = 0;
        NSs[ns - 1].ops = nullptr;

        auto* p = new NVMe_device_private();
        p->controller_id = 0;
        p->nsid = ns;
        NSs[ns - 1].private_data = p;
    }

    state = NVMe::CTRL_READY;
    return make_success();
}

KURD_t NVMe_Controller::pre_init()
{
    using namespace DEVICES_locs::NVMe_events::pre_init_results::fail_reasons;
    // ==== pre_init：堆数据结构分配 ====
    if (this->state != NVMe::CTRL_UNINIT)
        return make_fail(DEVICES_locs::NVMe_events::init_results::fail_reasons::already_init);
    this->sqs = new sq_complex[logical_processor_count+1]();
    this->sq_count = logical_processor_count+1;
    this->sqs[0].sq_bitmap = new Ktemplats::kernel_bitmap(DEFAULT_ADMIN_QUEUE_ENTRIES);
    this->sqs[0].block_tokens = new uint64_t[DEFAULT_ADMIN_QUEUE_ENTRIES]();
    this->sqs[0].sqid          = 0;
    this->sqs[0].num_of_entries = DEFAULT_ADMIN_QUEUE_ENTRIES;
    this->sqs[0].belonged_cqid  = 0;
    this->sqs[0].tail_idx       = 0;
    // ==== pre_init：PCI BAR 映射 + MSI-X 设置 ====
    // 通过ecam获取PCIe配置空间头
    pci_header_endpoint_t* header = reinterpret_cast<pci_header_endpoint_t*>(this->ecam);
    // 禁用设备的Memory/IO空间使能，准备探测BAR
    pci_command_t command = {
        .value = header->command
    };
    command.fields.memory_space = 0;
    command.fields.io_space = 0;
    command.fields.bus_master = 0;
    header->command = command.value;
    __sync_synchronize();
    
    // 遍历所有6个BAR寄存器
    uint8_t bar_idx = 0;
    while (bar_idx < 6) {
        bar_t bar = {
            .value = header->bars[bar_idx]
        };
        
        // 跳过I/O空间BAR
        if (bar.io_field.identifier) {
            bar_idx++;
            continue;
        }
        
        // 处理64位内存空间BAR
        if (bar.mem_field.mem_type == 0b10) {
            if (bar_idx + 1 >= 6) {
                // 无效的64位BAR（没有高位）
                bar_idx++;
                continue;
            }
            
            bar_t bar_up = {
                .value = header->bars[bar_idx + 1]
            };
            
            // 计算BAR基地址
            phyaddr_t bar_base = (bar.value & ~0xf) | (uint64_t(bar_up.value) << 32);
            
            // 探测BAR大小：先写高32位，再写低32位
            header->bars[bar_idx + 1] = ~0;
            __sync_synchronize();
            header->bars[bar_idx] = ~0;
            __sync_synchronize();
            
            uint32_t probe = header->bars[bar_idx];
            __sync_synchronize();
            uint32_t probe_up = header->bars[bar_idx + 1];
            __sync_synchronize();
            
            probe &= ~0xF;
            uint64_t bar_size = 1 + ~((uint64_t(probe_up) << 32) | probe);
            
            // 恢复原始BAR值：先写高位，再写低位
            header->bars[bar_idx + 1] = bar_up.value;
            __sync_synchronize();
            header->bars[bar_idx] = bar.value;
            __sync_synchronize();
            
            // 映射此BAR到虚拟地址空间
            if (bar_size > 0 && bar_base != 0) {
                vm_interval bar_interval = {
                    .vbase = 0,
                    .pbase = bar_base,
                    .size = bar_size,
                    .access = KSPACE_RW_UC_ACCESS
                };
                
                KURD_t kurd;
                vaddr_t mapped_vbase = phyaddr_direct_map(&bar_interval, &kurd);
                if (!error_kurd(kurd)) {
                    bar_intervals[bar_idx].vbase = mapped_vbase;
                    bar_intervals[bar_idx].pbase = bar_base;
                    bar_intervals[bar_idx].size = bar_size;
                    bar_intervals[bar_idx].access = KSPACE_RW_UC_ACCESS;
                }else return kurd;
            }
            
            bar_idx += 2; // 64位BAR占用两个槽位
            continue;
        }
        // 处理32位内存空间BAR
        else if (bar.mem_field.mem_type == 0) {
            if (bar.value == 0) {
                bar_idx++;
                continue;
            }
            
            // 计算BAR基地址
            uint32_t base = bar.value & ~0xF;
            
            // 探测BAR大小
            header->bars[bar_idx] = ~0;
            __sync_synchronize();
            uint32_t probe = header->bars[bar_idx];
            __sync_synchronize();
            probe &= ~0xF;
            uint64_t bar_size = 1 + static_cast<uint64_t>(~probe);
            
            // 恢复原始BAR值
            header->bars[bar_idx] = bar.value;
            __sync_synchronize();
            
            // 映射此BAR到虚拟地址空间
            if (bar_size > 0 && base != 0) {
                vm_interval bar_interval = {
                    .vbase = 0,
                    .pbase = base,
                    .size = bar_size,
                    .access = KSPACE_RW_UC_ACCESS
                };
                
                KURD_t kurd;
                vaddr_t mapped_vbase = phyaddr_direct_map(&bar_interval, &kurd);
                if (!error_kurd(kurd)) {
                    bar_intervals[bar_idx].vbase = mapped_vbase;
                    bar_intervals[bar_idx].pbase = base;
                    bar_intervals[bar_idx].size = bar_size;
                    bar_intervals[bar_idx].access = KSPACE_RW_UC_ACCESS;
                }else return kurd;
            }
            
            bar_idx++;
            continue;
        }
        else {
            // 未知的BAR类型，跳过
            bar_idx++;
            continue;
        }
    }
    
    // 恢复设备解码能力
    command.fields.memory_space = 1;
    command.fields.bus_master = 1;
    header->command = command.value;
    __sync_synchronize();
    
    // ==== 遍历PCIe能力链表，查找MSI-X能力 ====
    if (header->status & 0x10) { // 检查capabilities_list位
        uint8_t cap_offset = header->capabilities_ptr;
        
        while (cap_offset != 0) {
            // 读取能力ID和下一个能力偏移
            volatile uint8_t* cap_base = reinterpret_cast<volatile uint8_t*>(this->ecam) + cap_offset;
            uint8_t cap_id = cap_base[0];
            uint8_t next_cap = cap_base[1];
            
            // 检查是否为MSI-X能力 (ID = 0x11)
            if (cap_id == PCI_CAP_ID_MSIX) {
                volatile pci_msix_cap_t* msix_cap = reinterpret_cast<volatile pci_msix_cap_t*>(cap_base);
                
                // 解析MSI-X控制寄存器
                pci_msix_ctrl_t msix_ctrl = {
                    .value = msix_cap->ctrl
                };

                // 清 function_mask，使能 MSI-X
                msix_ctrl.fields.function_mask = 0;
                msix_ctrl.fields.enable = true;

                this->max_msix_vectors = msix_ctrl.fields.table_size + 1;
                this->cqs = new cq_complex[this->max_msix_vectors]();
                cqs[0].is_first_time=true;
                this->cq_count = this->max_msix_vectors;
                this->IO_CQ_vecs = new uint8_t[this->max_msix_vectors - 1]();

                uint32_t table_bir = msix_cap->table_offset & 0x7;
                uint32_t table_offset = msix_cap->table_offset & ~0x7;
                uint32_t pba_bir  = msix_cap->pba_offset & 0x7;
                uint32_t pba_offset = msix_cap->pba_offset & ~0x7;

                if (table_bir < 6 && bar_intervals[table_bir].vbase != 0)
                    this->msix_table = (MSIX_entry_t*)(bar_intervals[table_bir].vbase + table_offset);

                if (this->max_msix_vectors > 0) {
                    this->msix_pending_bits_array =
                        (uint64_t*)(bar_intervals[pba_bir].vbase + pba_offset);

                    // 映射后立即 Mask 所有 entry，防止使能瞬间误触发
                    for (uint32_t i = 0; i < this->max_msix_vectors; i++) {
                        this->msix_table[i].vector_control |= 1u;
                    }
                    asm volatile("mfence" ::: "memory");
                }

                msix_cap->ctrl = msix_ctrl.value;
                break;
            }
            
            // 移动到下一个能力
            cap_offset = next_cap;
        }
    }
    head_regs=(head_regs_t*)bar_intervals[0].vbase;
    doorbells=(uint32_t*)(bar_intervals[0].vbase+0x1000);
    NVMe::cap_reg_t cap{ .value = head_regs->cap };
    doorbell_stride_u32 = 1u << cap.field.DSTRD;
    return KURD_t();
} // ============================================================
// device_init：外部入口（可传入 create_kthread）
// 1. 堆数据结构分配（pre_init）
// 2. 控制器硬件初始化（second_stage_init）
// ============================================================
KURD_t NVMe_Controller::device_init(NVMe_Controller* dev)
{
    KURD_t kurd = dev->pre_init();
    if(error_kurd(kurd))
        return kurd;
    // ==== second_stage_init ====
    return dev->second_stage_init();
}
KURD_t NVMe_Controller::offline(uint64_t flags)
{
    using namespace DEVICES_locs::NVMe_events::init_results::fail_reasons;

    if (state != NVMe::CTRL_READY)
        return make_fail(already_init);  // 不在运行状态，无需下线

    KURD_t kurd = empty_kurd;
/*
    // ---- 1. 遍历 I/O 队列，逆序删除 ----
    // I/O SQ 从 1 开始，I/O CQ 从 1 开始
    for (uint16_t qid = 1; qid < sq_count; qid++) {
        if (!sqs[qid].sq_ring.vbase) continue;

        NVMe::command::submit_command_common del_sq{};
        del_sq.fiedls.opcode = 0x00;  // Delete I/O Submission Queue
        del_sq.dwords[10] = qid;      // SQ Identifier
        uint64_t st = ADMIN_cmd_submit_and_process(del_sq, kurd);
        if (NVMe::status::is_error((uint16_t)st)) {
            // 非致命：继续删下一个
        }

        // 释放 SQ ring 内存
        if (sqs[qid].sq_ring.vbase) {
            __wrapped_pgs_vfree((void*)sqs[qid].sq_ring.vbase);
            sqs[qid].sq_ring.vbase = 0;
        }
        delete sqs[qid].sq_bitmap;
        delete[] sqs[qid].block_tokens;
    }

    for (uint16_t qid = 1; qid < cq_count; qid++) {
        if (!cqs[qid].cq_ring.vbase) continue;

        NVMe::command::submit_command_common del_cq{};
        del_cq.fiedls.opcode = 0x04;  // Delete I/O Completion Queue
        del_cq.dwords[10] = qid;      // CQ Identifier
        uint64_t st = ADMIN_cmd_submit_and_process(del_cq, kurd);
        if (NVMe::status::is_error((uint16_t)st)) {
            // 非致命
        }

        if (cqs[qid].cq_ring.vbase) {
            __wrapped_pgs_vfree((void*)cqs[qid].cq_ring.vbase);
            cqs[qid].cq_ring.vbase = 0;
        }
    }*/

    // ---- 2. 释放 Admin 队列环内存 ----
    if (sqs[0].sq_ring.vbase) {
        __wrapped_pgs_vfree((void*)sqs[0].sq_ring.vbase,sqs[0].sq_ring.size/4096);
        sqs[0].sq_ring.vbase = 0;
    }
    if (cqs[0].cq_ring.vbase) {
        __wrapped_pgs_vfree((void*)cqs[0].cq_ring.vbase,cqs[0].cq_ring.size/4096);
        cqs[0].cq_ring.vbase = 0;
    }

    // ---- 3. 释放 MSI-X 向量 ----
    for (uint16_t v = 0; v < max_msix_vectors; v++) {
        msix_vec_free(v);
    }

    // ---- 4. 通知控制器正常关机 ----
    NVMe::controler_ctrl_t ctrl{ .value = head_regs->controller_configuration };
    ctrl.field.SHN = 1;  // 01b = Normal Shutdown Notification
    head_regs->controller_configuration = ctrl.value;

    // 等待 CSTS.SHST = 10b (Shutdown Processing Complete)
    uint32_t remain_ms = 10000;  // 最多等 10 秒
    while (remain_ms > 0) {
        uint32_t csts = head_regs->controller_status;
        uint32_t shst = (csts >> 2) & 0x3;
        if (shst == 2) break;  // 关机完成
        kthread_sleep(1 * 1000);
        remain_ms--;
    }

    // ---- 5. 禁用控制器 ----
    ctrl.value = head_regs->controller_configuration;
    ctrl.field.EN = 0;
    ctrl.field.SHN = 0;
    head_regs->controller_configuration = ctrl.value;

    NVMe::cap_reg_t cap{ .value = head_regs->cap };
    uint32_t to = cap.field.to;
    if (to == 0) to = 10;
    wait_for_ready(head_regs, false, to);

    // ---- 6. 屏蔽 MSI-X ----
    if (msix_table) {
        for (uint32_t i = 0; i < max_msix_vectors; i++) {
            msix_table[i].vector_control |= 1;  // mask
        }
    }

    // ---- 7. 释放堆数据结构 ----
    delete[] sqs;
    delete[] cqs;
    delete[] NSs;
    delete[] IO_CQ_vecs;
    sqs = nullptr;
    cqs = nullptr;
    NSs = nullptr;
    IO_CQ_vecs = nullptr;
    sq_count = 0;
    cq_count = 0;
    NS_count = 0;

    state = NVMe::CTRL_UNINIT;
    return make_success();
}