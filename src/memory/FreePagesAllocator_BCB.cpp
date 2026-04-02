#include "memory/FreePagesAllocator.h"
#include "memory/all_pages_arr.h"
#include "util/kout.h"
#include "util/OS_utils.h"
#include "panic.h"
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

    for(uint8_t current_order=order;current_order<=max_supprt_order;++current_order){
        if(statistics.free_count[current_order]>0){
            return true;
        }
    }
    return false;
}
void FreePagesAllocator::BuddyControlBlock::corebcb_mixedbitmap_base_acclaim(vaddr_t bitmap_base_addr)
{
    order_freepage_existency_bitmaps=new mixed_bitmap_t(1ull<<(max_supprt_order+1), bitmap_base_addr);
    order_freepage_existency_bitmaps->mixedbitmap_base_specify(bitmap_base_addr);
    order_bases[0]=0;
    for(uint8_t order=0;order<max_supprt_order;order++){
        order_bases[order+1]=order_bases[order]+(1ULL<<(max_supprt_order-order));
        //bsp_kout<<"order_bases["<<order<<"]="<<order_bases[order]<<kendl;
    }
    #ifdef REPALY_MODE
    replay_internal_init();
    #endif
    //bsp_kout<<max_supprt_order<<kendl;
    free_page_without_merge(0,max_supprt_order);
    #ifdef REPALY_MODE
    KURD_t replay_kurd = replay_validate_tree_no_lock("second_stage_init");
    if (!success_all_kurd(replay_kurd)) {
        return replay_kurd;
    }
    #endif
}
uint8_t FreePagesAllocator::BuddyControlBlock::get_order()
{
    return this->max_supprt_order;
}
KURD_t FreePagesAllocator::BuddyControlBlock::default_fatal()
{
    KURD_t kurd=default_kurd();
    kurd=set_fatal_result_level(kurd);
    return kurd;
}
KURD_t FreePagesAllocator::BuddyControlBlock::default_error()
{
    KURD_t kurd=default_kurd();
    kurd=set_result_fail_and_error_level(kurd);
    return kurd;
}
KURD_t FreePagesAllocator::BuddyControlBlock::default_kurd()
{
    return KURD_t(0,0,module_code::MEMORY,MEMMODULE_LOCAIONS::LOCATION_CODE_FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK,0,0,err_domain::CORE_MODULE);
}
#ifdef REPALY_MODE
void FreePagesAllocator::free_pages_in_seg_control_block::replay_internal_init()
{
    for (uint8_t order = 0; order < DESINGED_MAX_SUPPORT_ORDER; ++order) {
        order_Internal_bitmap[order] = nullptr;
    }
    for (uint8_t order = 1; order <= MAX_SUPPORT_ORDER && order < DESINGED_MAX_SUPPORT_ORDER; ++order) {
        order_Internal_bitmap[order] = new Ktemplats::kernel_bitmap(1ULL << (MAX_SUPPORT_ORDER - order));
    }
}

void FreePagesAllocator::free_pages_in_seg_control_block::replay_internal_mark_split(uint8_t order, uint64_t idx)
{
    if (order == 0 || order >= DESINGED_MAX_SUPPORT_ORDER) return;
    if (order_Internal_bitmap[order]) {
        order_Internal_bitmap[order]->bit_set(idx, true);
    }
}

void FreePagesAllocator::free_pages_in_seg_control_block::replay_internal_mark_free(uint8_t order, uint64_t idx)
{
    if (order == 0 || order >= DESINGED_MAX_SUPPORT_ORDER) return;
    if (order_Internal_bitmap[order]) {
        order_Internal_bitmap[order]->bit_set(idx, false);
    }
}

KURD_t FreePagesAllocator::free_pages_in_seg_control_block::replay_validate_node(uint8_t order, uint64_t idx, const char* tag)
{
    namespace REPLAY_FATAL = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::REPLAY_VALIDATE_RESULTS_CODE::FATAL_REASONS_CODE;
    auto make_success = [this]() {
        KURD_t kurd = default_success();
        kurd.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_REPLAY_VALIDATE;
        return kurd;
    };
    auto make_fatal = [this](uint16_t reason) {
        KURD_t kurd = default_fatal();
        kurd.event_code = MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_REPLAY_VALIDATE;
        kurd.reason = reason;
        return kurd;
    };
    if (order > MAX_SUPPORT_ORDER) {
        bsp_kout<< "Replay validate invalid order at " << tag << kendl;
        return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_INVALID_ORDER);
    }
    const uint64_t count = 1ULL << (MAX_SUPPORT_ORDER - order);
    if (idx >= count) {
        bsp_kout<< "Replay validate invalid index at " << tag << kendl;
        return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_INVALID_INDEX);
    }

    bool parent_free = order_freepage_existency_bitmaps->bit_get(order_bases[order] + idx);
    bool parent_internal = false;
    if (order > 0) {
        if (!order_Internal_bitmap[order]) {
            bsp_kout<< "Replay validate missing internal bitmap at " << tag << kendl;
            return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_INTERNAL_BITMAP_MISSING);
        }
        parent_internal = order_Internal_bitmap[order]->bit_get(idx);
    }
    if (parent_free && parent_internal) {
        bsp_kout<< "Replay validate conflict parent free/internal at " << tag << kendl;
        return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_INTERNAL_CONFLICT);
    }
    if (order == 0) {
        return make_success();
    }

    const uint64_t left_idx = idx << 1;
    const uint64_t right_idx = left_idx + 1;
    bool left_free = order_freepage_existency_bitmaps->bit_get(order_bases[order - 1] + left_idx);
    bool right_free = order_freepage_existency_bitmaps->bit_get(order_bases[order - 1] + right_idx);
    bool left_internal = false;
    bool right_internal = false;
    if (order > 1) {
        if (!order_Internal_bitmap[order - 1]) {
            bsp_kout<< "Replay validate missing internal bitmap at " << tag << kendl;
            return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_INTERNAL_BITMAP_MISSING);
        }
        left_internal = order_Internal_bitmap[order - 1]->bit_get(left_idx);
        right_internal = order_Internal_bitmap[order - 1]->bit_get(right_idx);
    }

    if (parent_free) {
        if (left_free || right_free || left_internal || right_internal) {
            bsp_kout<< "Replay validate parent free but child used at " << tag << kendl;
            return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_PARENT_FREE_CHILD_USED);
        }
        return make_success();
    }
    if (!parent_internal) {
        if (left_free || right_free || left_internal || right_internal) {
            bsp_kout<< "Replay validate parent used but child used at " << tag << kendl;
            return make_fatal(REPLAY_FATAL::FAIL_REASON_CODE_PARENT_USED_CHILD_USED);
        }
        return make_success();
    }

    KURD_t left_res = replay_validate_node(order - 1, left_idx, tag);
    if (!success_all_kurd(left_res)) return left_res;
    KURD_t right_res = replay_validate_node(order - 1, right_idx, tag);
    return right_res;
}

KURD_t FreePagesAllocator::free_pages_in_seg_control_block::replay_validate_tree_no_lock(const char* tag)
{
    return replay_validate_node(MAX_SUPPORT_ORDER, 0, tag ? tag : "replay_validate_tree");
}

KURD_t FreePagesAllocator::free_pages_in_seg_control_block::replay_validate_tree(const char* tag)
{
    bcb_lock_guard_t guard(lock);
    return replay_validate_tree_no_lock(tag);
}
#endif
uint8_t FreePagesAllocator::BuddyControlBlock::get_max_order()
{

    return this->max_supprt_order;
}

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
        if (order_freepage_existency_bitmaps->bit_get(order_bases[order] + idx)) {
            out_idx = idx;
            suggest_order_free_page_index[order][i] = INVALID_INBCB_INDEX;
            return true;
        }
        suggest_order_free_page_index[order][i] = INVALID_INBCB_INDEX;
    }
    return false;
}

uint8_t FreePagesAllocator::BuddyControlBlock::size_to_order(uint64_t size)
{
    if (size == 0) {
        return 0;
    }
    
    // 计算需要多少个4KB页面（向上取整）
    uint64_t numof_4kbpgs = (size + _4KB_PAGESIZE - 1) / _4KB_PAGESIZE;
    
    // 使用匿名函数计算向上取整的2的幂
    auto next_pow2 = [](uint64_t n) -> uint64_t {
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        n++;
        return n;
    };
    
    // 将页面数向上取整到2的幂
    uint64_t np2 = next_pow2(numof_4kbpgs);
    
    // 用__builtin_clzll取log2
    // 注意：np2 保证是2的幂且不为0
    return 63 - __builtin_clzll(np2);
}
phyaddr_t FreePagesAllocator::BuddyControlBlock::allocate_buddy_way(uint64_t size, KURD_t &result,uint8_t align_log2)
{

    KURD_t success=default_success();
    KURD_t error=default_error();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY;
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY;
    fatal.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_ALLOCATE_BUDY_WAY;
    uint8_t order=0;
    uint8_t size_order=size_to_order(size);
    uint8_t align_order=align_log2>12?align_log2-12:0;

    order=size_order>align_order?size_order:align_order;
    if(size_order>max_supprt_order||align_order>max_supprt_order){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::ALLOCATE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_ACQUIRE_SIZE_TO_BIG;
        result=error;
        return 0;
    }
    KURD_t kurd;
    //kurd=free_pages_flush();
    if(!success_all_kurd(kurd)){
            bsp_kout<<"free page allocator fatal error"<<kendl;
            print_basic_info_no_lock();
            result=kurd;
            return 0;
    }
    for(uint8_t i=order;i<=max_supprt_order;i++){
        uint64_t cached_idx = INVALID_INBCB_INDEX;
        if(cache_pick(i, cached_idx)){
            statistics.suggest_hit[order]++;
            if(i>order){
                kurd=split_page(cached_idx,i,order);
                if(!success_all_kurd(kurd)){
                    result=kurd;
                    return 0;
                }
            }
            order_freepage_existency_bitmaps->bit_set(order_bases[order]+(cached_idx<<(i-order)),false);
            #ifdef REPALY_MODE
            replay_internal_mark_free(order, (cached_idx << (i - order)));
            KURD_t replay_kurd = replay_validate_tree_no_lock("allocate_buddy_way");
            if (!success_all_kurd(replay_kurd)) {
                result = replay_kurd;
                return 0;
            }
            #endif
            
            phyaddr_t res_addr=base+(cached_idx<< (i+12));
            statistics.alloc_times_success++;
            if(align_order>size_order){
                kurd=split_page(cached_idx << (i - order),order,size_order);
                if(!success_all_kurd(kurd)){
                    statistics.alloc_times_success--;
                    statistics.alloc_times_fail++;
                    result=kurd;
                    return 0;
                }
                statistics.free_count[size_order]--;
            }else{
                statistics.free_count[order]--;
            }
            result=success;
            return res_addr;
        }else statistics.suggest_miss[order]++;
    }
    statistics.scan_count++;
    for(uint8_t i=order;i<=max_supprt_order;i++){//线性扫描
        uint64_t found_idx=order_freepage_existency_bitmaps->find_free_in_interval(
            order_bases[i],
            1ULL<<(max_supprt_order-i)
        );
        if(found_idx!=0xFFFFFFFFFFFFFFFF){
            result=success;
            if(i>order){
                kurd=split_page(found_idx,i,order);
                if(!success_all_kurd(kurd)){
                    result=kurd;
                    return 0;
                }
            }
            order_freepage_existency_bitmaps->bit_set(order_bases[order]+(found_idx<<(i-order)),false);
            #ifdef REPALY_MODE
            replay_internal_mark_free(order, (found_idx << (i - order)));
            KURD_t replay_kurd = replay_validate_tree_no_lock("allocate_buddy_way");
            if (!success_all_kurd(replay_kurd)) {
                result = replay_kurd;
                return 0;
            }
            #endif
            
            statistics.alloc_times_success++;
            phyaddr_t res_addr=base+(found_idx<< (i+12));
            if(align_order>size_order){
                kurd=split_page(found_idx<<(i - order),order,size_order);
                if(!success_all_kurd(kurd)){
                    statistics.alloc_times_success--;
                    statistics.alloc_times_fail++;
                    result=kurd;
                    return 0;
                }
                statistics.free_count[size_order]--;
            }else {
                statistics.free_count[order]--;
            }
            return res_addr;
        }
    }

    //bsp_kout<<"higher order free page found,validation start"<<kendl;
    statistics.alloc_times_fail++;
    error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::ALLOCATE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_NO_AVALIABLE_BUDDY;
    result=error;
    return 0;
}
phyaddr_t FreePagesAllocator::BuddyControlBlock::get_base()
{
    return this->base;
}
FreePagesAllocator::BuddyControlBlock::BuddyControlBlock(phyaddr_t base, uint8_t max_support_order)
{
    this->max_supprt_order=max_support_order;
    this->base=base;
    for(uint8_t i=0;i<=max_supprt_order;i++){
        for (uint8_t j = 0; j < PER_ORDER_CACHE_SUGGEST_COUNT; ++j) {
            suggest_order_free_page_index[i][j]=~0;
        }
        suggest_order_cache_cursor[i]=0;
        order_bases[i]=0;
    }
    #ifdef REPALY_MODE
    for (uint8_t i = 0; i < DESINGED_MAX_SUPPORT_ORDER; ++i) {
        order_Internal_bitmap[i] = nullptr;
    }
    #endif
    this->is_splited_bitmap_valid=is_splited_bitmap_valid;
    ksetmem_8(&statistics, 0, sizeof(statistics));
}
FreePagesAllocator::BuddyControlBlock::BuddyControlBlock()
{
}

void FreePagesAllocator::BuddyControlBlock::free_page_without_merge(uint64_t in_bcb_idx, uint8_t order)
{
    order_freepage_existency_bitmaps->bit_set(order_bases[order]+in_bcb_idx,true);
    statistics.free_count[order]++;
    cache_insert(order, in_bcb_idx);
    #ifdef REPALY_MODE
    /*replay_internal_mark_free(order, in_bcb_idx);
    KURD_t replay_kurd = replay_validate_tree_no_lock("free_page_without_merge");
    if (!success_all_kurd(replay_kurd)) {
        return;
    }*/
    #endif
}
KURD_t FreePagesAllocator::BuddyControlBlock::split_page(uint64_t splited_idx, uint8_t splited_order, uint8_t target_order) 
{//每一层递归固定被拆分层减少一个，拆分出来两个增加
    KURD_t success=default_success();
    KURD_t error=default_error();
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_SPLIT_PAGE;
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_SPLIT_PAGE;
    if(splited_order<target_order||target_order>max_supprt_order||splited_order>max_supprt_order){//外部的错误参数
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::SPLIT_PAGE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_ORDER;
        return error;
    }
    if(splited_order==target_order){//触底成功
        return success;
    }
    order_freepage_existency_bitmaps->bit_set(order_bases[splited_order]+splited_idx,false);
    #ifdef REPALY_MODE
    replay_internal_mark_split(splited_order, splited_idx);
    KURD_t replay_kurd = replay_validate_tree_no_lock("split_page");
    if (!success_all_kurd(replay_kurd)) {
        return replay_kurd;
    }
    #endif
    statistics.free_count[splited_order]--;
    free_page_without_merge(1+(splited_idx<<1),splited_order-1);
    statistics.free_count[splited_order-1]++;
    statistics.split_count++;
    return split_page(splited_idx<<1,splited_order-1,target_order);
}
KURD_t FreePagesAllocator::BuddyControlBlock::conanico_free(uint64_t in_bcb_idx, uint8_t order)
{
    KURD_t success=default_success();
    KURD_t error=default_error();
    KURD_t fatal=default_fatal();
    error.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE;
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE;
    fatal.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_CONANICO_FREE;
    if(order>max_supprt_order){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_ORDER;
        return error;
    }
    if(order_freepage_existency_bitmaps->bit_get(order_bases[order]+in_bcb_idx)){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_DOUBLE_FREE;
        return error;
    }
    if(in_bcb_idx>=1ULL<<(max_supprt_order-order)){
        error.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_INVALID_PAGE_INDEX;
        return error;
    }
    order_freepage_existency_bitmaps->bit_set(order_bases[order]+in_bcb_idx,true);
    #ifdef REPALY_MODE
    replay_internal_mark_free(order, in_bcb_idx);
    #endif
    cache_insert(order, in_bcb_idx);
    statistics.free_count[order]++;
    uint8_t current_order=order;
    uint64_t current_idx=in_bcb_idx;
    for(;current_order<max_supprt_order;current_order++){
        uint64_t buddy_idx=current_idx^1;
        if(!order_freepage_existency_bitmaps->bit_get(order_bases[current_order]+buddy_idx)){
            statistics.fold_count_fail++;
            break;
        }
        order_freepage_existency_bitmaps->bit_set(order_bases[current_order]+buddy_idx,false);
        order_freepage_existency_bitmaps->bit_set(order_bases[current_order]+current_idx,false);
        statistics.free_count[current_order]-=2;
        statistics.fold_count_success++;
        current_idx=current_idx>>1;
        if(order_freepage_existency_bitmaps->bit_get(order_bases[current_order+1]+current_idx)){
            //检测到二叉树结构被破坏，准备panic
            fatal.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::CONANICO_FREE_RESULTS_CODE::FATAL_REASONS_CODE::BIN_TREE_CONSISTENCY_VIOLATION;
            return fatal;
        }
        order_freepage_existency_bitmaps->bit_set(order_bases[current_order+1]+current_idx,true);
        #ifdef REPALY_MODE
        replay_internal_mark_free(current_order + 1, current_idx);
        #endif
        cache_insert(current_order + 1, current_idx);
        statistics.free_count[current_order+1]++;
    }
    statistics.free_times_success++;
    #ifdef REPALY_MODE
    KURD_t replay_kurd = replay_validate_tree_no_lock("conanico_free");
    if (!success_all_kurd(replay_kurd)) {
        return replay_kurd;
    }
    #endif
    return success;
}
bool FreePagesAllocator::BuddyControlBlock::is_addr_belong_to_this_BCB(phyaddr_t addr)
{
    uint64_t base_val = static_cast<uint64_t>(this->base);
    uint64_t addr_val = static_cast<uint64_t>(addr);
    uint64_t end_val = base_val + (static_cast<uint64_t>(1) << (max_supprt_order+12));
    
    return (base_val <= addr_val) && (addr_val < end_val);
}
KURD_t FreePagesAllocator::BuddyControlBlock::free_buddy_way(phyaddr_t base, uint64_t size)
{
    bool base_belong=is_addr_belong_to_this_BCB_no_lock(base);
    bool top_belong=is_addr_belong_to_this_BCB_no_lock(base+size-1);
    if(base_belong&&top_belong){
        uint8_t order=size_to_order(size);
        uint64_t in_bcb_idx=(base-this->base)>>(order+12);//还要对齐校验
        return conanico_free(in_bcb_idx,order);
    }else{
        KURD_t fail=default_error();
        fail.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FREE;
        fail.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::FREE_RESULTS_CODE::FAIL_REASONS_CODE::FAIL_REASON_CODE_BASE_NOT_BELONG;
        uint64_t bcb_size = (1ull<<(max_supprt_order+12));
        bsp_kout<<"[ERROR] Bad free request - release seg: ["
            <<HEX<<base<<" , "<<(base+size-1)<<"] (size: "<<size<<")"
            <<" does not belong to BCB: ["<<this->base<<" , "<<(this->base+bcb_size-1)<<"] (size: "<<bcb_size<<")"
            <<" base_belong:"<<base_belong<<" top_belong:"<<top_belong;
        return fail;
    }
}
bool FreePagesAllocator::BuddyControlBlock::is_reclusive_fold_success(uint64_t idx, uint8_t order)
{
    if(order>max_supprt_order||order==0){
        return false;
    }
   
    bool left=order_freepage_existency_bitmaps->bit_get(order_bases[order-1]+(idx<<1));
    bool right=order_freepage_existency_bitmaps->bit_get(order_bases[order-1]+(idx<<1)+1);
    if(left&&right){
        order_freepage_existency_bitmaps->bit_set(order_bases[order]+idx,true);
        #ifdef REPALY_MODE
        replay_internal_mark_free(order, idx);
        #endif
        cache_insert(order, idx);
        statistics.free_count[order]++;
        order_freepage_existency_bitmaps->bit_set(order_bases[order-1]+(idx<<1),false);
        order_freepage_existency_bitmaps->bit_set(order_bases[order-1]+(idx<<1)+1,false);
        statistics.free_count[order-1]-=2;
        statistics.fold_count_success++;
        return true;
    }
    bool left_succeed=false;
    bool right_succeed=false;
    if(!left){
        left_succeed=is_reclusive_fold_success(idx<<1,order-1);
    }
    if(!right){
        right_succeed=is_reclusive_fold_success((idx<<1)+1,order-1);
    }
    if(left_succeed&&right_succeed){
        order_freepage_existency_bitmaps->bit_set(order_bases[order]+idx,true);
        #ifdef REPALY_MODE
        replay_internal_mark_free(order, idx);
        #endif
        cache_insert(order, idx);
        statistics.free_count[order]++;
        order_freepage_existency_bitmaps->bit_set(order_bases[order-1]+(idx<<1),false);
        order_freepage_existency_bitmaps->bit_set(order_bases[order-1]+(idx<<1)+1,false);
        statistics.free_count[order-1]-=2;
        statistics.fold_count_success++;
        return true;
    }
        statistics.fold_count_fail++;
        return false;

}
void FreePagesAllocator::BuddyControlBlock::top_fold()
{

    is_reclusive_fold_success(0,max_supprt_order);
}
void FreePagesAllocator::BuddyControlBlock::print_basic_info_no_lock()
{
    /**
     * 打印base，MAX_SUPPORT_ORDER，suggest_order_free_page_index[DESINGED_MAX_SUPPORT_ORDER]数组的有效条目
     */
    bsp_kout<< "[FreePagesAllocator::free_pages_in_seg_control_block Info]" << kendl;
    bsp_kout<< "Base Address: 0x";
    bsp_kout.shift_hex();  // 切换到十六进制模式
    bsp_kout<< base << kendl;
    bsp_kout.shift_dec();  // 切换回十进制模式
    bsp_kout<< "Max Support Order: ";
    bsp_kout<< (uint32_t)max_supprt_order << kendl;
    bsp_kout<< "Suggest Order Free Page Index Cache:" << kendl;
    for(uint8_t i = 0; i <=max_supprt_order; i++){
        bsp_kout<<"order "<<i<< " free_count:"<<statistics.free_count[i]<<kendl;
    }
    for (uint8_t i = 0; i <=max_supprt_order; i++) {
        bool any=false;
        for (uint8_t j = 0; j < PER_ORDER_CACHE_SUGGEST_COUNT; ++j) {
            if (suggest_order_free_page_index[i][j] != INVALID_INBCB_INDEX) {
                if (!any) {
                    bsp_kout<< "  [" << (uint32_t)i << "] = ";
                    any = true;
                }
                bsp_kout<< suggest_order_free_page_index[i][j] << " ";
            }
        }
        if (any) {
            bsp_kout<< kendl;
        }
        
    }

}
void FreePagesAllocator::BuddyControlBlock::print_basic_info()
{
    print_basic_info_no_lock();
}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_info_no_lock()
{
    bsp_kout<< "[Bitmap Info for all Orders]" << kendl;
    
    for (uint8_t order = 0; order < max_supprt_order; order++) {
        print_bitmap_order_info_compress_no_lock(order);
    }
}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_info()
{
    print_bitmap_info_no_lock();
}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_info_compress_no_lock(uint8_t order)
{
    print_bitmap_order_interval_compress_no_lock(order, 0, 1ULL << (max_supprt_order - order));
}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_info_compress(uint8_t order)
{
    print_bitmap_order_info_compress_no_lock(order);
}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_interval_compress_no_lock(uint8_t order, uint64_t base, uint64_t length)
{
    bsp_kout<< "[Bitmap Order " << (uint32_t)order << " Interval Compress Print]" << kendl;
    
    if (order >= max_supprt_order) {
        bsp_kout<< "Invalid order!" << kendl;
        return;
    }
    
    uint64_t max_count = 1 << (max_supprt_order - order);
    
    // 确保base和length不超过有效范围
    if (base >= max_count) {
        bsp_kout<< "Base index out of range!" << kendl;
        return;
    }
    
    // 计算实际扫描长度
    uint64_t actual_length = length;
    if (base + length > max_count) {
        actual_length = max_count - base;
    }
    
    uint64_t i = base;
    bool scanning = false;  // 是否正在扫描连续区间
    bool current_state = false;  // 当前状态：true表示空闲页存在(1)，false表示不存在(0)
    uint64_t interval_start = 0;  // 当前区间的起始位置
    
    while (i < base + actual_length) {
        bool value = order_freepage_existency_bitmaps->bit_get(order_bases[order] + i); // 为1代表空闲页存在，为0代表不存在
        
        if (!scanning) {
            // 开始新的扫描
            scanning = true;
            current_state = value;
            interval_start = i;
        } else if (value != current_state) {
            // 状态发生变化，输出上一个区间
            bsp_kout<< "[" << interval_start << "," << i-1 << "]=";
            if (current_state) {
                bsp_kout<< "EXIST" << kendl;
            } else {
                bsp_kout<< "NOT_EXIST" << kendl;
            }
            
            // 更新状态，开始新区间
            current_state = value;
            interval_start = i;
        }
        
        i++;
    }
    
    // 输出最后一个区间
    if (scanning) {
        bsp_kout<< "[" << interval_start << "," << i-1 << "]=";
        if (current_state) {
            bsp_kout<< "EXIST" << kendl;
        } else {
            bsp_kout<< "NOT_EXIST" << kendl;
        }
    }
}
void FreePagesAllocator::BuddyControlBlock::print_bitmap_order_interval_compress(uint8_t order, uint64_t base, uint64_t length)
{
    print_bitmap_order_interval_compress_no_lock(order, base, length);
}

void FreePagesAllocator::BuddyControlBlock::print_all_statistics()
{
    
    bsp_kout << kendl;
    bsp_kout << "========================================" << kendl;
    bsp_kout << "[BuddyControlBlock Full Statistics Report]" << kendl;
    bsp_kout << "========================================" << kendl;
    
    // 基本信息
    bsp_kout << "[Basic Information]" << kendl;
    bsp_kout << "Base Address: 0x";
    bsp_kout.shift_hex();
    bsp_kout << base << kendl;
    bsp_kout.shift_dec();
    bsp_kout << "Max Support Order: " << (uint32_t)max_supprt_order << kendl;
    bsp_kout << "Is Split Bitmap Valid: " << (is_splited_bitmap_valid ? "Yes" : "No") << kendl;
    bsp_kout << kendl;
    
    // 各阶空闲页统计
    bsp_kout << "[Free Page Count per Order]" << kendl;
    for (uint8_t i = 0; i <= max_supprt_order; i++) {
        bsp_kout << "Order " << (uint32_t)i << ": " << statistics.free_count[i] << " pages" << kendl;
    }
    bsp_kout << kendl;
    
    // 缓存命中率统计（使用整数百分比）
    bsp_kout << "[Cache Hit/Miss Statistics]" << kendl;
    for (uint8_t i = 0; i <= max_supprt_order; i++) {
        uint64_t total = statistics.suggest_hit[i] + statistics.suggest_miss[i];
        uint64_t hit_rate_percent = (total > 0) ? 
            (statistics.suggest_hit[i] * 100 / total) : 0;
        bsp_kout << "Order " << (uint32_t)i << ": Hit=" << statistics.suggest_hit[i] 
                 << ", Miss=" << statistics.suggest_miss[i]
                 << ", Hit Rate=" << hit_rate_percent << "%" << kendl;
    }
    bsp_kout << kendl;
    
    // 分配/释放操作统计
    bsp_kout << "[Allocation/Deallocation Operations]" << kendl;
    bsp_kout << "Successful Allocations: " << statistics.alloc_times_success << kendl;
    bsp_kout << "Failed Allocations: " << statistics.alloc_times_fail << kendl;
    bsp_kout << "Successful Deallocation: " << statistics.free_times_success << kendl;
    
    uint64_t total_alloc = statistics.alloc_times_success + statistics.alloc_times_fail;
    uint64_t alloc_success_rate_percent = (total_alloc > 0) ? 
        (statistics.alloc_times_success * 100 / total_alloc) : 0;
    bsp_kout << "Allocation Success Rate: " << alloc_success_rate_percent << "%" << kendl;
    bsp_kout << kendl;
    
    // 扫描和折叠统计
    bsp_kout << "[Scan and Fold Statistics]" << kendl;
    bsp_kout << "Total Scan Count: " << statistics.scan_count << kendl;
    bsp_kout << "Successful Fold Count: " << statistics.fold_count_success << kendl;
    bsp_kout << "Failed Fold Count: " << statistics.fold_count_fail << kendl;
    
    uint64_t total_fold = statistics.fold_count_success + statistics.fold_count_fail;
    uint64_t fold_success_rate_percent = (total_fold > 0) ? 
        (statistics.fold_count_success * 100 / total_fold) : 0;
    bsp_kout << "Fold Success Rate: " << fold_success_rate_percent << "%" << kendl;
    bsp_kout << kendl;
    
    // 分裂操作统计
    bsp_kout << "[Page Split Statistics]" << kendl;
    bsp_kout << "Total Split Count: " << statistics.split_count << kendl;
    bsp_kout << kendl;
    
    // 缓存条目详情
    bsp_kout << "[Suggest Order Cache Details]" << kendl;
    for (uint8_t i = 0; i <= max_supprt_order; i++) {
        bool any = false;
        bsp_kout << "Order " << (uint32_t)i << " cache entries: ";
        for (uint8_t j = 0; j < PER_ORDER_CACHE_SUGGEST_COUNT; ++j) {
            if (suggest_order_free_page_index[i][j] != INVALID_INBCB_INDEX) {
                if (!any) {
                    any = true;
                } else {
                    bsp_kout << ", ";
                }
                bsp_kout << suggest_order_free_page_index[i][j];
            }
        }
        if (!any) {
            bsp_kout << "(empty)";
        }
        bsp_kout << kendl;
    }
    bsp_kout << kendl;
    
    // 性能汇总
    bsp_kout << "[Performance Summary]" << kendl;
    uint64_t total_ops = statistics.alloc_times_success + statistics.alloc_times_fail + statistics.free_times_success;
    bsp_kout << "Total Operations: " << total_ops << kendl;
    if (total_ops > 0) {
        uint64_t avg_scan_per_op_int = statistics.scan_count / total_ops;
        uint64_t avg_scan_per_op_fraction = (statistics.scan_count % total_ops * 100) / total_ops;
        bsp_kout << "Average Scans per Operation: " << avg_scan_per_op_int 
                 << "." << avg_scan_per_op_fraction << kendl;
    }
    bsp_kout << "========================================" << kendl;
    bsp_kout << kendl;
}

KURD_t FreePagesAllocator::BuddyControlBlock::free_pages_flush()
{
    KURD_t success=default_success();
    KURD_t fatal=default_fatal();
    success.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FLUSH_FREE_COUNT;
    fatal.event_code=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::EVENT_CODE_FLUSH_FREE_COUNT;
    for(uint8_t i=0;i<=max_supprt_order;i++){
        uint64_t actual_free_count=0;
        for(uint64_t j=0;j<(1ULL<<(max_supprt_order-i));j++){
            if(order_freepage_existency_bitmaps->bit_get(order_bases[i]+j))
                actual_free_count++;
        }
        if(actual_free_count!=statistics.free_count[i]){
            bsp_kout<< "FreePagesAllocator::free_pages_in_seg_control_block::free_pages_flush() violation" << kendl;
            bsp_kout<< "Order " << (uint32_t)i << ": Expected free count: " << statistics.free_count[i] << ", Actual free count: " << actual_free_count << kendl;
            //statistics.free_count[i]=actual_free_count;
            fatal.reason=MEMMODULE_LOCAIONS::FREEPAGES_ALLOCATOR_BUDDY_CONTROL_BLOCK_EVENTS_CODES::FLUSH_FREE_COUNT_RESULTS_CODE::FATAL_REASONS_CODE::COSISTENCY_VIOLATION;
            //Panic::panic(default_panic_behaviors_flags,nullptr,nullptr,nullptr,fatal);
            break;
        }
    }
    if(fatal.reason)return fatal;
    return success;
}

void FreePagesAllocator::print_all_bcb_statistics()
{
    bsp_kout << kendl;
    bsp_kout << "============================================" << kendl;
    bsp_kout << "[FreePagesAllocator - All BCB Statistics]" << kendl;
    bsp_kout << "============================================" << kendl;

    if (BCBS == nullptr || BCB_count == 0) {
        bsp_kout << "[INFO] BCBS is empty or not initialized, skipping..." << kendl;
        bsp_kout << "============================================" << kendl;
        bsp_kout << "[All BCB Statistics Print Complete]" << kendl;
        bsp_kout << "============================================" << kendl;
        bsp_kout << kendl;
        return;
    }

    bsp_kout << kendl;
    bsp_kout << ">>> [BCBS Array Statistics] <<<" << kendl;
    bsp_kout << "Total BCB count: " << BCB_count << kendl;
    for (uint64_t i = 0; i < BCB_count; ++i) {
        bsp_kout << kendl;
        bsp_kout << "--- BCBS[" << i << "] ---" << kendl;
        BCBS[i].print_all_statistics();
    }

    bsp_kout << "============================================" << kendl;
    bsp_kout << "[All BCB Statistics Print Complete]" << kendl;
    bsp_kout << "============================================" << kendl;
    bsp_kout << kendl;
}
