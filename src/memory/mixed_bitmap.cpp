// mixed_bitmap_v2 已从 FreePagesAllocator::BuddyControlBlock 中移除
// 替换为 BuddyControlBlock_foundation (3×2^N bits, 4-state nodes)
// kpoolmemmgr_t 仍保有独立的 mixed_bitmap_v2 实现 (在 kpoolmemmgr_HCBv3.cpp)
