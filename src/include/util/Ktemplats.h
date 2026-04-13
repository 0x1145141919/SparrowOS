// kernel_sparse_table.hpp
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "bitmap.h"
#include "util/OS_utils.h"
#ifdef KERNEL_MODE
#include "memory/kpoolmemmgr.h"
#endif
#ifdef USER_MODE
#include <new>
#endif

namespace Ktemplats {


/*
 * Kernel-owned bitmap wrapper
 * ---------------------------
 *  - RAII construction / destruction
 *  - Uses default_kernel_allocator
 *  - No std dependency
 *  - Intended for embedding in kernel structures
 */
class kernel_bitmap : public bitmap_t
{
public:
    kernel_bitmap(uint64_t bit_count);
    ~kernel_bitmap();
    kernel_bitmap(const kernel_bitmap&) = delete;
    kernel_bitmap& operator=(const kernel_bitmap&) = delete;
};

/*
 * Kernel generic sparse two-level table
 * -----------------------------------
 * Design goals:
 *  - No dependency on std / STL
 *  - Lazy allocation of sub-tables
 *  - Exposes a flat logical index space
 *  - Suitable for kernel subsystems (physmem, PID, FD, VMA, etc.)
 *
 * Index layout:
 *   [ high_entries_count_log2 | sub_entries_log2 ]
 *
 * Logical index = (high_index << sub_entries_log2) | low_index
 */
/*ValueT里面若要使用类请使用指针，自行管理生命周期，否则后果自负
*/
template<
    typename IndexT,
    typename ValueT,
    uint32_t high_entries_count_log2,
    uint32_t sub_entries_log2
>
class sparse_table_2level_no_OBJCONTENT
{
    static_assert(high_entries_count_log2 > 0, "invalid high_entries_count_log2");
    static_assert(sub_entries_log2 > 0, "invalid sub_entries_log2");

public:
    static constexpr uint32_t HIGH_ENTRIES = 1u << high_entries_count_log2;
    static constexpr uint32_t SUB_ENTRIES  = 1u << sub_entries_log2;
    static constexpr uint32_t LOW_MASK     = SUB_ENTRIES - 1;

    struct sub_table {
    kernel_bitmap used_map;
    ValueT entries[SUB_ENTRIES];
    sub_table():used_map(SUB_ENTRIES){}
    };

    struct root_entry {
        sub_table* table = nullptr;
        uint32_t   flags = 0;
        uint32_t   refcnt = 0;
    };

protected:
    root_entry m_root[HIGH_ENTRIES];

    static inline uint32_t high_index(IndexT idx) {
        return (uint32_t)(idx >> sub_entries_log2);
    }

    static inline uint32_t low_index(IndexT idx) {
        return (uint32_t)(idx & LOW_MASK);
    }
public:
    sparse_table_2level_no_OBJCONTENT() {
    }

    ~sparse_table_2level_no_OBJCONTENT() {
        for (uint32_t i = 0; i < HIGH_ENTRIES; ++i) {
            if (m_root[i].table)
                delete m_root[i].table;
        }
    }


    virtual int enable_idx(IndexT idx){
        uint32_t hi = high_index(idx);
        if (hi >= HIGH_ENTRIES)return OS_BAD_FUNCTION;
        root_entry& e = m_root[hi];
        if (!e.table){
            e.table=new sub_table();
            ksetmem_8(e.table->entries, 0, sizeof(ValueT)*SUB_ENTRIES);
        }
        uint32_t low = low_index(idx);
        if (e.table->used_map.bit_get(low)==false){
            e.table->used_map.bit_set(low,true);
            e.refcnt++;
        }
            return OS_SUCCESS;
    }
    virtual ValueT* get(IndexT idx) {
        uint32_t hi = high_index(idx);
        if (hi >= HIGH_ENTRIES)
            return nullptr;

        root_entry& e = m_root[hi];

        if (!e.table||e.table->used_map.bit_get(low_index(idx))==false)
            return nullptr;
        return &e.table->entries[low_index(idx)];
    }

    virtual int release(IndexT idx) {
        uint32_t hi = high_index(idx);
        if (hi >= HIGH_ENTRIES)
            return OS_OUT_OF_RANGE;

        root_entry& e = m_root[hi];
        if (!e.table)
            return OS_INVALID_ADDRESS;

        if (e.table->used_map.bit_get(low_index(idx))==true)
            {
                e.table->used_map.bit_set(low_index(idx),false);
                --e.refcnt;
            }
        else return OS_RERELEASE_ERROR;
        if (e.refcnt == 0) {
            delete e.table;
            e.table = nullptr;
            e.flags = 0;
            return OS_SUCCESS;
        }
        return OS_UNREACHABLE_CODE;
    }
};
/*
 * Kernel generic doubly-linked list
 * --------------------------------
 * Design goals:
 *  - No dependency on std / STL
 *  - Intrusive-free (node owned by list)
 *  - Uses allocator from Ktemplats
 *  - Suitable for kernel object lists (VM_DESC, tasks, devices, etc.)
 */
template<typename T>
class list_doubly
{
protected:
    struct node {
        node* prev;
        node* next;
        T     value;
    };

    node* m_head = nullptr;
    node* m_tail = nullptr;
    size_t m_size = 0;

    node* alloc_node(const T& val) {
        void* mem = new node();
        if (!mem) return nullptr;
        node* n = reinterpret_cast<node*>(mem);
        n->prev = nullptr;
        n->next = nullptr;
        new (&n->value) T(val);
        return n;
    }

    void free_node(node* n) {
        if (!n) return;
        n->value.~T();
        delete n;
    }

public:
    list_doubly() = default;

    ~list_doubly() {
        clear();
    }

    size_t size() const {
        return m_size;
    }

    bool empty() const {
        return m_size == 0;
    }

    T* front() {
        return m_head ? &m_head->value : nullptr;
    }

    T* back() {
        return m_tail ? &m_tail->value : nullptr;
    }

    const T* front() const {
        return m_head ? &m_head->value : nullptr;
    }

    const T* back() const {
        return m_tail ? &m_tail->value : nullptr;
    }

    bool push_front(const T& val) {
        node* n = alloc_node(val);
        if (!n) return false;

        n->next = m_head;
        if (m_head)
            m_head->prev = n;
        else
            m_tail = n;

        m_head = n;
        ++m_size;
        return true;
    }

    bool push_back(const T& val) {
        node* n = alloc_node(val);
        if (!n) return false;

        n->prev = m_tail;
        if (m_tail)
            m_tail->next = n;
        else
            m_head = n;

        m_tail = n;
        ++m_size;
        return true;
    }

    void pop_front() {
        if (!m_head) return;
        node* n = m_head;
        m_head = n->next;
        if (m_head)
            m_head->prev = nullptr;
        else
            m_tail = nullptr;
        free_node(n);
        --m_size;
    }
    T pop_front_value()
    {
        if (!m_head) return;
        node* n = m_head;
        m_head = n->next;
        if (m_head)
            m_head->prev = nullptr;
        else
            m_tail = nullptr;
        T val = n->value;
        free_node(n);
        --m_size;
        return val;
    }
    void pop_back() {
        if (!m_tail) return;
        node* n = m_tail;
        m_tail = n->prev;
        if (m_tail)
            m_tail->next = nullptr;
        else
            m_head = nullptr;
        free_node(n);
        --m_size;
    }
    T pop_back_value()
    {
        if (!m_tail) return;
        node* n = m_tail;
        m_tail = n->prev;
        if (m_tail)
            m_tail->next = nullptr;
        else
            m_head = nullptr;
        T val = n->value;
        free_node(n);
        --m_size;
        return val;
    }
    void clear() {
        node* cur = m_head;
        while (cur) {
            node* next = cur->next;
            free_node(cur);
            cur = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
        m_size = 0;
    }

    class iterator {
        node* cur;
    public:
        explicit iterator(node* n) : cur(n) {}
        iterator& operator++() {
            if (cur) cur = cur->next;
            return *this;
        }
        bool operator!=(const iterator& other) const {
            return cur != other.cur;
        }
        T& operator*() {
            return cur->value;
        }
    };

    iterator begin() { return iterator(m_head); }
    iterator end()   { return iterator(nullptr); }
};
template <typename T, int (*Compare)(const T&, const T&) = nullptr>
class RBTree {//int (*Compare)(const T& a, const T& b) a>b则大于0
    //a==b则等于0
    //a<b则小于0
public:
    struct Node {
        Node* parent;
        Node* left;
        Node* right;
        bool is_red;
        T data;  // 存储实际数据
        
        // 可以加扩展点
        Node(const T& val) : data(val), parent(nullptr), 
                              left(nullptr), right(nullptr), is_red(true) {}
    };
protected:
    Node* root;
    static Node* rotate_left(Node* old_root) { 
        Node*new_root=old_root->right;
        Node*old_rl=old_root->right->left;
        Node*old_root_parent=old_root->parent;
        new_root->left=old_root;
        old_root->right=old_rl;
        if(old_rl){
            old_rl->parent=old_root;
        }
        new_root->parent=old_root_parent;
        old_root->parent=new_root;
        return new_root;
    }
    static Node* rotate_right(Node* old_root) {
        Node*new_root=old_root->left;
        Node*old_lr=old_root->left->right;
        Node*old_root_parent=old_root->parent;
        new_root->right=old_root;
        old_root->parent=new_root;
        old_root->left=old_lr;
        if(old_lr){
            old_lr->parent=old_root;
        }
        new_root->parent=old_root_parent;
        
        return new_root;
    }
    virtual Node* allocate_node(const T& val)
    {
        return new Node(val);
    }
    virtual void deallocate_node(Node* node)
    {
        delete node;
    }
    Node* bst_insert(const T& val, Node*& subroot, Node* parent_node) {
        if (subroot == nullptr) {
            subroot = allocate_node(val);
            subroot->parent = parent_node;  // 设置父节点指针
            return subroot;
        }
        
        int cmp = Compare(val, subroot->data);
        if (cmp == 0) return nullptr;  // 重复值
        else if (cmp < 0) {
            return bst_insert(val, subroot->left, subroot);
        }
        else {
            return bst_insert(val, subroot->right, subroot);
        }
    }
    void color_fix(Node* node) {
        if(node->is_red==false)return;
        if(node==root){node->is_red=false;return;}
        Node*parent=node->parent;
        if(parent==root){root->is_red=false;return;}
        if(parent->is_red==false)return;  
        Node*grandparent=parent->parent;
        Node*uncle=grandparent->left==parent?grandparent->right:grandparent->left;
        bool is_uncle_red=uncle?uncle->is_red:false;
        if(is_uncle_red){
            parent->is_red=false;
            uncle->is_red=false;
            grandparent->is_red=true;
            color_fix(grandparent);
        }else{
            enum four_case{
                LEFT_LEFT,
                LEFT_RIGHT,
                RIGHT_RIGHT,
                RIGHT_LEFT
            };
            Node*&new_root_ref=[&]()->Node*&{
                    if(grandparent==root)return root;
                    else{
                        if(grandparent==grandparent->parent->left)return grandparent->parent->left;
                        else return grandparent->parent->right;
                    }
                }();
            four_case case_=grandparent->left==parent?
                (parent->left==node?LEFT_LEFT:LEFT_RIGHT):
                (parent->right==node?RIGHT_RIGHT:RIGHT_LEFT);
            switch (case_)
            {
            case LEFT_LEFT:
                new_root_ref=rotate_right(grandparent);
                parent->is_red=false;
                break;
            case RIGHT_RIGHT:
                new_root_ref=rotate_left(grandparent);
                parent->is_red=false;
                break;
            case LEFT_RIGHT:
                grandparent->left=rotate_left(parent);
                new_root_ref=rotate_right(grandparent);
                node->is_red=false;
                break;
            case RIGHT_LEFT:
                grandparent->right=rotate_right(parent);
                new_root_ref=rotate_left(grandparent);
                node->is_red=false;
                break;
            }
            grandparent->is_red=true;
            
        }
    }
    static Node* subtree_min(Node* node)
    {
        Node* cur = node;
        while (cur && cur->left) cur = cur->left;
        return cur;
    }
public:
    RBTree() : root(nullptr) {}
    // 清晰的返回值
    bool insert(const T value) // 返回是否成功插入（如已存在返回false）
    {
        if(root==nullptr){
            root=allocate_node(value);
            root->is_red=false;
            return true;
        }
        Node* new_node = bst_insert(value, root, nullptr);
        if(new_node==nullptr)return false;
        color_fix(new_node);
        return true;
    }
private:
    Node* find_node(const T& value) const
    {
        Node* cur = root;
        while (cur) {
            int cmp = Compare(value, cur->data);
            if (cmp == 0) return cur;
            cur = (cmp < 0) ? cur->left : cur->right;
        }
        return nullptr;
    }
    
    Node*& parent_link(Node* node)
    {
        if (node->parent == nullptr) return root;
        if (node->parent->left == node) return node->parent->left;
        return node->parent->right;
    }
    void transplant(Node* old_node, Node* new_node)
    {
        if (old_node->parent == nullptr) {
            root = new_node;
        } else if (old_node == old_node->parent->left) {
            old_node->parent->left = new_node;
        } else {
            old_node->parent->right = new_node;
        }
        if (new_node) {
            new_node->parent = old_node->parent;
        }
    }
    void erase_fixup(Node* fixup_node, Node* fixup_parent)
    {
        auto is_red = [](Node* n) { return n && n->is_red; };
        auto is_black = [&](Node* n) { return !is_red(n); };

        while (fixup_node != root && is_black(fixup_node)) {
            if (fixup_parent == nullptr) break;
            if (fixup_node == fixup_parent->left) {
                Node* sibling = fixup_parent->right;
                if (is_red(sibling)) {
                    sibling->is_red = false;
                    fixup_parent->is_red = true;
                    Node*& parent_slot = parent_link(fixup_parent);
                    Node* new_subroot = rotate_left(fixup_parent);
                    parent_slot = new_subroot;
                    sibling = fixup_parent->right;
                }
                if (!sibling || (is_black(sibling->left) && is_black(sibling->right))) {
                    if (sibling) sibling->is_red = true;
                    fixup_node = fixup_parent;
                    fixup_parent = fixup_node->parent;
                } else {
                    if (is_black(sibling->right)) {
                        if (sibling->left) sibling->left->is_red = false;
                        sibling->is_red = true;
                        Node*& parent_slot = parent_link(sibling);
                        Node* new_subroot = rotate_right(sibling);
                        parent_slot = new_subroot;
                        sibling = fixup_parent->right;
                    }
                    sibling->is_red = fixup_parent->is_red;
                    fixup_parent->is_red = false;
                    if (sibling->right) sibling->right->is_red = false;
                    Node*& parent_slot = parent_link(fixup_parent);
                    Node* new_subroot = rotate_left(fixup_parent);
                    parent_slot = new_subroot;
                    fixup_node = root;
                }
            } else {
                Node* sibling = fixup_parent->left;
                if (is_red(sibling)) {
                    sibling->is_red = false;
                    fixup_parent->is_red = true;
                    Node*& parent_slot = parent_link(fixup_parent);
                    Node* new_subroot = rotate_right(fixup_parent);
                    parent_slot = new_subroot;
                    sibling = fixup_parent->left;
                }
                if (!sibling || (is_black(sibling->left) && is_black(sibling->right))) {
                    if (sibling) sibling->is_red = true;
                    fixup_node = fixup_parent;
                    fixup_parent = fixup_node->parent;
                } else {
                    if (is_black(sibling->left)) {
                        if (sibling->right) sibling->right->is_red = false;
                        sibling->is_red = true;
                        Node*& parent_slot = parent_link(sibling);
                        Node* new_subroot = rotate_left(sibling);
                        parent_slot = new_subroot;
                        sibling = fixup_parent->left;
                    }
                    sibling->is_red = fixup_parent->is_red;
                    fixup_parent->is_red = false;
                    if (sibling->left) sibling->left->is_red = false;
                    Node*& parent_slot = parent_link(fixup_parent);
                    Node* new_subroot = rotate_right(fixup_parent);
                    parent_slot = new_subroot;
                    fixup_node = root;
                }
            }
        }
        if (fixup_node) {
            fixup_node->is_red = false;
        }
    }
public:
    bool erase(const T& value)   // 返回是否成功删除
    {
        Node* target = find_node(value);
        if (!target) return false;

        Node* node_to_remove = target;
        bool removed_was_red = node_to_remove->is_red;
        Node* fixup_node = nullptr;
        Node* fixup_parent = nullptr;

        if (target->left == nullptr) {
            fixup_node = target->right;
            fixup_parent = target->parent;
            transplant(target, target->right);
        } else if (target->right == nullptr) {
            fixup_node = target->left;
            fixup_parent = target->parent;
            transplant(target, target->left);
        } else {
            Node* successor = subtree_min(target->right);
            removed_was_red = successor->is_red;
            fixup_node = successor->right;
            if (successor->parent == target) {
                fixup_parent = successor;
                if (fixup_node) fixup_node->parent = successor;
            } else {
                fixup_parent = successor->parent;
                transplant(successor, successor->right);
                successor->right = target->right;
                successor->right->parent = successor;
            }
            transplant(target, successor);
            successor->left = target->left;
            successor->left->parent = successor;
            successor->is_red = target->is_red;
        }

        deallocate_node(target);
        if (!removed_was_red) {
            erase_fixup(fixup_node, fixup_parent);
        }
        return true;
    }
    
    // 安全的查找接口
    bool contains(const T& value) const  // 检查是否存在
    {
        return find_node(value) != nullptr;
    }
    T* find(const T& value)              // 返回指针，可为nullptr
    {
        Node* node = find_node(value);
        return node ? &node->data : nullptr;
    }
    const T* find(const T& value) const  // const版本
    {
        Node* node = find_node(value);
        return node ? &node->data : nullptr;
    }
    
    // 基本操作
    size_t size() const
    {
        size_t count = 0;
        auto inorder = [&](auto&& self, Node* n) -> void {
            if (!n) return;
            self(self, n->left);
            ++count;
            self(self, n->right);
        };
        inorder(inorder, root);
        return count;
    }
    bool empty() const
    {
        return root == nullptr;
    }
    void clear()
    {
        auto postorder = [&](auto&& self, Node* n) -> void {
            if (!n) return;
            self(self, n->left);
            self(self, n->right);
            deallocate_node(n);
        };
        postorder(postorder, root);
        root = nullptr;
    }
    
private:
    // ...
};
} // namespace Ktemplats
