#include "arch/x86_64/PCIe/base.h"
#include "arch/x86_64/PCIe/prased.h"
#include "util/kshell.h"
#include "util/kshell_commands.h"
#include "util/kout.h"
#include "util/OS_utils.h"

using namespace kio;

static KURD_t make_ok() {
    return {result_code::SUCCESS, 0, module_code::INFRA,
            INFR_LOCATIONS::KSHELL, 0, level_code::INFO, err_domain::CORE_MODULE};
}

// ─────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────

/// ECAM address of a device's config space
static inline volatile void* ecam_addr(ecam_node_t* node, uint8_t bus,
                                       uint8_t dev, uint8_t func) {
    uint64_t off = (bus  - node->start_bus_num) * 32 * 8 * 0x1000
                 + dev  * 8 * 0x1000
                 + func * 0x1000;
    return reinterpret_cast<volatile void*>(node->vminterval.vbase + off);
}

/// Read 16-bit from config space
static uint16_t cfg_read16(volatile void* base, uint8_t reg) {
    return *(volatile uint16_t*)((uintptr_t)base + reg);
}

/// Read 32-bit from config space
static uint32_t cfg_read32(volatile void* base, uint8_t reg) {
    return *(volatile uint32_t*)((uintptr_t)base + reg);
}

/// Parse a decimal number from a substring
static int parse_dec(const char* s, size_t len) {
    int val = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        val = val * 10 + (s[i] - '0');
    }
    return val;
}

/// Parse a hex or decimal number from a substring (0x prefix = hex)
static bool parse_num(const char* s, size_t len, uint64_t* out) {
    if (!out || len == 0) return false;
    uint64_t val = 0; bool hex = false;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = true;
        s += 2; len -= 2;
    }
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')       val = val * (hex ? 16 : 10) + (uint64_t)(c - '0');
        else if (hex && c >= 'a' && c <= 'f') val = val * 16 + (uint64_t)(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') val = val * 16 + (uint64_t)(c - 'A' + 10);
        else return false;
    }
    *out = val;
    return true;
}

/// Parse BDF from a single token:  "1:0.0" or "0:1:0.0" or "1,0,0"
/// Returns false on parse error.
static bool parse_bdf(const token_t& tok,
                      uint16_t* out_seg,
                      uint8_t* out_bus,
                      uint8_t* out_dev,
                      uint8_t* out_func)
{
    const char* s = tok.str;
    size_t len = tok.len;

    // Count colons and dots
    int colons = 0, dots = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ':') colons++;
        if (s[i] == '.') dots++;
    }

    // seg:bus:dev.func  (2 colons, 1 dot)
    if (colons == 2 && dots == 1) {
        // Find positions
        size_t c1 = 0, c2 = 0, dot = 0;
        int found = 0;
        for (size_t i = 0; i < len && found < 3; i++) {
            if (s[i] == ':') { if (found == 0) c1 = i; else c2 = i; found++; }
            if (s[i] == '.') { dot = i; found++; }
        }
        int seg  = parse_dec(s, c1);
        int bus  = parse_dec(s + c1 + 1, c2 - c1 - 1);
        int dev  = parse_dec(s + c2 + 1, dot - c2 - 1);
        int func = parse_dec(s + dot + 1, len - dot - 1);
        if (seg < 0 || bus < 0 || dev < 0 || func < 0) return false;
        *out_seg = (uint16_t)seg; *out_bus = (uint8_t)bus;
        *out_dev = (uint8_t)dev;  *out_func = (uint8_t)func;
        return true;
    }

    // bus:dev.func  (1 colon, 1 dot)
    if (colons == 1 && dots == 1) {
        size_t col = 0, dot = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == ':') col = i;
            if (s[i] == '.') dot = i;
        }
        int bus  = parse_dec(s, col);
        int dev  = parse_dec(s + col + 1, dot - col - 1);
        int func = parse_dec(s + dot + 1, len - dot - 1);
        if (bus < 0 || dev < 0 || func < 0) return false;
        *out_seg = 0; *out_bus = (uint8_t)bus;
        *out_dev = (uint8_t)dev; *out_func = (uint8_t)func;
        return true;
    }

    // bus,dev,func  (2 commas)
    int commas = 0;
    for (size_t i = 0; i < len; i++) if (s[i] == ',') commas++;
    if (commas == 2) {
        size_t c1 = 0, c2 = 0, found = 0;
        for (size_t i = 0; i < len && found < 2; i++) {
            if (s[i] == ',') { if (found == 0) c1 = i; else c2 = i; found++; }
        }
        int bus  = parse_dec(s, c1);
        int dev  = parse_dec(s + c1 + 1, c2 - c1 - 1);
        int func = parse_dec(s + c2 + 1, len - c2 - 1);
        if (bus < 0 || dev < 0 || func < 0) return false;
        *out_seg = 0; *out_bus = (uint8_t)bus;
        *out_dev = (uint8_t)dev; *out_func = (uint8_t)func;
        return true;
    }

    return false;
}

/// Find ECAM node for a given segment.  Returns first node if seg==0 and
/// there is only one segment (shortcut).
static ecam_node_t* find_segment(uint16_t seg) {
    if (!global_container) return nullptr;
    for (auto it = global_container->begin(); it != global_container->end(); ++it) {
        ecam_node_t& node = *it;
        if (node.seg_group_number == seg) return &node;
    }
    return nullptr;
}

static const char* class_name(uint8_t base, uint8_t sub) {
    switch (base) {
    case 0x00: return sub == 0x01 ? "VGA" : "Legacy";
    case 0x01: return "MassStorage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x05: return "Memory";
    case 0x06: return "Bridge";
    case 0x07: return "SimpleComm";
    case 0x08:
        switch (sub) {
        case 0x00: return "PIC";
        case 0x01: return "DMA";
        case 0x02: return "Timer";
        case 0x03: return "RTC";
        case 0x04: return "PCI-HotPlug";
        case 0x80: return "System";
        default:   return "BaseSystem";
        }
    case 0x09: return "Input";
    case 0x0b: return "Processor";
    case 0x0c:
        switch (sub) {
        case 0x00: return "FireWire";
        case 0x01: return "USB";
        case 0x02: return "uC";
        case 0x03: return "SMBus";
        default:   return "SerialBus";
        }
    case 0x0d: return "Wireless";
    case 0x0e: return "I2C";
    case 0x11: return "DataProc";
    default:   return "Unknown";
    }
}

// ─────────────────────────────────────────────────────────────────
//  pcie_segs  —  list ECAM segments
// ─────────────────────────────────────────────────────────────────
KURD_t cmd_pcie_segs(const line_t* line) {
    (void)line;
    if (!global_container) {
        bsp_kout << "[PCIe] ECAM not initialized (global_container == null)\n";
        return make_ok();
    }

    bsp_kout << "ECAM segments:\n";
    for (auto it = global_container->begin(); it != global_container->end(); ++it) {
        ecam_node_t& node = *it;
        bsp_kout << "  seg=" << (uint32_t)node.seg_group_number
                 << "  buses " << (uint32_t)node.start_bus_num
                 << " - " << (uint32_t)(node.start_bus_num + node.bus_count - 1)
                 << "  ECAM vaddr=0x" << HEX << node.vminterval.vbase << DEC
                 << kendl;
    }
    return make_ok();
}

// ─────────────────────────────────────────────────────────────────
//  pcie_BDFs <seg> [--bus=N] [--class=0xXXYY] [--vendor=0xXXXX]
//            [--bridge-only] [--tree]
// ─────────────────────────────────────────────────────────────────
KURD_t cmd_pcie_BDFs(const line_t* line) {
    if (line->token_count < 2) {
        bsp_kout << "Usage: pcie_BDFs <seg> [--bus=N] [--class=0xCCCC] [--vendor=0xVVVV] [--bridge-only] [--tree]\n";
        return make_ok();
    }

    uint16_t seg = 0;
    if (!token_to_uint64(line->tokens[1], (uint64_t*)&seg)) {
        bsp_kout << "[ERROR] invalid seg number\n"; return make_ok();
    }

    ecam_node_t* node = find_segment(seg);
    if (!node) {
        bsp_kout << "[ERROR] segment " << (uint32_t)seg << " not found\n";
        return make_ok();
    }

    // Parse filters
    int     filter_bus    = -1;
    uint32_t filter_class = 0xFFFFFFFF;
    uint16_t filter_vendor = 0xFFFF;
    bool    bridge_only   = false;
    bool    tree_mode     = false;

    for (size_t i = 2; i < line->token_count; i++) {
        const token_t& t = line->tokens[i];
        // --bus=N
        if (token_equals(t, "--bridge-only")) { bridge_only = true; continue; }
        if (token_equals(t, "--tree"))        { tree_mode   = true; continue; }
        // prefix matching for --key=value
        const char* s = t.str;
        size_t sl = t.len;
        if (sl > 6 && strncmp_in_kernel(s, "--bus=", 6) == 0) {
            uint64_t v;
            if (parse_num(s + 6, sl - 6, &v)) filter_bus = (int)v;
        }
        if (sl > 8 && strncmp_in_kernel(s, "--class=", 8) == 0) {
            uint64_t v;
            if (parse_num(s + 8, sl - 8, &v)) filter_class = (uint32_t)v;
        }
        if (sl > 9 && strncmp_in_kernel(s, "--vendor=", 9) == 0) {
            uint64_t v;
            if (parse_num(s + 9, sl - 9, &v)) filter_vendor = (uint16_t)v;
        }
    }

    bsp_kout << "BDF scan for seg=" << (uint32_t)seg << ":\n";

    uint16_t bus_start = (filter_bus >= 0) ? (uint16_t)filter_bus : node->start_bus_num;
    uint16_t bus_end   = (filter_bus >= 0) ? (uint16_t)filter_bus
                                          : (uint16_t)(node->start_bus_num + node->bus_count - 1);

    int count = 0;
    for (uint16_t bus = bus_start; bus <= bus_end; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                auto* base = ecam_addr(node, bus, dev, func);
                uint16_t vid = cfg_read16(base, 0);
                if (vid == 0xFFFF || vid == 0) continue;

                // Only func 0 — check multi-function
                if (func == 0) {
                    uint8_t htype = *(volatile uint8_t*)((uintptr_t)base + 0x0E);
                    if ((htype & PCI_HEADER_TYPE_MULTIFUNC) == 0) {
                        // single-function, skip remaining funcs on this dev
                        // (loop increment handles func++)
                    }
                }

                uint32_t classreg = cfg_read32(base, 0x08);
                uint8_t  base_class = (uint8_t)(classreg >> 24);
                uint8_t  sub_class  = (uint8_t)(classreg >> 16);
                uint16_t devid      = cfg_read16(base, 2);

                // Filters
                if (filter_class != 0xFFFFFFFF) {
                    uint32_t cc = ((uint32_t)base_class << 8) | sub_class;
                    if (cc != filter_class) continue;
                }
                if (filter_vendor != 0xFFFF && vid != filter_vendor) continue;

                uint8_t htype = *(volatile uint8_t*)((uintptr_t)base + 0x0E) & PCI_HEADER_TYPE_MASK;
                if (bridge_only && htype != PCI_HEADER_TYPE_PCI_BRIDGE) continue;

                bsp_kout << "  " << (uint32_t)bus << ":" << (uint32_t)dev << "."
                         << (uint32_t)func
                         << "  0x" << HEX << vid << ":" << devid << DEC
                         << "  " << class_name(base_class, sub_class)
                         << "  (" << HEX << (uint32_t)base_class << ":"
                         << (uint32_t)sub_class << DEC << ")"
                         << "  " << ((htype == PCI_HEADER_TYPE_ENDPOINT) ? "EP"
                                    : (htype == PCI_HEADER_TYPE_PCI_BRIDGE) ? "BR" : "?")
                         << kendl;
                count++;

                // If single-function and not multi-function capable, skip remaining funcs
                if (func == 0) {
                    uint8_t raw_htype = *(volatile uint8_t*)((uintptr_t)base + 0x0E);
                    if ((raw_htype & PCI_HEADER_TYPE_MULTIFUNC) == 0) {
                        // Jump to next device
                        goto next_dev;
                    }
                }
            }
            next_dev:;
        }
    }
    bsp_kout << "  (" << count << " device(s) found)\n";
    return make_ok();
}

// ─────────────────────────────────────────────────────────────────
//  pcie_BDF <BDF> [--bars] [--caps] [--header] [--raw=OFF:LEN]
// ─────────────────────────────────────────────────────────────────
KURD_t cmd_pcie_BDF(const line_t* line) {
    if (line->token_count < 2) {
        bsp_kout << "Usage: pcie_BDF <BDF> [--bars] [--caps] [--header] [--raw=OFF:LEN]\n";
        return make_ok();
    }

    uint16_t seg; uint8_t bus, dev, func;
    if (!parse_bdf(line->tokens[1], &seg, &bus, &dev, &func)) {
        bsp_kout << "[ERROR] invalid BDF format\n"; return make_ok();
    }

    ecam_node_t* node = find_segment(seg);
    if (!node) {
        bsp_kout << "[ERROR] segment " << (uint32_t)seg << " not found\n";
        return make_ok();
    }

    auto* base = ecam_addr(node, bus, dev, func);
    uint16_t vid = cfg_read16(base, 0);
    if (vid == 0xFFFF || vid == 0) {
        bsp_kout << "No device at " << (uint32_t)bus << ":" << (uint32_t)dev << "."
                 << (uint32_t)func << "\n";
        return make_ok();
    }

    // Parse view options
    bool show_bars   = false;
    bool show_caps   = false;
    bool show_header = false;
    int   raw_off    = -1;
    int   raw_len    = -1;
    bool show_all    = true;

    for (size_t i = 2; i < line->token_count; i++) {
        const token_t& t = line->tokens[i];
        if (token_equals(t, "--bars"))   { show_bars   = true; show_all = false; }
        if (token_equals(t, "--caps"))   { show_caps   = true; show_all = false; }
        if (token_equals(t, "--header")) { show_header = true; show_all = false; }
        const char* s = t.str;
        size_t sl = t.len;
        if (sl > 6 && strncmp_in_kernel(s, "--raw=", 6) == 0) {
            show_all = false;
            // Parse OFF:LEN
            int colon = -1;
            for (size_t p = 6; p < sl; p++) { if (s[p] == ':') { colon = (int)p; break; } }
            if (colon > 0) {
                uint64_t ov = 0, lv = 0;
                if (parse_num(s + 6, (size_t)(colon - 6), &ov) &&
                    parse_num(s + colon + 1, sl - colon - 1, &lv)) {
                    raw_off = (int)ov; raw_len = (int)lv;
                }
            }
        }
    }

    if (show_all || show_header) {
        uint32_t classreg = cfg_read32(base, 0x08);
        uint16_t devid    = cfg_read16(base, 2);
        uint8_t  rev      = (uint8_t)(classreg);
        uint8_t  prog_if  = (uint8_t)(classreg >> 8);
        uint8_t  sub_cls  = (uint8_t)(classreg >> 16);
        uint8_t  base_cls = (uint8_t)(classreg >> 24);
        uint16_t cmd      = cfg_read16(base, 4);
        uint16_t status   = cfg_read16(base, 6);

        bsp_kout << (uint32_t)seg << ":" << (uint32_t)bus << ":" << (uint32_t)dev << "."
                 << (uint32_t)func << "\n";
        bsp_kout << "  Vendor:Device  = 0x" << HEX << vid << " : 0x" << devid << DEC << "\n";
        bsp_kout << "  Class          = " << class_name(base_cls, sub_cls)
                 << "  (0x" << HEX << (uint32_t)base_cls << ":0x"
                 << (uint32_t)sub_cls << ":0x" << (uint32_t)prog_if << DEC << ")\n";
        bsp_kout << "  Revision       = 0x" << HEX << (uint32_t)rev << DEC << "\n";
        bsp_kout << "  Command        = 0x" << HEX << cmd << DEC
                 << "  Status = 0x" << HEX << status << DEC << "\n";

        uint8_t htype = *(volatile uint8_t*)((uintptr_t)base + 0x0E) & PCI_HEADER_TYPE_MASK;
        bsp_kout << "  HeaderType     = 0x" << HEX << (uint32_t)htype << DEC
                 << "  (" << ((htype == PCI_HEADER_TYPE_ENDPOINT) ? "Endpoint"
                             : (htype == PCI_HEADER_TYPE_PCI_BRIDGE) ? "PCI-PCI Bridge"
                             : "Other") << ")\n";
        if (htype == PCI_HEADER_TYPE_PCI_BRIDGE) {
            auto* br = (volatile pci_header_pci_bridge_t*)((uintptr_t)base);
            bsp_kout << "  PriBus=" << (uint32_t)br->primary_bus
                     << "  SecBus=" << (uint32_t)br->secondary_bus
                     << "  SubBus=" << (uint32_t)br->subordinate_bus << "\n";
        }
    }

    if (show_all || show_bars) {
        bsp_kout << "  BARs:\n";
        uint8_t htype = *(volatile uint8_t*)((uintptr_t)base + 0x0E) & PCI_HEADER_TYPE_MASK;
        int bar_count = (htype == PCI_HEADER_TYPE_PCI_BRIDGE) ? 2 : 6;
        for (int i = 0; i < bar_count; i++) {
            uint32_t bar_raw = cfg_read32(base, 0x10 + i * 4);
            if (bar_raw == 0) continue;
            bool is_io = (bar_raw & 1) != 0;
            if (is_io) {
                bsp_kout << "    BAR" << i << " = 0x" << HEX
                         << (bar_raw & ~3) << DEC << "  (I/O)\n";
            } else {
                uint8_t mem_type = (bar_raw >> 1) & 3;
                bool prefetch = (bar_raw >> 3) & 1;
                if (mem_type == PCI_BAR_TYPE_64BIT) {
                    // 64-bit: consume next BAR
                    uint32_t high = cfg_read32(base, 0x10 + (i + 1) * 4);
                    uint64_t addr = ((uint64_t)high << 32) | (bar_raw & ~0xF);
                    bsp_kout << "    BAR" << i << " = 0x" << HEX << addr << DEC
                             << "  (64-bit Mem" << (prefetch ? ", Prefetch" : "") << ")\n";
                    i++; // skip consumed
                } else {
                    bsp_kout << "    BAR" << i << " = 0x" << HEX
                             << (bar_raw & ~0xF) << DEC
                             << "  (32-bit Mem" << (prefetch ? ", Prefetch" : "") << ")\n";
                }
            }
        }
    }

    if (show_all || show_caps) {
        bsp_kout << "  Capabilities:\n";
        uint8_t cap_off = *(volatile uint8_t*)((uintptr_t)base + 0x34);
        while (cap_off) {
            uint8_t cap_id  = *(volatile uint8_t*)((uintptr_t)base + cap_off);
            uint8_t cap_next = *(volatile uint8_t*)((uintptr_t)base + cap_off + 1);
            const char* cap_name = "?";
            switch (cap_id) {
            case PCI_CAP_ID_PM:   cap_name = "PM";      break;
            case PCI_CAP_ID_MSI:  cap_name = "MSI";     break;
            case PCI_CAP_ID_EXP:  cap_name = "PCIe";    break;
            case PCI_CAP_ID_MSIX: cap_name = "MSI-X";   break;
            }
            bsp_kout << "    [" << HEX << (uint32_t)cap_off << DEC << "] "
                     << cap_name << " (id=0x" << HEX << (uint32_t)cap_id << DEC << ")"
                     << "  next=0x" << HEX << (uint32_t)cap_next << DEC << "\n";
            if (cap_id == PCI_CAP_ID_MSIX && (show_all || show_caps)) {
                auto* msix = (volatile pci_msix_cap_t*)((uintptr_t)base + cap_off);
                uint16_t msix_ctrl = msix->ctrl;
                uint16_t table_sz = (msix_ctrl & 0x7FF) + 1;
                uint8_t  bir      = (uint8_t)(msix->table_offset & 7);
                uint32_t tbl_off  = msix->table_offset & ~7;
                bsp_kout << "      table_size=" << (uint32_t)table_sz
                         << "  BIR=" << (uint32_t)bir
                         << "  offset=0x" << HEX << tbl_off << DEC
                         << "  enabled=" << ((msix_ctrl >> 15) & 1) << "\n";
            }
            if (cap_id == PCI_CAP_ID_MSI) {
                auto* msi = (volatile pci_msi_32_cap_t*)((uintptr_t)base + cap_off);
                msi_ctl_t ctl;
                uint16_t ctrl_val = *(volatile uint16_t*)&msi->ctrl;
                ctl.value = ctrl_val;
                bsp_kout << "      enable=" << (uint32_t)ctl.msi_enable
                         << "  64bit=" << (uint32_t)ctl.address_64_capable
                         << "  mm=" << (uint32_t)ctl.multi_message_enable
                         << "/" << (uint32_t)ctl.multi_message_capable << "\n";
            }
            cap_off = cap_next;
        }
        if (!*(volatile uint8_t*)((uintptr_t)base + 0x34))
            bsp_kout << "    (none)\n";
    }

    if (raw_off >= 0 && raw_len > 0) {
        bsp_kout << "  Raw dump @" << HEX << (uint32_t)raw_off
                 << " len=" << (uint32_t)raw_len << DEC << ":\n    ";
        for (int i = 0; i < raw_len && (raw_off + i) < 4096; i++) {
            uint8_t b = *(volatile uint8_t*)((uintptr_t)base + raw_off + i);
            bsp_kout << HEX << (uint32_t)b << DEC << " ";
            if ((i & 15) == 15 && i + 1 < raw_len) bsp_kout << "\n    ";
        }
        bsp_kout << "\n";
    }

    return make_ok();
}
