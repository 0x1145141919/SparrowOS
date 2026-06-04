// ════════════════════════════════════════════════════════════════
// netty_simulator — 旧版 Netty PoolChunk byte-per-node 树模拟器
//
// 参考: PoolChunk_old.java (pre-0d701d7c3c)
// 接口与 BuddyControlBlock_foundation 对齐
//
// Netty 内部使用 depth (0=root=N, N=leaf=0)，
// 对外接口使用 order (0=leaf, N=root)，自动转换
// ════════════════════════════════════════════════════════════════

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include "abi/os_error_definitions.h"

class netty_simulator {
public:
    static constexpr uint8_t  ORDER_COUNT = 65;
    static constexpr uint8_t  ERROR_MARK = 0x40;

private:
    uint8_t* memoryMap;
    uint64_t  mapSize;
    uint8_t   max_order;
    uint8_t   unusable;

    static inline uint8_t depth_of_id(int id) {
        return 63 - __builtin_clzll((uint64_t)id);
    }

public:
    netty_simulator() : memoryMap(nullptr), mapSize(0),
                        max_order(0), unusable(0) {}

    void init(uint64_t bitmap_va, uint8_t mo) {
        max_order = mo;
        unusable  = mo + 1;
        uint64_t leaf_cnt = 1ULL << mo;
        mapSize = leaf_cnt << 1;         // 2 * 2^N bytes
        memoryMap = reinterpret_cast<uint8_t*>(bitmap_va);

        // 初始化: memoryMap[id] = depth_of_id
        // Heap layout: id=1 (root, depth=0), id=2..3 (depth=1), ...
        std::memset(memoryMap, 0, mapSize);
        int idx = 1;
        for (int d = 0; d <= mo; d++) {
            int cnt = 1 << d;
            for (int p = 0; p < cnt; p++)
                memoryMap[idx++] = (uint8_t)d;
        }
    }

    // ─── 底层访问 ───
    uint8_t   val(int id) const { return memoryMap[id]; }
    void      setv(int id, uint8_t v) { memoryMap[id] = v; }

    // ─── 冒泡更新（alloc分支） ───
    void updateParentsAlloc(int id) {
        while (id > 1) {
            int p = id >> 1;
            uint8_t a = val(id);
            uint8_t b = val(id ^ 1);
            setv(p, (a < b) ? a : b);
            id = p;
        }
    }

    // ─── 冒泡更新（free分支，需处理双子全空特例） ───
    void updateParentsFree(int id) {
        int lc = depth_of_id(id) + 1;
        while (id > 1) {
            int p = id >> 1;
            uint8_t a = val(id);
            uint8_t b = val(id ^ 1);
            lc -= 1;
            if (a == lc && b == lc)
                setv(p, (uint8_t)(lc - 1));
            else
                setv(p, (a < b) ? a : b);
            id = p;
        }
    }

    // ─── 核心: 在目标深度 d 精确分配（d=0=root, d=N=leaf） ───
    int allocateNode(int d) {
        if (val(1) > (uint8_t)d) return -1;

        int id = 1;
        uint8_t v = val(id);

        while (v < d) {
            // Not yet at target depth, descend
            id <<= 1;            // try left
            v = val(id);
            if (v > (uint8_t)d) {
                id ^= 1;         // left too deep → right
                v = val(id);
            }
        }

        setv(id, unusable);
        updateParentsAlloc(id);
        return id;
    }

    // ─── 接口 ───

    // 在目标 order 分配；如果精确 order 不可用则尝试更大的 order
    uint64_t find_candidate(uint8_t& base_order, KURD_t& kurd) {
        KURD_t ok, fail;
        ok.result = result_code::SUCCESS;
        fail.result = result_code::FAIL;

        for (int ao = base_order; ao <= (int)max_order; ao++) {
            int d = max_order - ao;          // order → depth
            if (val(1) > (uint8_t)d) continue;
            int id = allocateNode(d);
            if (id >= 0) {
                uint64_t off = id - (1ULL << d);
                base_order = (uint8_t)ao;
                kurd = ok;
                return off;
            }
        }

        base_order = ERROR_MARK;
        kurd = fail;
        return ~0ULL;
    }

    KURD_t split(uint8_t, uint64_t, uint8_t) {
        KURD_t ok; ok.result = result_code::SUCCESS; return ok;
    }

    KURD_t order_occupy_try(uint8_t, uint64_t) {
        KURD_t ok; ok.result = result_code::SUCCESS; return ok;
    }

    uint8_t order_return(uint8_t order, uint64_t offset, KURD_t& kurd) {
        int d   = max_order - order;
        int id  = (int)((1ULL << d) + offset);

        setv(id, depth_of_id(id));
        updateParentsFree(id);

        kurd = KURD_t(0, 0, 0, 0, 0, 0, err_domain::CORE_MODULE);
        kurd.result = result_code::SUCCESS;
        return order;
    }

    bool order_exist_check(uint8_t order) const {
        return val(1) <= (max_order - order);
    }

    bool is_free(uint8_t order, uint64_t offset) const {
        int d  = max_order - order;
        int id = (int)((1ULL << d) + offset);
        return val(id) == (uint8_t)d;
    }

    uint8_t get_max_order() const { return max_order; }
};
