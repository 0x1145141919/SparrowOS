#pragma once
#include <stdint.h>
#include "memory/memory_base.h"
#include "elf.h"

// 用 kIMG_self_window（mem_init.cpp 全局）替代内部的 KImage/Kbss
// 初始化后 sg_bss_phdr 缓存 BSS 段程序头指针
void self_introspection_init();
extern "C" phyaddr_t get_phyaddr_for_Kbss(vaddr_t vaddr);
