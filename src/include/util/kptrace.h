#pragma once
#include <stdint.h>
#include "abi/boot.h"
#include "memory/memory_base.h"
struct symbol_entry {
    uint64_t address;
    char name[119];
    uint8_t type;
};
class ksymmanager {//被映射的表项是只读的，写操作会触发页错误，当然是在非初始化页表下的时候
    static phyaddr_t phybase;
    static vaddr_t virtbase;
    static symbol_entry *symbol_table;//虚拟地址
    static uint32_t entry_count;
    static uint32_t entry_size;
    public:
    static int Init(vm_interval* entry,uint64_t file_size);
    static symbol_entry*get_entry_near_addr(vaddr_t addr);//找到引索为n的符号使(symbol_table[n].address<=addr)&&(symbol_table[n+1].address>addr)
    //保证地址随引索的增加而增加，使用二分查找
    static const symbol_entry* get_table() { return symbol_table; }
    static phyaddr_t get_phybase();
    static vaddr_t get_virtbase();
    static uint32_t get_entry_count();
};
void self_trace();
void else_trace(void* rbp);
bool kptrace_current_stack_has_kthread_entry();
