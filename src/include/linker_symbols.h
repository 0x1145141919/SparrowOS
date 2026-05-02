#ifndef LINKER_SYMBOLS_H
#define LINKER_SYMBOLS_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 内核内存布局常量
extern const uint64_t __KImgSize;
extern const uint64_t PAGE_LEVEL_5;
extern const uint64_t PAGE_LEVEL_4;
extern const uint64_t init_sec_base;
extern const uint64_t init_sec_size;
extern const uint64_t KImgphybase;
extern const uint64_t KImgvbase;
extern const uint64_t Koffset;

// AP 引导段符号（初始化代码）
extern uint8_t ap_bootstrap_begin;
extern uint8_t ap_bootstrap_end;
extern uint8_t ap_bootstrap_text_begin;
extern uint8_t ap_bootstrap_text_end;
extern uint8_t ap_bootstrap_rodata_begin;
extern uint8_t ap_bootstrap_rodata_end;
extern uint8_t ap_bootstrap_data_begin;
extern uint8_t ap_bootstrap_data_end;
extern uint8_t ap_bootstrap_stack_begin;
extern uint8_t ap_bootstrap_stack_end;
// 主文本段符号
extern uint8_t text_begin;
extern uint8_t text_end;

// 数据段符号
extern uint8_t _data_start;
extern uint8_t _data_end;

// 只读数据段符号
extern uint8_t _rodata_start;
extern uint8_t _rodata_end;

// BSS 段符号
extern uint8_t _bss_begin;
extern uint8_t _bss_end;
extern uint8_t base_kernel_address;
// 获取地址的宏定义，方便使用
#define GET_SYMBOL_ADDR(symbol) ((void*)&symbol)

#ifdef __cplusplus
}
#endif

#endif // LINKER_SYMBOLS_H