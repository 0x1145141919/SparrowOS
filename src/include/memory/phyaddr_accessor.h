#pragma once
#include "AddresSpace.h"
#include "all_pages_arr.h"
constexpr uint8_t CACHE_VMDESC_MAX=16;
class PhyAddrAccessor { 
    friend KspacePageTable;
    friend all_pages_arr;
    static phyaddr_t init_pgtb_root;
    static VM_DESC BASIC_DESC;
    static VM_DESC cache_tb[CACHE_VMDESC_MAX];
    static bool is_init_cr3();
    public:
    #ifdef USER_MODE
    PhyAddrAccessor();
    #endif
    static uint8_t readu8(phyaddr_t addr);
    static uint16_t readu16(phyaddr_t addr);
    static uint32_t readu32(phyaddr_t addr);
    static uint64_t readu64(phyaddr_t addr);
    static void cache_flush(phyaddr_t addr);
    static void cache_flush_serial(phyaddr_t addr);
    static void writeu8(phyaddr_t addr,uint8_t value);
    static void writeu16(phyaddr_t addr,uint16_t value);
    static void writeu32(phyaddr_t addr,uint32_t value);
    static void writeu64(phyaddr_t addr,uint64_t value);
}; 
#ifdef USER_MODE
    extern PhyAddrAccessor gAccessor;
#endif