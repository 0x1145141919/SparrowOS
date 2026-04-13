#include "arch/x86_64/PCIe/prased.h"
#include "util/kout.h"
ecams_container_t*global_container;
void pcie_bus_scan(ecam_node_t*node,uint8_t bus_num, uint32_t depth);
void pcie_an_ecam_print(vaddr_t vbase, uint32_t depth){//vbase为直接的底
    if(vbase == 0) return;
    
    // 生成缩进字符串
    for(uint32_t i = 0; i < depth; i++) {
        bsp_kout << "    ";
    }
    
    // 读取header type字段 (偏移0x0E)
    uint8_t header_type = *(volatile uint8_t*)(vbase + 0x0E);
    bool is_multifunction = (header_type & PCI_HEADER_TYPE_MULTIFUNC) != 0;
    uint8_t type = header_type & PCI_HEADER_TYPE_MASK;
    
    bsp_kout << "[PCIe] ECAM Base: 0x";
    bsp_kout.shift_hex();
    bsp_kout << reinterpret_cast<uint64_t>(vbase) << "\n";
    for(uint32_t i = 0; i < depth; i++) {
        bsp_kout << "    ";
    }
    bsp_kout << "[PCIe] Header Type: 0x" << static_cast<uint32_t>(type);
    if(is_multifunction) {
        bsp_kout << " (Multi-function)";
    }
    bsp_kout << "\n";
    
    if(type == PCI_HEADER_TYPE_ENDPOINT) {
        // Type 0: 端点设备
        pci_header_endpoint_t* header = reinterpret_cast<pci_header_endpoint_t*>(vbase);
        
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Device Type: Endpoint (Type 0)\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Vendor ID: 0x";
        bsp_kout.shift_hex();
        bsp_kout << header->vendor_id << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Device ID: 0x" << header->device_id << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Command: 0x" << header->command.value << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Status: 0x" << header->status.value << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Revision ID: 0x" << static_cast<uint32_t>(header->revision_id) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Class Code: 0x";
        bsp_kout.shift_hex();
        bsp_kout << static_cast<uint32_t>(header->base_class) << ":"
                     << static_cast<uint32_t>(header->sub_class) << ":"
                     << static_cast<uint32_t>(header->prog_if) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Cache Line Size: " << static_cast<uint32_t>(header->cache_line_size) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Latency Timer: " << static_cast<uint32_t>(header->latency_timer) << "\n";
        
        // 打印BARs
        for(int i = 0; i < 6; i++) {
            if(header->bars[i] != 0) {
                for(uint32_t j = 0; j < depth; j++) {
                    bsp_kout << "    ";
                }
                bsp_kout << "[PCIe] BAR" << i << ": 0x";
                bsp_kout.shift_hex();
                
                // 检查是否为64位BAR
                if((header->bars[i] & 0x1) == 0) {  // 内存空间
                    uint8_t bar_type = (header->bars[i] >> 1) & 0x3;
                    if(bar_type == PCI_BAR_TYPE_64BIT) {
                        // 64位BAR: 需要组合下一个BAR
                        if(i + 1 < 6 && header->bars[i + 1] != 0) {
                            uint64_t full_addr = static_cast<uint64_t>(header->bars[i]) | 
                                               (static_cast<uint64_t>(header->bars[i + 1]) << 32);
                            bsp_kout << full_addr << " (64-bit Memory, Full Address)\n";
                            i++;  // 跳过下一个BAR,因为它已经被使用了
                        } else {
                            bsp_kout << header->bars[i] << " (64-bit Memory, Incomplete)\n";
                        }
                    } else {
                        bsp_kout << header->bars[i] << " (32-bit Memory)\n";
                    }
                } else {
                    bsp_kout << header->bars[i] << " (I/O Space)\n";
                }
            }
        }
        
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Subsystem Vendor ID: 0x" << header->subsystem_vendor_id << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Subsystem ID: 0x" << header->subsystem_id << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Expansion ROM Base: 0x" << header->expansion_rom_base << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Capabilities Pointer: 0x" << static_cast<uint32_t>(header->capabilities_ptr) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Interrupt Line: " << static_cast<uint32_t>(header->interrupt_line) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Interrupt Pin: " << static_cast<uint32_t>(header->interrupt_pin) << "\n";
        
    } else if(type == PCI_HEADER_TYPE_PCI_BRIDGE) {
        // Type 1: PCI-PCI桥
        pci_header_pci_bridge_t* header = reinterpret_cast<pci_header_pci_bridge_t*>(vbase);
        
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Device Type: PCI-PCI Bridge (Type 1)\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Vendor ID: 0x";
        bsp_kout.shift_hex();
        bsp_kout << header->vendor_id << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Device ID: 0x" << header->device_id << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Command: 0x" << header->command << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Status: 0x" << header->status << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Revision ID: 0x" << static_cast<uint32_t>(header->revision_id) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Class Code: 0x";
        bsp_kout.shift_hex();
        bsp_kout << static_cast<uint32_t>(header->base_class) << ":"
                     << static_cast<uint32_t>(header->sub_class) << ":"
                     << static_cast<uint32_t>(header->prog_if) << "\n";
        
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Primary Bus: " << static_cast<uint32_t>(header->primary_bus) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Secondary Bus: " << static_cast<uint32_t>(header->secondary_bus) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Subordinate Bus: " << static_cast<uint32_t>(header->subordinate_bus) << "\n";
        
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] I/O Base: 0x" << static_cast<uint32_t>(header->io_base) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] I/O Limit: 0x" << static_cast<uint32_t>(header->io_limit) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Memory Base: 0x" << header->memory_base<<16 << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Memory Limit: 0x" << header->memory_limit << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Prefetchable Memory Base: 0x" << header->prefetchable_memory_base << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Prefetchable Memory Limit: 0x" << header->prefetchable_memory_limit << "\n";
        
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Interrupt Line: " << static_cast<uint32_t>(header->interrupt_line) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Interrupt Pin: " << static_cast<uint32_t>(header->interrupt_pin) << "\n";
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Bridge Control: 0x" << header->bridge_control << "\n";
        
    } else {
        for(uint32_t i = 0; i < depth; i++) {
            bsp_kout << "    ";
        }
        bsp_kout << "[PCIe] Unknown Header Type: 0x" << static_cast<uint32_t>(type) << "\n";
    }
    
    bsp_kout.shift_dec(); // 恢复十进制输出
}

void pcie_text_praser(ecam_node_t*node)
{
    if(node==nullptr)return;
    if(node->bus_count==0)return;
    if(node->start_bus_num!=0)return;
    //默认从0开始扫描
    pcie_bus_scan(node, 0, 0);
    
}
void pcie_bus_scan(ecam_node_t* node, uint8_t bus_num, uint32_t depth)
{
    if(node == nullptr) return;
    if(bus_num < node->start_bus_num || bus_num >= node->start_bus_num + node->bus_count) return;
    
    vaddr_t vbase = node->vminterval.vbase + ((bus_num - node->start_bus_num) * (ECAM_size << (func_bit_width + device_bit_num)));
    
    for(uint8_t device = 0; device < 32; device++) {
        for(uint8_t func = 0; func < 8; func++) {
            vaddr_t header = vbase + (device * (ECAM_size << func_bit_width)) + (func * ECAM_size);
            
            // 检查设备是否存在
            if(*(uint16_t*)header == 0xFFFF) {
                if(func == 0) break;  // function 0 不存在，跳出设备循环
                continue;              // function 0 存在但其他 func 不存在
            }
            
            // 打印BDF信息(带缩进)
            for(uint32_t i = 0; i < depth; i++) {
                bsp_kout << "    ";
            }
            bsp_kout << "[PCIe] BUS: " << static_cast<uint32_t>(bus_num) 
                     << " DEVICE: " << static_cast<uint32_t>(device) 
                     << " FUNC: " << static_cast<uint32_t>(func) << "\n";
            pcie_an_ecam_print(header, depth + 1);
            
            // 读取 Header Type
            uint8_t header_type = *(uint8_t*)(header + 0x0e);
            bool is_multi_function = (header_type & 0x80) != 0;  // bit7 表示多功能设备
            
            // 检查是否是 PCI-to-PCI 桥 (Header Type 0x01)
            if((header_type & 0x7f) == 0x01) {  // 去掉多功能标志位后比较
                pci_header_pci_bridge_t* bridge_header = reinterpret_cast<pci_header_pci_bridge_t*>(header);
                uint8_t start_bus = bridge_header->secondary_bus;
                uint8_t end_bus = bridge_header->subordinate_bus;
                
                // 安全检查
                if(start_bus == 0 || end_bus == 0) {
                    for(uint32_t i = 0; i < depth + 1; i++) {
                        bsp_kout << "    ";
                    }
                    bsp_kout << "[WARN] Invalid bus range: start=" << start_bus << " end=" << end_bus << "\n";
                    continue;
                }
                
                if(start_bus > end_bus) {
                    for(uint32_t i = 0; i < depth + 1; i++) {
                        bsp_kout << "    ";
                    }
                    bsp_kout << "[WARN] start_bus > end_bus: " << start_bus << " > " << end_bus << "\n";
                    continue;
                }
                
                // 防止无限递归：检查是否超出 node 范围
                if(start_bus < node->start_bus_num || end_bus >= node->start_bus_num + node->bus_count) {
                    for(uint32_t i = 0; i < depth + 1; i++) {
                        bsp_kout << "    ";
                    }
                    bsp_kout << "[WARN] Bus range out of ECAM range\n";
                    continue;
                }
                
                // 防止重复扫描：可以添加一个已扫描总线号的集合
                // 这里简单检查如果 start_bus == bus_num 则跳过
                if(start_bus <= bus_num && end_bus >= bus_num) {
                    for(uint32_t i = 0; i < depth + 1; i++) {
                        bsp_kout << "    ";
                    }
                    bsp_kout << "[WARN] Bridge loops back to current bus: " << bus_num << "\n";
                    continue;
                }
                
                for(uint32_t i = 0; i < depth + 1; i++) {
                    bsp_kout << "    ";
                }
                bsp_kout << "[PCIe] Bridge found, scanning buses " << start_bus << " -> " << end_bus << "\n";
                
                for(uint8_t bus = start_bus; bus <= end_bus; bus++) {
                    pcie_bus_scan(node, bus, depth + 1);
                }
            }
            
            // 如果不是多功能设备且 func == 0，跳出 func 循环
            if(!is_multi_function && func == 0) {
                break;  // 单功能设备，只扫描 function 0
            }
        }
    }
}
void pcie_text_praser()
{
    if(global_container == nullptr) {
        bsp_kout << "[PCIe] global_container is null\n";
        return;
    }
    
    bsp_kout << "[PCIe] Starting PCIe enumeration...\n";
    
    // 遍历所有ECAM节点
    for(auto it = global_container->begin(); it != global_container->end(); ++it) {
        ecam_node_t& node = *it;
        
        bsp_kout << "[PCIe] Processing ECAM segment group: " 
                 << node.seg_group_number 
                 << ", Bus range: " 
                 << static_cast<uint32_t>(node.start_bus_num) 
                 << " - " 
                 << static_cast<uint32_t>(node.start_bus_num + node.bus_count - 1)
                 << "\n";
        
        // 调用有参版本的pcie_text_praser处理每个节点
        pcie_text_praser(&node);
    }
    
    bsp_kout << "[PCIe] PCIe enumeration completed\n";
}
ecams_container_t::ecams_container_t(MCFG_Table* mcfg){
    if (mcfg == nullptr) return;
    if (mcfg->Header.Length < sizeof(MCFG_Table)) return;

    const uint64_t payload_bytes = static_cast<uint64_t>(mcfg->Header.Length) - sizeof(MCFG_Table);
    const uint64_t entry_count = payload_bytes / sizeof(ACPI_MCFG::MCFG_Table_Entry);
    auto* entries = reinterpret_cast<ACPI_MCFG::MCFG_Table_Entry*>(
        reinterpret_cast<uint8_t*>(mcfg) + sizeof(MCFG_Table)
    );

    for (uint64_t i = 0; i < entry_count; ++i) {
        const ACPI_MCFG::MCFG_Table_Entry& e = entries[i];
        if (e.EndBusNumber < e.StartBusNumber) continue;

        ecam_node_t node = {};
        node.seg_group_number = e.PciSegmentGroupNumber;
        node.start_bus_num = e.StartBusNumber;
        node.bus_count = e.EndBusNumber - e.StartBusNumber + 1;

        // 每个 bus 的 ECAM 配置空间固定 1MB（32 devices * 8 funcs * 4KB）
        node.vminterval.pbase = e.BaseAddress;
        node.vminterval.size = static_cast<uint64_t>(node.bus_count) * 0x100000ULL;
        node.vminterval.access = ecam_mem_access;
        node.vminterval.vbase = 0;

        KURD_t kurd = KURD_t();
        vaddr_t vbase = phyaddr_direct_map(&node.vminterval, &kurd);
        if (vbase == 0 || error_kurd(kurd)) {
            continue;
        }
        node.vminterval.vbase = vbase;
        (void)this->push_back(node);
    }
}
