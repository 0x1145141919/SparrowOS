#include <iostream>
#include <vector>

#include "memory/memory_base.h"
#include "util/Ktemplats.h"

static constexpr uint64_t kInsertTotal = 10000;
static constexpr uint64_t kPrintEvery = 1000;

int VM_desc_cmp(const VM_DESC& a, const VM_DESC& b)
{
    if (a.start >= b.end) return 1;
    if (a.end <= b.start) return -1;
    return 0;
}

class kspace_vm_table_t : public Ktemplats::RBTree<VM_DESC, VM_desc_cmp>
{
public:
    using Node = Ktemplats::RBTree<VM_DESC, VM_desc_cmp>::Node;

    void all_node_print()
    {
        std::cout << "[KSPACE_VM_TABLE] inorder traversal:\n";
        auto inorder = [&](auto&& self, Node* n) -> void {
            if (!n) return;
            self(self, n->left);
            const VM_DESC& d = n->data;
            std::cout << "  node: "
                      << "start=0x" << std::hex << d.start
                      << ", end=0x" << d.end
                      << ", phys=0x" << d.phys_start << std::dec
                      << ", map_type=" << static_cast<uint32_t>(d.map_type)
                      << ", color=" << (n->is_red ? "R" : "B")
                      << '\n';
            self(self, n->right);
        };
        inorder(inorder, root);
    }

    Node* search(vaddr_t vaddr)
    {
        Node* cur = root;
        while (cur) {
            if (vaddr < cur->data.start) {
                cur = cur->left;
            } else if (vaddr >= cur->data.end) {
                cur = cur->right;
            } else {
                return cur;
            }
        }
        return nullptr;
    }

    int insert(VM_DESC data)
    {
        return Ktemplats::RBTree<VM_DESC, VM_desc_cmp>::insert(data) ? 0 : -1;
    }

    int remove(vaddr_t vaddr)
    {
        Node* cur = search(vaddr);
        if (!cur) return -1;
        return Ktemplats::RBTree<VM_DESC, VM_desc_cmp>::erase(cur->data) ? 0 : -1;
    }
};

static VM_DESC make_desc(uint64_t index)
{
    constexpr uint64_t kStride = 0x2000;  // 8KB
    constexpr uint64_t kSize = 0x1000;    // 4KB
    constexpr uint64_t kVbase = 0xffff800010000000ull;
    constexpr uint64_t kPbase = 0x10000000ull;

    VM_DESC d{};
    d.start = kVbase + index * kStride;
    d.end = d.start + kSize;
    d.map_type = VM_DESC::MAP_PHYSICAL;
    d.phys_start = kPbase + index * kSize;
    d.access = KSPACE_RW_ACCESS;
    d.committed_full = 1;
    d.is_vaddr_alloced = 0;
    d.is_out_bound_protective = 0;
    d.SEG_SIZE_ONLY_UES_IN_BASIC_SEG = 0;
    return d;
}

static bool should_print(uint64_t op_count, uint64_t total, uint64_t print_every)
{
    if (print_every == 0) return false;
    return (op_count % print_every == 0) || (op_count == total);
}

int main()
{
    constexpr uint64_t insert_total = kInsertTotal;
    constexpr uint64_t print_every = kPrintEvery;

    std::cout << "kspace_vm_table insert/remove test\n"
              << "insert_total=" << insert_total
              << ", print_every=" << print_every << '\n';

    kspace_vm_table_t table;
    std::vector<vaddr_t> inserted_starts;
    inserted_starts.reserve(insert_total);

    std::cout << "[Phase 1] insert begin\n";
    for (uint64_t i = 0; i < insert_total; ++i) {
        VM_DESC d = make_desc(i);
        if (table.insert(d) != 0) {
            std::cerr << "insert failed at i=" << i << '\n';
            return 1;
        }
        inserted_starts.push_back(d.start);

        const vaddr_t probe = d.start + ((d.end - d.start) >> 1);
        if (table.search(probe) == nullptr) {
            std::cerr << "search verify failed after insert, i=" << i << '\n';
            return 1;
        }

        if (should_print(i + 1, insert_total, print_every)) {
            std::cout << "[insert] finished " << (i + 1) << "/" << insert_total << '\n';
            table.all_node_print();
        }
    }
    std::cout << "[Phase 1] insert done, node_count=" << table.size() << '\n';

    std::cout << "[Phase 2] remove begin\n";
    for (uint64_t removed = 0; removed < insert_total; ++removed) {
        const uint64_t idx = insert_total - 1 - removed;
        const vaddr_t start = inserted_starts[idx];
        if (table.remove(start) != 0) {
            std::cerr << "remove failed at i=" << removed << ", vaddr=0x" << std::hex << start
                      << std::dec << '\n';
            return 1;
        }

        if (should_print(removed + 1, insert_total, print_every)) {
            std::cout << "[remove] finished " << (removed + 1) << "/" << insert_total << '\n';
            table.all_node_print();
        }
    }
    std::cout << "[Phase 2] remove done, node_count=" << table.size() << '\n';

    if (!table.empty()) {
        std::cerr << "test failed: table is not empty after all removes\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}
