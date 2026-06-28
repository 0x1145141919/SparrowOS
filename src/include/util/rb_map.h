#pragma once
#include "util/Ktemplats.h"

// ── rb_map<K, V> — 基于红黑树的有序 K/V 映射 ────────────────
//
// 底层使用 Ktemplats::RBTree 的红黑树机制。
// 查找/插入/删除均为 O(log N)。
//
// 典型用途：
//   rb_map<uint64_t, task*>  tid_map;     // tid → task
//   rb_map<wq_id_t, tid_wait_queue*> wq_map;  // qid → wait_queue

template<typename K, typename V>
class rb_map {
    // 内部 K/V 条目，RBTree 以整体为单位存储，比较只按 key
    struct kv_entry {
        K key;
        V value;

        kv_entry() = default;
        kv_entry(const K& k, const V& v) : key(k), value(v) {}
    };

    // 比较函数：只比 key，value 不参与排序
    static int kv_compare(const kv_entry& a, const kv_entry& b) {
        if (a.key > b.key) return 1;
        if (a.key < b.key) return -1;
        return 0;
    }

    // 底层红黑树
    Ktemplats::RBTree<kv_entry, kv_compare> m_tree;

public:
    // ── 插入 ──
    // 返回 true 表示成功插入新条目
    // 返回 false 表示 key 已存在（不覆盖）
    bool insert(const K& key, const V& value) {
        return m_tree.insert(kv_entry(key, value));
    }

    // ── 查找 ──
    // 返回 value 指针，key 不存在时返回 nullptr
    V* find(const K& key) {
        kv_entry probe;
        probe.key = key;
        kv_entry* entry = m_tree.find(probe);
        return entry ? &entry->value : nullptr;
    }

    const V* find(const K& key) const {
        kv_entry probe;
        probe.key = key;
        const kv_entry* entry = m_tree.find(probe);
        return entry ? &entry->value : nullptr;
    }

    // ── 删除 ──
    // 返回 true 表示成功删除，false 表示 key 不存在
    bool remove(const K& key) {
        kv_entry probe;
        probe.key = key;
        return m_tree.erase(probe);
    }

    // ── 存在性检查 ──
    bool contains(const K& key) const {
        kv_entry probe;
        probe.key = key;
        return m_tree.contains(probe);
    }

    // ── 容量 ──
    size_t size() const {
        return m_tree.size();
    }

    bool empty() const {
        return m_tree.empty();
    }

    // ── 清空 ──
    void clear() {
        m_tree.clear();
    }
};
