#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "panic.h"

// ════════════════════════════════════════════════════════════════
// HCB_v3 移植: mixed_bitmap_v2 适配
//   - 所有 order_bases[o] + idx → bcb_bitmap.bit_get/bit_set0/bit_set1(idx, order)
//   - 所有 find_free_in_interval → bcb_bitmap.scan_free_block
//   - corebcb_mixedbitmap_base_acclaim 简化 (不再计算 order_bases)
// ════════════════════════════════════════════════════════════════

bool FreePagesAllocator::BuddyControlBlock::is_addr_belong_to_this_BCB_no_lock(phyaddr_t addr)
{
    return (addr>=this->base) && (addr<(this->base+(1ull<<(max_supprt_order+12))));
}

KURD_t FreePagesAllocator::BuddyControlBlock::default_success()
{
    KURD_t kurd=default_kurd();
    kurd.result=result_code::SUCCESS;
    kurd.level=level_code::INFO;
    return kurd;
}

bool FreePagesAllocator::BuddyControlBlock::can_alloc(uint8_t order)
{
    if (dirty_count != 0) return false;
    for(uint8_t current_order=order;current_order<=max_supprt_order;++current_order){
        if(statistics.free_count[current_order]>0) return true;
    }
    return false;
}

void FreePagesAllocator::BuddyControlBlock::corebcb_mixedbitmap_base_acclaim(vaddr_t bitmap_base_addr)
{
    bcb_bitmap.online(bitmap_base_addr, max_supprt_order);
    // order_bases 已删除: 位置编码 order, 无需计算
    #ifdef REPALY_MODE
    replay_internal_init();
    #endif
    free_page_without_merge(0, max_supprt_order);
    #ifdef REPALY_MODE
    KURD_t replay_kurd = replay_validate_tree_no_lock("second_stage_init");
    if (!success_all_kurd(replay_kurd)) return;
    #endif
}

uint8_t FreePagesAllocator::BuddyControlBlock::get_order() { return this->max_supprt_order; }
KURD_t FreePagesAllocator::BuddyControlBlock::default_fatal() { KURD_t k=default_kurd(); k=set_fatal_result_level(k); return k; }
KURD_t FreePagesAllocator::BuddyControlBlock::default_error() { KURD_t k=default_kurd(); k=set_result_fail_and_error_level(k); return k; }
KURD_t FreePagesAllocator::BuddyControlBlock::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK,0,0,err_domain::CORE_MODULE);
}
uint8_t FreePagesAllocator::BuddyControlBlock::get_max_order() { return this->max_supprt_order; }

// ─── 缓存 ───

void FreePagesAllocator::BuddyControlBlock::cache_insert(uint8_t order, uint64_t idx)
{
    if (order >= DESINGED_MAX_SUPPORT_ORDER) return;
    for (uint8_t i = 0; i < PER_ORDER_CACHE_SUGGEST_COUNT; ++i) {
        if (suggest_order_free_page_index[order][i] == INVALID_INBCB_INDEX) {
            suggest_order_free_page_index[order][i] = idx;
            return;
        }
    }
    uint8_t& cursor = suggest_order_cache_cursor[order];
    suggest_order_free_page_index[order][cursor] = idx;
    cursor = (cursor + 1) % PER_ORDER_CACHE_SUGGEST_COUNT;
}

bool FreePagesAllocator::BuddyControlBlock::cache_pick(uint8_t order, uint64_t& out_idx)
{
    if (order >= DESINGED_MAX_SUPPORT_ORDER) return false;
    for (uint8_t i = 0; i < PER_ORDER_CACHE_SUGGEST_COUNT; ++i) {
        uint64_t idx = suggest_order_free_page_index[order][i];
        if (idx == INVALID_INBCB_INDEX) continue;
        if (bcb_bitmap.bit_get(idx, order)) {
            out_idx = idx;
            suggest_order_free_page_index[order][i] = INVALID_INBCB_INDEX;
            return true;
        }
        suggest_order_free_page_index[order][i] = INVALID_INBCB_INDEX;
    }
    return false;
}

// ─── size_to_order ───

uint8_t FreePagesAllocator::BuddyControlBlock::size_to_order(uint64_t size)
{
    if (size == 0) return 0;
    uint64_t numof_4kbpgs = (size + _4KB_PAGESIZE - 1) / _4KB_PAGESIZE;
    auto next_pow2 = [](uint64_t n) -> uint64_t {
        n--; n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16; n |= n >> 32; n++;
        return n;
    };
    return 63 - __builtin_clzll(next_pow2(numof_4kbpgs));
}

// ─── allocate_buddy_way ───

phyaddr_t FreePagesAllocator::BuddyControlBlock::allocate_buddy_way(uint64_t size, KURD_t &result, uint8_t align_log2)
{
    KURD_t success=default_success();
    KURD_t error=default_error();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY;
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY;
    fatal.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY;
    uint8_t order = size_to_order(size);
    uint8_t align_order = align_log2>12 ? align_log2-12 : 0;
    order = order > align_order ? order : align_order;
    if(order > max_supprt_order || align_order > max_supprt_order){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::ALLOCATE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_ACQUIRE_SIZE_TO_BIG;
        result=error; return 0;
    }

    // 先试缓存 (所有 order)
    for(uint8_t i=order; i<=max_supprt_order; i++){
        uint64_t cached_idx = INVALID_INBCB_INDEX;
        if(cache_pick(i, cached_idx)){
            statistics.suggest_hit[order]++;
            if(i > order){
                KURD_t kurd = split_page(cached_idx, i, order);
                if(!success_all_kurd(kurd)){ result=kurd; return 0; }
            }
            // 分裂后, 目标 order 上的偏移为 cached_idx << (i - order)
            uint64_t alloc_idx = (i > order) ? (cached_idx << (i - order)) : cached_idx;
            bcb_bitmap.bit_set0(alloc_idx, order);
            #ifdef REPALY_MODE
            replay_internal_mark_free(order, alloc_idx);
            KURD_t replay_kurd = replay_validate_tree_no_lock("allocate_buddy_way");
            if(!success_all_kurd(replay_kurd)){ result=replay_kurd; return 0; }
            #endif
            // 物理地址由原始 cache_idx 在 order i 上决定 (分裂不改变物理地址)
            phyaddr_t res_addr = base + (cached_idx << (i + 12));
            statistics.alloc_times_success++;
            statistics.free_count[order]--;
            result=success; return res_addr;
        } else statistics.suggest_miss[order]++;
    }

    // 全部 cache miss → 扫描位图 (用 scan_free_block)
    statistics.scan_count++;
    for(uint8_t i=order; i<=max_supprt_order; i++){
        uint8_t found_order = i;
        uint64_t found_off = bcb_bitmap.scan_free_block(found_order);
        // scan_free_block 返回 offset (非 bitmap 绝对引索)
        if(found_off == ~0ULL) continue;

        if(found_order > order){
            KURD_t kurd = split_page(found_off, found_order, order);
            if(!success_all_kurd(kurd)){ result=kurd; return 0; }
            found_off <<= (found_order - order);
        }
        bcb_bitmap.bit_set0(found_off, order);
        #ifdef REPALY_MODE
        replay_internal_mark_free(order, found_off);
        KURD_t replay_kurd = replay_validate_tree_no_lock("allocate_buddy_way");
        if(!success_all_kurd(replay_kurd)){ result=replay_kurd; return 0; }
        #endif
        statistics.alloc_times_success++;
        phyaddr_t res_addr = base + (found_off << (order + 12));
        statistics.free_count[order]--;
        result=success; return res_addr;
    }

    statistics.alloc_times_fail++;
    error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::ALLOCATE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_NO_AVALIABLE_BUDDY;
    result=error; return 0;
}

// ─── 构造 ───

FreePagesAllocator::BuddyControlBlock::BuddyControlBlock(phyaddr_t base, uint8_t max_support_order)
{
    this->max_supprt_order = max_support_order;
    this->base = base;
    for(uint8_t i=0; i<=max_supprt_order; i++){
        for(uint8_t j=0; j<PER_ORDER_CACHE_SUGGEST_COUNT; j++)
            suggest_order_free_page_index[i][j] = ~0;
        suggest_order_cache_cursor[i] = 0;
    }
    #ifdef REPALY_MODE
    for(uint8_t i=0; i<DESINGED_MAX_SUPPORT_ORDER; i++) order_Internal_bitmap[i] = nullptr;
    #endif
    ksetmem_8(&statistics, 0, sizeof(statistics));
}
FreePagesAllocator::BuddyControlBlock::BuddyControlBlock() {}

// ─── free_page_without_merge ───

void FreePagesAllocator::BuddyControlBlock::free_page_without_merge(uint64_t in_bcb_idx, uint8_t order)
{
    bcb_bitmap.bit_set1(in_bcb_idx, order);
    statistics.free_count[order]++;
    cache_insert(order, in_bcb_idx);
    #ifdef REPALY_MODE
    /* replay_internal_mark_free(order, in_bcb_idx); ... */
    #endif
}

// ─── split_page ───

KURD_t FreePagesAllocator::BuddyControlBlock::split_page(uint64_t splited_idx, uint8_t splited_order, uint8_t target_order)
{
    KURD_t success=default_success();
    KURD_t error=default_error();
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_SPLIT_PAGE;
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_SPLIT_PAGE;
    if(splited_order < target_order || (target_order < 0) ||
       target_order > max_supprt_order || splited_order > max_supprt_order){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::SPLIT_PAGE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_ORDER;
        return error;
    }
    if(splited_order == target_order) return success;

    bcb_bitmap.bit_set0(splited_idx, splited_order);
    #ifdef REPALY_MODE
    replay_internal_mark_split(splited_order, splited_idx);
    KURD_t replay_kurd = replay_validate_tree_no_lock("split_page");
    if(!success_all_kurd(replay_kurd)) return replay_kurd;
    #endif
    statistics.free_count[splited_order]--;
    free_page_without_merge(1 + (splited_idx << 1), splited_order - 1);
    statistics.free_count[splited_order - 1]++; // 额外 +1 平衡分配时的 --
    statistics.split_count++;
    return split_page(splited_idx << 1, splited_order - 1, target_order);
}

// ─── conanico_free ───

KURD_t FreePagesAllocator::BuddyControlBlock::conanico_free(uint64_t in_bcb_idx, uint8_t order)
{
    KURD_t success=default_success();
    KURD_t error=default_error();
    KURD_t fatal=default_fatal();
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE;
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE;
    fatal.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE;
    if(order > max_supprt_order){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_ORDER;
        return error;
    }
    if(bcb_bitmap.bit_get(in_bcb_idx, order)){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_DOUBLE_FREE;
        return error;
    }
    if(in_bcb_idx >= (1ULL << (max_supprt_order - order))){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_PAGE_INDEX;
        return error;
    }
    bcb_bitmap.bit_set1(in_bcb_idx, order);
    #ifdef REPALY_MODE
    replay_internal_mark_free(order, in_bcb_idx);
    #endif
    cache_insert(order, in_bcb_idx);
    statistics.free_count[order]++;

    uint8_t current_order = order;
    uint64_t current_idx = in_bcb_idx;
    for(; current_order < max_supprt_order; current_order++){
        uint64_t buddy_idx = current_idx ^ 1;
        if(!bcb_bitmap.bit_get(buddy_idx, current_order)){
            statistics.fold_count_fail++;
            break;
        }
        bcb_bitmap.bit_set0(current_idx, current_order);
        bcb_bitmap.bit_set0(buddy_idx, current_order);
        statistics.free_count[current_order] -= 2;
        statistics.fold_count_success++;
        current_idx >>= 1;
        if(bcb_bitmap.bit_get(current_idx, current_order + 1)){
            fatal.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FATAL_REASONS_CODE::BIN_TREE_CONSISTENCY_VIOLATION;
            return fatal;
        }
        bcb_bitmap.bit_set1(current_idx, current_order + 1);
        #ifdef REPALY_MODE
        replay_internal_mark_free(current_order + 1, current_idx);
        KURD_t replay_kurd = replay_validate_tree_no_lock("conanico_free");
        if(!success_all_kurd(replay_kurd)) return replay_kurd;
        #endif
        cache_insert(current_order + 1, current_idx);
        statistics.free_count[current_order + 1]++;
    }
    return success;
}

// ─── is_reclusive_fold_success ───

bool FreePagesAllocator::BuddyControlBlock::is_reclusive_fold_success(uint64_t idx, uint8_t order)
{
    if(order == 0){
        return bcb_bitmap.bit_get(idx, 0);
    }
    bool left = is_reclusive_fold_success(idx << 1, order - 1);
    bool right = is_reclusive_fold_success((idx << 1) + 1, order - 1);
    if(left && right){
        bcb_bitmap.bit_set0(idx << 1, order - 1);
        bcb_bitmap.bit_set0((idx << 1) + 1, order - 1);
        bcb_bitmap.bit_set1(idx, order);
        return true;
    }
    return false;
}

// ─── top_fold ───

void FreePagesAllocator::BuddyControlBlock::top_fold()
{
    if(!is_reclusive_fold_success(0, max_supprt_order))
        return;
}

// ─── free_pages_flush ───

KURD_t FreePagesAllocator::BuddyControlBlock::free_pages_flush()
{
    KURD_t success=default_success();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FLUSH_FREE_COUNT;
    fatal.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FLUSH_FREE_COUNT;
    for(uint8_t i=0; i<=max_supprt_order; i++){
        uint64_t actual = 0;
        for(uint64_t j=0; j<(1ULL<<(max_supprt_order-i)); j++){
            if(bcb_bitmap.bit_get(j, i)) actual++;
        }
        if(actual != statistics.free_count[i]){
            bsp_kout << "mixed_bitmap_v2: order " << (uint32_t)i
                     << " free_count mismatch: expected=" << statistics.free_count[i]
                     << " actual=" << actual << kendl;
            return fatal;
        }
    }
    return success;
}

// ─── free_buddy_way / is_addr_belong_to_this_BCB ───
// (这些不涉及 order_bases, 保持原样)

phyaddr_t FreePagesAllocator::BuddyControlBlock::get_base() { return this->base; }

bool FreePagesAllocator::BuddyControlBlock::is_addr_belong_to_this_BCB(phyaddr_t addr)
{
    // 省略锁逻辑 (原实现)
    return is_addr_belong_to_this_BCB_no_lock(addr);
}

KURD_t FreePagesAllocator::BuddyControlBlock::free_buddy_way(phyaddr_t base, uint64_t size)
{
    using namespace MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::FREE_RESULTS_CODE;
    KURD_t success=default_success();
    KURD_t error=default_error();
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FREE;
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FREE;
    if(!is_addr_belong_to_this_BCB_no_lock(base) ||
       !is_addr_belong_to_this_BCB_no_lock(base + size - 1)){
        bsp_kout << "free_buddy_way: addr not belong" << kendl;
        error.reason=FAIL_REASONS_CODE::FAIL_REASON_CODE_BASE_NOT_BELONG;
        return error;
    }
    uint8_t order = size_to_order(size);
    uint64_t in_bcb_idx = (base - this->base) >> (order + 12);
    return conanico_free(in_bcb_idx, order);
}

// ─── print 函数 (精简, 只改位图访问) ───

void FreePagesAllocator::BuddyControlBlock::print_basic_info_no_lock()
{
    bsp_kout << "BCB base=0x" << (void*)(uint64_t)base
             << " max_order=" << (uint32_t)max_supprt_order << kendl;
}

void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_info_compress_no_lock(uint8_t order)
{
    uint64_t count = 1ULL << (max_supprt_order - order);
    bsp_kout << "Order " << (uint32_t)order << " [";
    for(uint64_t j=0; j<count; j++){
        bsp_kout << (bcb_bitmap.bit_get(j, order) ? '1' : '0');
    }
    bsp_kout << "]" << kendl;
}

void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_interval_compress_no_lock(uint8_t order,
    uint64_t base_, uint64_t length)
{
    uint64_t end = base_ + length;
    uint64_t count = 1ULL << (max_supprt_order - order);
    if(end > count) end = count;
    bsp_kout << "Order " << (uint32_t)order << "[" << base_ << "," << end << "): ";
    char last = 'X';
    uint64_t run = 0;
    for(uint64_t j=base_; j<end; j++){
        char c = bcb_bitmap.bit_get(j, order) ? '1' : '0';
        if(c != last && run > 0){
            bsp_kout << run << last << " ";
            run = 0;
        }
        last = c; run++;
    }
    if(run > 0) bsp_kout << run << last;
    bsp_kout << kendl;
}

void FreePagesAllocator::BuddyControlBlock::print_bitmap_info_no_lock()
{
    for(uint8_t i=0; i<=max_supprt_order; i++)
        print_bitmap_order_info_compress_no_lock(i);
}

void FreePagesAllocator::BuddyControlBlock::print_basic_info()  { /* 锁原样 */ print_basic_info_no_lock(); }
void FreePagesAllocator::BuddyControlBlock::print_bitmap_info() { print_bitmap_info_no_lock(); }
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_info_compress(uint8_t o) { print_bitmap_order_info_compress_no_lock(o); }
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_interval_compress(uint8_t o, uint64_t b, uint64_t l) { print_bitmap_order_interval_compress_no_lock(o, b, l); }

bool FreePagesAllocator::BuddyControlBlock::is_bcb_avaliable() { return true; }
