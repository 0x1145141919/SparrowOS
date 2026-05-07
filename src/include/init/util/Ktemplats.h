// kernel_sparse_table.hpp
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../include/util/bitmap.h"
#include "util/OS_utils.h"
#ifdef KERNEL_MODE
#include "init/heap_alloc.h"
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

private:
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


    int enable_idx(IndexT idx){
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
    ValueT* get(IndexT idx) {
        uint32_t hi = high_index(idx);
        if (hi >= HIGH_ENTRIES)
            return nullptr;

        root_entry& e = m_root[hi];

        if (!e.table||e.table->used_map.bit_get(low_index(idx))==false)
            return nullptr;
        return &e.table->entries[low_index(idx)];
    }

    int release(IndexT idx) {
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
        bool operator==(const iterator& other) const {
            return cur == other.cur;
        }
        T& operator*() {
            return cur->value;
        }
        node* get_node() const { return cur; }
    };

    iterator begin() { return iterator(m_head); }
    iterator end()   { return iterator(nullptr); }
    
    /**
     * @brief 删除指定位置的元素
     * @param pos 要删除的元素的迭代器
     * @return 指向被删除元素后继的迭代器
     */
    iterator erase(iterator pos) {
        if (!pos.get_node() || m_size == 0) return end();
        
        node* n = pos.get_node();
        iterator next_iter(n->next);
        
        // 处理头节点
        if (n == m_head) {
            m_head = n->next;
            if (m_head) {
                m_head->prev = nullptr;
            } else {
                m_tail = nullptr;
            }
        }
        // 处理中间或尾部节点
        else {
            n->prev->next = n->next;
            if (n->next) {
                n->next->prev = n->prev;
            } else {
                m_tail = n->prev;
            }
        }
        
        free_node(n);
        --m_size;
        return next_iter;
    }

};

} // namespace Ktemplats
